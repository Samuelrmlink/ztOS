// SPDX-License-Identifier: GPL-2.0-or-later
#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "ztbt_proto.h"

namespace ztbt {

constexpr int kReconnectMs = 2000;

int64_t NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

Conn::Conn(std::string path) : path_(std::move(path)) {}

Conn::~Conn() { Stop(); }

void Conn::OnEvent(uint16_t op, const char* sig, Event fn) {
  events_[op] = EventHandler{sig, std::move(fn)};
}

void Conn::Start(Fn thread_start, Fn thread_end) {
  if (thread_.joinable()) return;
  thread_start_ = std::move(thread_start);
  thread_end_ = std::move(thread_end);
  stop_ = false;
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  thread_ = std::thread([this] { Loop(); });
}

void Conn::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> l(mu_);
    stop_ = true;
  }
  Wake();
  if (std::this_thread::get_id() == thread_.get_id()) {
    thread_.detach();
  } else {
    thread_.join();
  }
  if (wake_fd_ >= 0) close(wake_fd_);
  wake_fd_ = -1;
}

void Conn::Wake() {
  uint64_t one = 1;
  if (wake_fd_ >= 0) (void)!write(wake_fd_, &one, sizeof one);
}

void Conn::Post(Fn fn) {
  {
    std::lock_guard<std::mutex> l(mu_);
    posted_.push_back(std::move(fn));
  }
  Wake();
}

uint64_t Conn::After(int ms, Fn fn) {
  uint64_t id;
  {
    std::lock_guard<std::mutex> l(mu_);
    id = next_timer_++;
    timers_.emplace(NowMs() + ms, std::make_pair(id, std::move(fn)));
  }
  Wake();
  return id;
}

void Conn::Cancel(uint64_t timer) {
  std::lock_guard<std::mutex> l(mu_);
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->second.first == timer) {
      timers_.erase(it);
      return;
    }
  }
}

void Conn::Request(uint16_t op, const Values& args, const char* reply_sig, Reply cb) {
  std::string packet;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(mu_);
    id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
  }
  if (!Encode(op, id, args, &packet)) {
    LOGE("request 0x%04x: can't encode", op);
    Post([cb] { cb(ZTBT_ST_INVALID, {}); });
    return;
  }
  {
    std::lock_guard<std::mutex> l(mu_);
    if (fd_ < 0) {
      posted_.push_back([cb] { cb(ZTBT_ST_NOT_READY, {}); });
      Wake();
      return;
    }
    pending_[id] = Pending{reply_sig ? reply_sig : "", std::move(cb)};
  }
  bool sent;
  {
    std::lock_guard<std::mutex> l(send_mu_);
    int fd = fd_;
    sent = fd >= 0 && send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) == (ssize_t)packet.size();
  }
  if (!sent) {
    Reply failed;
    {
      std::lock_guard<std::mutex> l(mu_);
      auto it = pending_.find(id);
      if (it != pending_.end()) {
        failed = std::move(it->second.cb);
        pending_.erase(it);
      }
    }
    if (failed) Post([failed] { failed(ZTBT_ST_NOT_READY, {}); });
  }
}

bool Conn::Call(uint16_t op, const Values& args, const char* reply_sig, uint32_t* status,
                Values* out, int timeout_ms) {
  if (InLoop()) return false;
  struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    uint32_t status = ZTBT_ST_TIMEOUT;
    Values values;
  };
  auto sh = std::make_shared<Shared>();
  Request(op, args, reply_sig, [sh](uint32_t st, const Values& v) {
    std::lock_guard<std::mutex> l(sh->m);
    sh->done = true;
    sh->status = st;
    sh->values = v;
    sh->cv.notify_all();
  });
  std::unique_lock<std::mutex> l(sh->m);
  sh->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return sh->done; });
  *status = sh->status;
  if (out) *out = sh->values;
  return sh->done;
}

bool Conn::TryConnect() {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  if (path_.size() >= sizeof sa.sun_path) {
    close(fd);
    return false;
  }
  memcpy(sa.sun_path, path_.data(), path_.size());
  if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
    close(fd);
    return false;
  }
  struct timeval tv = {2, 0};   // a stuck bridge must not hang Android's binder threads for long
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  {
    std::lock_guard<std::mutex> l(mu_);
    fd_ = fd;
  }
  LOGI("connected to %s", path_.c_str());
  if (on_connected_) on_connected_();
  return true;
}

