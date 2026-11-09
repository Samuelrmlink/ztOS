// SPDX-License-Identifier: GPL-2.0-or-later
// The shim's connection to ztos-btbridge, and the one thread everything
// Android hears comes from. Android 13's JNI accepts callbacks only on the
// thread that announced itself with ASSOCIATE_JVM, so replies, events,
// timers and posted work all run on this loop thread.
#pragma once

#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wire.h"

namespace ztbt {

class Conn {
 public:
  using Reply = std::function<void(uint32_t status, const Values& values)>;
  using Event = std::function<void(const Values& values)>;
  using Fn = std::function<void()>;

  explicit Conn(std::string path);
  ~Conn();

  // Handlers, set before Start(). Called on the loop thread.
  void OnEvent(uint16_t op, const char* sig, Event fn);
  void OnConnected(Fn fn) { on_connected_ = std::move(fn); }
  void OnDisconnected(Fn fn) { on_disconnected_ = std::move(fn); }

  // Start the loop thread; thread_start/thread_end run on it first and last.
  void Start(Fn thread_start, Fn thread_end);
  void Stop();

  // From any thread. The reply comes on the loop thread; values are checked
  // against reply_sig (an ATT status brings one 'y'). Not connected: the
  // reply is ZTBT_ST_NOT_READY, posted.
  void Request(uint16_t op, const Values& args, const char* reply_sig, Reply cb);
  // Wait for the reply (never on the loop thread: false at once there).
  bool Call(uint16_t op, const Values& args, const char* reply_sig, uint32_t* status,
            Values* out, int timeout_ms = 3000);
  void Post(Fn fn);
  uint64_t After(int ms, Fn fn);
  void Cancel(uint64_t timer);

  bool connected() const { return fd_ >= 0; }
  bool InLoop() const { return std::this_thread::get_id() == loop_id_; }

 private:
  struct Pending {
    std::string sig;
    Reply cb;
  };
  struct EventHandler {
    std::string sig;
    Event fn;
  };
  void Loop();
  bool TryConnect();
  void Drop();
  void Dispatch(const uint8_t* p, size_t n);
  void Wake();
  int64_t RunTimers();   // ms until the next timer, or -1

  std::string path_;
  int fd_ = -1;
  int wake_fd_ = -1;
  bool stop_ = false;
  std::thread thread_;
  std::thread::id loop_id_;
  Fn thread_start_, thread_end_, on_connected_, on_disconnected_;

  std::mutex mu_;   // guards the members below
  uint32_t next_id_ = 1;
  std::map<uint32_t, Pending> pending_;
  std::vector<Fn> posted_;
  uint64_t next_timer_ = 1;
  std::multimap<int64_t, std::pair<uint64_t, Fn>> timers_;
  std::mutex send_mu_;
  std::map<uint16_t, EventHandler> events_;
};

int64_t NowMs();

}  // namespace ztbt