void Conn::Drop() {
  std::map<uint32_t, Pending> failed;
  {
    std::lock_guard<std::mutex> l(send_mu_);
    std::lock_guard<std::mutex> l2(mu_);
    if (fd_ < 0) return;
    close(fd_);
    fd_ = -1;
    failed.swap(pending_);
  }
  LOGW("bridge connection lost");
  for (auto& p : failed) p.second.cb(ZTBT_ST_NOT_READY, {});
  if (on_disconnected_) on_disconnected_();
}

void Conn::Dispatch(const uint8_t* p, size_t n) {
  uint16_t op;
  uint32_t id;
  Values vals;
  if (!Decode(p, n, &op, &id, &vals)) {
    LOGE("bad packet from the bridge (%zu bytes)", n);
    return;
  }
  if (op == ZTBT_OP_REPLY) {
    Pending pend;
    {
      std::lock_guard<std::mutex> l(mu_);
      auto it = pending_.find(id);
      if (it == pending_.end()) return;
      pend = std::move(it->second);
      pending_.erase(it);
    }
    if (vals.empty() || vals[0].t != 'u') {
      pend.cb(ZTBT_ST_FAIL, {});
      return;
    }
    uint32_t st = vals[0].num();
    Values rest(vals.begin() + 1, vals.end());
    const char* sig = st == ZTBT_ST_OK ? pend.sig.c_str() : (st == ZTBT_ST_ATT ? "y" : "");
    if (!Matches(sig, rest)) {
      LOGE("reply %u: values don't fit \"%s\"", id, sig);
      pend.cb(ZTBT_ST_FAIL, {});
      return;
    }
    pend.cb(st, rest);
    return;
  }
  auto it = events_.find(op);
  if (it == events_.end()) return;   // a newer bridge's event: ignore
  if (!Matches(it->second.sig.c_str(), vals)) {
    LOGE("event 0x%04x: values don't fit", op);
    return;
  }
  it->second.fn(vals);
}

int64_t Conn::RunTimers() {
  for (;;) {
    Fn fn;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (timers_.empty()) return -1;
      auto it = timers_.begin();
      int64_t wait = it->first - NowMs();
      if (wait > 0) return wait;
      fn = std::move(it->second.second);
      timers_.erase(it);
    }
    fn();
  }
}

void Conn::Loop() {
  loop_id_ = std::this_thread::get_id();
  if (thread_start_) thread_start_();
  int64_t next_try = 0;
  std::vector<uint8_t> buf(kMaxPacket);
  for (;;) {
    std::vector<Fn> work;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (stop_) break;
      work.swap(posted_);
    }
    for (auto& fn : work) fn();

    if (fd_ < 0 && NowMs() >= next_try) {
      if (!TryConnect()) next_try = NowMs() + kReconnectMs;
    }
    int64_t wait = RunTimers();
    if (fd_ < 0) {
      int64_t until = next_try - NowMs();
      if (wait < 0 || until < wait) wait = until < 0 ? 0 : until;
    }
    {
      std::lock_guard<std::mutex> l(mu_);
      if (!posted_.empty() || stop_) wait = 0;
    }
    struct pollfd pfd[2] = {{wake_fd_, POLLIN, 0}, {fd_, POLLIN, 0}};
    int n = poll(pfd, fd_ >= 0 ? 2 : 1, wait < 0 ? -1 : (int)wait);
    if (n < 0 && errno != EINTR) break;
    if (pfd[0].revents & POLLIN) {
      uint64_t v;
      (void)!read(wake_fd_, &v, sizeof v);
    }
    if (fd_ >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t got = recv(fd_, buf.data(), buf.size(), MSG_DONTWAIT);
      if (got > 0) {
        Dispatch(buf.data(), (size_t)got);
      } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
        Drop();
        next_try = NowMs() + kReconnectMs;
      }
    }
  }
  Drop();
  if (thread_end_) thread_end_();
}

}  // namespace ztbt
