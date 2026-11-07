#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ztos-ntfyd whole, against a pretend ntfy server on 127.0.0.1 and a pretend notification
service on a private bus. Everything is in a temporary directory: runtime dir, home, bus. It
opens no window and tells the running session nothing.

When it can, it runs itself in network and mount namespaces of its own (unshare -rnm): no way
out to any real server at all, a loopback it can add addresses to, to see a network change,
and a list of trusted certificates that only it sees, to see https work. Nothing outside
changes.

    make check-ntfyd     (needs ntfy/ztos-ntfyd, ntfy/ztntfy, notifyd/ztnotifyd, dbus-python, PyGObject)
"""
import json
import os
import shutil
import signal
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# (ZTOS_NTFYD_TEST_BIN: another build of it, one with sanitizers say)
DAEMON = os.environ.get("ZTOS_NTFYD_TEST_BIN") or os.path.realpath(os.path.join(HERE, "..", "ntfy", "ztos-ntfyd"))
ZTNOTIFYD = os.path.realpath(os.path.join(HERE, "..", "notifyd", "ztnotifyd"))
IN_NS = os.environ.get("ZTOS_NTFYD_TEST_NS") == "1"

if not IN_NS and shutil.which("unshare") and shutil.which("ip"):
    # again, inside (if that's allowed here)
    if subprocess.run(["unshare", "-rnm", "true"], stderr=subprocess.DEVNULL).returncode == 0:
        env = dict(os.environ, ZTOS_NTFYD_TEST_NS="1")
        os.execvpe("unshare", ["unshare", "-rnm", sys.executable, os.path.abspath(__file__)], env)
if IN_NS:
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

TOKEN = "tk_secret_token_value_1234"
PASSWORD = "hunter2 with space"
TOPIC_SECRET = "zt-secret-topic-name"
failures = []


def check(what, ok, more=""):
    print(("ok   " if ok else "FAIL ") + what + (f"  [{more}]" if more and not ok else ""), flush=True)
    if not ok:
        failures.append(what)


def wait_for(cond, timeout=8.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.02)
    return cond()


# ---- the pretend server ------------------------------------------------------------

class Stream:
    """One open GET, for the test to write lines down."""

    def __init__(self, conn, path, query, headers):
        self.conn, self.path, self.query, self.headers = conn, path, query, headers
        self.closed = False

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8", "surrogatepass")
        # (chunked, as a real server's streaming answer is)
        try:
            self.conn.sendall(b"%x\r\n" % len(data) + data + b"\r\n")
            return True
        except OSError:
            return False

    def line(self, obj):
        return self.send(json.dumps(obj) + "\n")

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.conn.close()


class Server:
    def __init__(self, tls_context=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.tls = tls_context
        self.streams, self.posts, self.accepted = [], [], 0
        self.answer = None      # a canned answer instead of a stream ("401 Unauthorized")
        self.tls_failures = 0
        self.lock = threading.Lock()
        threading.Thread(target=self.run, daemon=True).start()

    @property
    def url(self):
        return f"{'https' if self.tls else 'http'}://127.0.0.1:{self.port}"

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with self.lock:
                self.accepted += 1
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        if self.tls:
            try:
                conn = self.tls.wrap_socket(conn, server_side=True)
            except (ssl.SSLError, OSError):
                self.tls_failures += 1
                conn.close()
                return
        data = b""
        try:
            while b"\r\n\r\n" not in data:
                got = conn.recv(4096)
                if not got:
                    conn.close()
                    return
                data += got
        except OSError:
            return
        head, _, body = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        method, target, _ = lines[0].split(" ", 2)
        headers = {}
        for h in lines[1:]:
            k, _, v = h.partition(":")
            headers[k.strip().lower()] = v.strip()
        path, _, query = target.partition("?")
        if method == "POST":
            want = int(headers.get("content-length", "0"))
            while len(body) < want:
                body += conn.recv(4096)
            self.posts.append((path, headers, body))
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}")
            conn.close()
            return
        s = Stream(conn, path, query, headers)
        if self.answer:
            conn.sendall(f"HTTP/1.1 {self.answer}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".encode())
            with self.lock:
                self.streams.append(s)
            conn.close()
            return
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: chunked\r\n\r\n")
        with self.lock:
            self.streams.append(s)

    def stream_count(self):
        with self.lock:
            return len(self.streams)

    def wait_stream(self, n, timeout=8.0):
        if wait_for(lambda: self.stream_count() >= n, timeout):
            return self.streams[n - 1]
        return None


def msg(mid, text, topic="phone", **kw):
    return dict({"id": mid, "time": int(time.time()), "event": "message", "topic": topic, "message": text}, **kw)


def opened(topic="phone"):
    return {"id": "open0", "time": int(time.time()), "event": "open", "topic": topic}


# ---- the pretend notification service ------------------------------------------------

class FakeNotifications(dbus.service.Object):
    """On a connection of its own, so that stopping it is a service going away."""

    def __init__(self):
        self.got = []
        self.conn = dbus.bus.BusConnection(bus_addr)
        self.name = dbus.service.BusName("org.freedesktop.Notifications", self.conn, do_not_queue=True)
        super().__init__(self.conn, "/org/freedesktop/Notifications")

    @dbus.service.method("org.freedesktop.Notifications", in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        self.got.append(dict(app=str(app), summary=str(summary), body=str(body), icon=str(icon),
                             actions=[str(a) for a in actions],
                             quiet=bool(hints.get("suppress-sound", False)), urgency=int(hints.get("urgency", -1)),
                             entry=str(hints.get("desktop-entry", ""))))
        return len(self.got)

    def stop(self):
        self.remove_from_connection()
        self.name = None    # (gives the name back while the connection can)
        self.conn.close()


# ---- one run of the daemon --------------------------------------------------------------

root = tempfile.mkdtemp(prefix="ztos-ntfyd-test.")
home = os.path.join(root, "home")
runtime = os.path.join(root, "run")
os.makedirs(home)
os.makedirs(runtime, mode=0o700)
conf_dir = os.path.join(home, ".config", "ztmetro", "ntfy")
conf_path = os.path.join(conf_dir, "subscriptions")
state_path = os.path.join(home, ".local", "state", "ztmetro", "ntfy-state")
status_path = os.path.join(runtime, "ztos-ntfyd.status")
log_path = os.path.join(root, "daemon.log")

bus_proc = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1",
                             f"--address=unix:path={root}/bus"], stdout=subprocess.PIPE, text=True)
bus_addr = bus_proc.stdout.readline().strip()
ENV = dict(os.environ, HOME=home, XDG_RUNTIME_DIR=runtime, DBUS_SESSION_BUS_ADDRESS=bus_addr, ZTOS_NTFYD_TEST_FAST="1")
ENV.pop("DISPLAY", None)
ENV.pop("WAYLAND_DISPLAY", None)

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
main_conn = dbus.bus.BusConnection(bus_addr)
loop = GLib.MainLoop()
threading.Thread(target=loop.run, daemon=True).start()


def write_conf(*lines, mode=0o600):
    """The file as ztntfy writes it: a new one renamed over the old."""
    os.makedirs(conf_dir, mode=0o700, exist_ok=True)
    tmp = conf_path + ".new"
    with open(tmp, "w") as f:
        f.write("# test\n")
        for line in lines:
            f.write(line + "\n")
    os.chmod(tmp, mode)
    os.rename(tmp, conf_path)


def sub(sid, server, topic="phone", added=1700000000, user="", pw="", token=""):
    return "\t".join([sid, str(added), server, topic, user, pw, token])


class Daemon:
    def __init__(self, env=None):
        self.log = open(log_path, "ab")
        self.p = subprocess.Popen([DAEMON], env=dict(ENV, **(env or {})), stdout=self.log, stderr=self.log)

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
                check("it stops when told to", False)
        self.log.close()

    def kill(self):
        self.p.kill()
        self.p.wait()
        self.log.close()

    def wakes(self):
        total = 0
        for name in os.listdir(f"/proc/{self.p.pid}/task"):
            with open(f"/proc/{self.p.pid}/task/{name}/status") as f:
                for line in f:
                    if line.startswith(("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches")):
                        total += int(line.split()[1])
        return total

    def sockets(self):
        """How many sockets it holds."""
        n = 0
        for fd in os.listdir(f"/proc/{self.p.pid}/fd"):
            try:
                n += os.readlink(f"/proc/{self.p.pid}/fd/{fd}").startswith("socket:")
            except OSError:
                pass
        return n

    def rss_kb(self):
        with open(f"/proc/{self.p.pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS"):
                    return int(line.split()[1])
        return 0


def status_of(sid):
    try:
        with open(status_path) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if p[0] == sid:
                    return p
    except OSError:
        pass
    return None


def saved_id(sid):
    try:
        with open(state_path) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if p[0] == sid:
                    return p[1]
    except OSError:
        pass
    return None


def reset():
    """Nothing left from the part before."""
    shutil.rmtree(os.path.join(home, ".config"), ignore_errors=True)
    shutil.rmtree(os.path.join(home, ".local"), ignore_errors=True)


fake = FakeNotifications()


def test_no_topics():
    server = Server()
    d = Daemon()
    time.sleep(1.0)
    check("no topics: still running", d.p.poll() is None)
    check("no topics: holds no socket at all, not even to the bus", d.sockets() == 0, str(d.sockets()))
    before = d.wakes()
    time.sleep(4.0)
    woke = d.wakes() - before
    check("no topics: never wakes", woke == 0, f"{woke} wakes in 4 s")
    print(f"     (no topics: {d.rss_kb()} kB resident)")
    # a line that can't be read connects to nothing either
    write_conf("this is not a subscription", sub("bad", "ftp://127.0.0.1:1"), sub("bad2", "file:///etc/passwd"))
    time.sleep(0.6)
    check("a file of nonsense connects to nothing", d.sockets() == 0 and server.accepted == 0)
    log = open(log_path).read()
    check("and doesn't repeat it in the log", "not a subscription" not in log)
    check("the server was never called", server.accepted == 0)
    d.stop()
    reset()


def test_stream():
    server = Server()
    d = Daemon()
    fake.got.clear()
    # it notices the file, with no signal
    write_conf(sub("aaaa", server.url))
    s = server.wait_stream(1)
    check("a topic added while running connects", s is not None)
    if not s:
        d.stop()
        return
    check("asks for the right stream", s.path == "/phone/json", s.path)
    check("a topic with no history starts from when it was added", s.query == "since=1700000000", s.query)
    s.line(opened())
    check("state says connected", wait_for(lambda: (status_of("aaaa") or ["", ""])[1] == "connected"))

    # pieces of a line, keepalives, a message
    s.send('{"id":"m1","time":%d,"event":"mess' % time.time())
    time.sleep(0.05)
    s.send('age","topic":"phone","title":"Hello","message":"World","priority":4}\n')
    check("a message, in two pieces, arrives", wait_for(lambda: len(fake.got) == 1))
    if fake.got:
        g = fake.got[0]
        check("title, text and label", (g["summary"], g["body"], g["app"]) == ("Hello", "World", "phone"), str(g))
        check("priority 4 alerts", not g["quiet"] and g["urgency"] == 1, str(g))
        check("says whose it is", g["entry"] == "ztntfy" and g["icon"] == "ztntfy", str(g))
        check("offers no actions", g["actions"] == [], str(g))
    check("the saved id moves on", wait_for(lambda: saved_id("aaaa") == "m1"), str(saved_id("aaaa")))
    check("state file is private", stat.S_IMODE(os.stat(state_path).st_mode) == 0o600)

    # priorities
    fake.got.clear()
    for i, p in enumerate((1, 2, 3, 5)):
        s.line(msg(f"p{i}", f"pri{p}", priority=p))
        wait_for(lambda: len(fake.got) == i + 1)
    check("priority 1 and 2 are quiet, 3 and 5 aren't",
          [g["quiet"] for g in fake.got] == [True, True, False, False], str(fake.got))

    # duplicates on a live stream
    fake.got.clear()
    s.line(msg("p0", "again"))
    s.line(msg("dup", "first"))
    s.line(msg("dup", "second"))
    wait_for(lambda: len(fake.got) >= 1)
    time.sleep(0.3)
    check("a repeat is shown once", [g["body"] for g in fake.got] == ["first"], str(fake.got))

    # only data wakes it
    time.sleep(1.0)
    before = d.wakes()
    for _ in range(4):
        time.sleep(0.5)
        s.line({"id": "k", "time": int(time.time()), "event": "keepalive", "topic": "phone"})
    woke = d.wakes() - before
    # the socket's four keepalives, and the state file's flush at most
    check("idle with keepalives: a few wakes, not many", woke <= 20, f"{woke} wakes in 2 s")
    time.sleep(1.2)
    before = d.wakes()
    time.sleep(1.8)    # (under the quiet limit of 3 s)
    woke = d.wakes() - before
    check("silent: doesn't poll", woke <= 1, f"{woke} wakes in 1.8 s")

    # reconnect, with since=
    fake.got.clear()
    s.close()
    s2 = server.wait_stream(2)
    check("reconnects after the server hangs up", s2 is not None)
    if s2:
        check("comes back with since= the last id", s2.query == "since=dup", s2.query)
        s2.line(opened())
        # what was missed, and what was already shown, as a server might send them
        s2.line(msg("dup", "already shown"))
        s2.line(msg("off1", "while offline"))
        check("what was missed arrives", wait_for(lambda: len(fake.got) == 1))
        time.sleep(0.5)
        check("and what was already shown doesn't", [g["body"] for g in fake.got] == ["while offline"], str(fake.got))
    d.stop()
    check("stopping tidies the status file", not os.path.exists(status_path))

    # across a restart
    fake.got.clear()
    d = Daemon()
    s3 = server.wait_stream(3)
    check("after a restart: since= from the saved file", s3 is not None and s3.query == "since=off1", s3.query if s3 else "")
    if s3:
        s3.line(opened())
        s3.line(msg("off1", "shown before the restart"))
        s3.line(msg("new1", "after the restart"))
        wait_for(lambda: len(fake.got) >= 1)
        time.sleep(0.5)
        check("after a restart: no repeats", [g["body"] for g in fake.got] == ["after the restart"], str(fake.got))
    d.kill()
    reset()


def test_backlog():
    server = Server()
    fake.got.clear()
    write_conf(sub("bbbb", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    if not s:
        check("backlog: connects", False)
        d.stop()
        return
    s.line(opened())
    for i in range(200):
        pr = 5 if i == 77 else 2
        s.line(msg(f"b{i}", f"backlog {i}", priority=pr))
    check("a long backlog comes to one summary", wait_for(lambda: len(fake.got) >= 1))
    time.sleep(0.8)
    check("exactly one", len(fake.got) == 1, str(len(fake.got)))
    g = fake.got[0] if fake.got else {}
    check("that says how many", g.get("summary") == "200 new messages", str(g))
    check("with the most urgent in it, and alerting", g.get("body") == "backlog 77" and not g.get("quiet"), str(g))
    check("and the place moves past all of them", saved_id("bbbb") == "b199", str(saved_id("bbbb")))

    # a short one: each by itself
    fake.got.clear()
    s.close()
    s2 = server.wait_stream(2)
    if s2:
        check("back from where the backlog ended", s2.query == "since=b199", s2.query)
        s2.line(opened())
        for i in range(3):
            s2.line(msg(f"s{i}", f"short {i}"))
        wait_for(lambda: len(fake.got) >= 3)
        check("a few are shown one by one", [g["body"] for g in fake.got] == ["short 0", "short 1", "short 2"], str(fake.got))

        # then a flood on the live stream: bounded, and nothing vanishes without a count
        fake.got.clear()
        time.sleep(0.6)
        for i in range(60):
            s2.line(msg(f"f{i}", f"flood {i}"))
        time.sleep(3.0)
        count = len(fake.got)
        check("a live flood is held to a few alerts", 1 <= count <= 16, str(count))
        check("with what didn't fit in a summary", any("new messages" in g["summary"] for g in fake.got), str(fake.got))
        check("and the place is the newest", wait_for(lambda: saved_id("bbbb") == "f59"), str(saved_id("bbbb")))
    d.stop()
    reset()


def test_hostile():
    server = Server()
    fake.got.clear()
    write_conf(sub("cccc", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    if not s:
        check("hostile: connects", False)
        d.stop()
        return
    s.line(opened())
    time.sleep(0.5)
    before = server.accepted
    # none of these is a message that may be shown
    s.send("not json\n")
    s.send("{\n")
    s.send('{"event":"message","id":"x1","message":"cut\n')
    s.send("\n\n\n")
    s.send(b"\xff\xfe\x00\x01\n")
    s.send("[" * 60000 + "\n")
    s.line({"event": "message", "message": "no id"})
    s.line({"event": "message", "id": "a&b=c", "message": "id that would reach the next address"})
    s.line({"event": "message", "id": "a\r\nX: y", "message": "id with a header in it"})
    s.line({"event": "message", "id": "other", "topic": "another", "message": "someone else's topic"})
    s.line({"event": "poll_request", "id": "pr", "message": "not a message"})
    # far too long a line, and the stream goes on
    s.send('{"event":"message","id":"huge","message":"' + "x" * 300000 + '"}\n')
    s.send("y" * 5000000 + "\n")
    s.line(msg("after", "after all that"))
    check("after nonsense and oversized lines, the next message arrives", wait_for(lambda: len(fake.got) >= 1, 15))
    check("and nothing else was shown", [g["body"] for g in fake.got] == ["after all that"], str(fake.got))
    check("the connection held", server.stream_count() == 1, str(server.stream_count()))
    check("the running daemon is alive", d.p.poll() is None)

    # hostile text, and things to be left alone
    fake.got.clear()
    canary = Server()
    s.line(msg("evil", "a\x1b[2J\x07b‮⁦c<b>d</b>\nnext​",
               title="T\x00i\ttle‮",
               tags=["\x1b]0;x\x07", "skull", "cd"],
               click=canary.url + "/click",
               icon=canary.url + "/icon",
               actions=[{"action": "http", "label": "x", "url": canary.url + "/action", "clear": True},
                        {"action": "view", "label": "y", "url": canary.url + "/view"},
                        {"action": "broadcast", "label": "z"}],
               attachment={"name": "cat‮.png", "url": canary.url + "/attachment", "size": 1}))
    check("a hostile message arrives", wait_for(lambda: len(fake.got) >= 1))
    g = fake.got[0] if fake.got else {"body": "", "summary": ""}
    bad = [c for c in g["body"] + g["summary"] + g["app"] if ord(c) < 0x20 or 0x7f <= ord(c) <= 0x9f or 0x202a <= ord(c) <= 0x202e or 0x2066 <= ord(c) <= 0x2069 or ord(c) == 0x200b]
    check("no control or bidi characters in what's shown", not bad, repr(g))
    check("no markup in what's shown", "<" not in g["body"] and ">" not in g["body"], repr(g))
    check("the text itself survives", g["body"].startswith("a[2Jbc") and "next" in g["body"], repr(g))
    check("an attachment is a name", "attachment: cat.png" in g["body"] and "http" not in g["body"], repr(g))
    time.sleep(1.0)
    check("click, icon, action and attachment addresses are never fetched", canary.accepted == 0, str(canary.accepted))
    check("no actions offered", g.get("actions") == [], repr(g))

    # a time far in the future doesn't make everything after it look old
    fake.got.clear()
    s.line(msg("future", "from the future", time=99999999999))
    wait_for(lambda: len(fake.got) >= 1)
    s.line(msg("normal", "normal"))
    check("a time in the far future harms nothing", wait_for(lambda: len(fake.got) >= 2), str(fake.got))

    # a server that won't stop is hung up on, and not asked again soon
    n = server.stream_count()
    chunk = "z" * (1024 * 1024)
    for _ in range(40):
        if not s.send(chunk):
            break
    check("a firehose is hung up on", wait_for(lambda: "too much" in (status_of("cccc") or ["", "", "", ""])[3]), str(status_of("cccc")))
    check("the daemon survives it", d.p.poll() is None)
    with open(status_path) as f:
        check("and the status holds no topic or address", server.url not in f.read())

    # a state file that has been got at puts nothing in the address
    d.stop()
    with open(state_path, "w") as f:
        f.write("cccc\tgood&x=1\t1\t1\t\n")
    d = Daemon()
    s2 = server.wait_stream(2)
    check("a bad saved id isn't used", s2 is not None and s2.query == "since=1700000000", s2.query if s2 else "")
    d.stop()
    reset()


def test_service_absent():
    global fake
    server = Server()
    fake.stop()
    time.sleep(0.2)
    write_conf(sub("dddd", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    came_back = None
    try:
        if not s:
            check("no service: connects", False)
            return
        s.line(opened())
        s.line(msg("w1", "waiting one"))
        s.line(msg("w2", "waiting two"))
        time.sleep(1.5)
        check("with no notification service: the place doesn't move", saved_id("dddd") in (None, "-"), str(saved_id("dddd")))
        check("it isn't spinning", d.wakes() < 200, str(d.wakes()))
        # a reconnect meanwhile mustn't double what's waiting
        s.close()
        s2 = server.wait_stream(2)
        if s2:
            s2.line(opened())
            s2.line(msg("w1", "waiting one"))
            s2.line(msg("w2", "waiting two"))
        time.sleep(0.5)
        came_back = FakeNotifications()
        check("when the service comes, they arrive", wait_for(lambda: len(came_back.got) >= 2, 5), str(came_back.got if came_back else ""))
        time.sleep(0.5)
        check("once each, in order", [g["body"] for g in came_back.got] == ["waiting one", "waiting two"], str(came_back.got))
        check("and the place moves on", wait_for(lambda: saved_id("dddd") == "w2"), str(saved_id("dddd")))

        # shown only when taken: a kill with something unshown asks for it again
        came_back.stop()
        time.sleep(0.2)
        s3 = server.streams[-1]
        s3.line(msg("w3", "killed before shown"))
        time.sleep(0.8)
        d.kill()
        d = None
        check("a message not yet shown isn't marked as seen", saved_id("dddd") == "w2", str(saved_id("dddd")))
        with open(state_path) as f:
            check("nor among the saved ids", "w3" not in f.read())
    finally:
        if d:
            d.stop()
    reset()
    fake = FakeNotifications()


def test_errors_and_secrets():
    server = Server()
    open(log_path, "w").close()
    write_conf(sub("eeee", server.url, topic=TOPIC_SECRET, token=TOKEN),
               sub("ffff", server.url, topic="users", user="me", pw=PASSWORD.replace(" ", "%20")))
    d = Daemon()
    a = server.wait_stream(2)
    check("two topics, two streams", a is not None)
    if a:
        by_path = {x.path: x for x in server.streams}
        t = by_path.get(f"/{TOPIC_SECRET}/json")
        u = by_path.get("/users/json")
        check("the token goes as a header", t is not None and t.headers.get("authorization") == f"Bearer {TOKEN}",
              str(t.headers if t else ""))
        import base64
        want = base64.b64encode(f"me:{PASSWORD}".encode()).decode()
        check("the password as basic sign-in", u is not None and u.headers.get("authorization") == f"Basic {want}",
              str(u.headers if u else ""))
        if t:
            t.line(opened(TOPIC_SECRET))
        wait_for(lambda: (status_of("eeee") or ["", ""])[1] == "connected")
    # sends a test: the secrets from the file, and none on the command line
    code = subprocess.run([DAEMON, "--publish", "eeee"], env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
    check("--publish sends", code.returncode == 0 and len(server.posts) == 1, f"{code.returncode} {code.stdout}")
    if server.posts:
        path, headers, body = server.posts[0]
        check("to the topic, signed in", path == f"/{TOPIC_SECRET}" and headers.get("authorization") == f"Bearer {TOKEN}", path)
        check("with words in it", b"test" in body)
    check("--publish of nothing says so", subprocess.run([DAEMON, "--publish", "none"], env=ENV).returncode == 2)
    with open(f"/proc/{d.p.pid}/cmdline", "rb") as f:
        check("no secrets on the command line", TOKEN.encode() not in f.read())
    with open(f"/proc/{d.p.pid}/environ", "rb") as f:
        check("nor in the environment", TOKEN.encode() not in f.read())

    # a file others can read is closed
    os.chmod(conf_path, 0o644)
    with open(conf_path, "a") as f:
        f.write("# touch\n")
    check("a config file others can read is closed", wait_for(lambda: stat.S_IMODE(os.stat(conf_path).st_mode) == 0o600),
          oct(os.stat(conf_path).st_mode))

    # refused: says why, in our words, at the longest wait
    server2 = Server()
    server2.answer = "401 Unauthorized"
    write_conf(sub("gggg", server2.url, token=TOKEN))
    check("a refused sign-in is reported", wait_for(lambda: "sign-in" in ((status_of("gggg") or ["", "", "", ""])[3])), str(status_of("gggg")))
    n = server2.stream_count()
    time.sleep(0.5)
    check("and isn't hammered", server2.stream_count() - n <= 1, str(server2.stream_count() - n))
    # no redirects
    server3 = Server()
    canary = Server()
    server3.answer = f"302 Found\r\nLocation: {canary.url}/x"
    write_conf(sub("hhhh", server3.url, token=TOKEN))
    wait_for(lambda: server3.stream_count() >= 1)
    time.sleep(0.8)
    check("a redirect isn't followed", canary.accepted == 0, str(canary.accepted))
    check("a removed topic's stream goes", d.p.poll() is None)
    # topic removed: no more connections to it
    write_conf()
    time.sleep(0.5)
    n = server3.accepted
    check("the last topic gone: back to holding no sockets", wait_for(lambda: d.sockets() == 0), str(d.sockets()))
    before = d.wakes()
    time.sleep(2.0)
    check("and never waking", d.wakes() - before == 0, str(d.wakes() - before))
    check("nothing more goes to the server", server3.accepted == n)
    d.stop()

    log = open(log_path, "rb").read()
    check("the log never has a token", TOKEN.encode() not in log)
    check("the log never has a password", b"hunter2" not in log)
    check("the log never has a topic's name", TOPIC_SECRET.encode() not in log)
    with open(state_path, "rb") as f:
        state = f.read()
    check("the state file has no secrets", TOKEN.encode() not in state and TOPIC_SECRET.encode() not in state)
    reset()


def test_backoff_and_down():
    # nothing listening: tries are spaced out, and it doesn't spin
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    write_conf(sub("iiii", f"http://127.0.0.1:{port}"))
    d = Daemon()
    time.sleep(0.5)
    s0 = d.wakes()
    time.sleep(4.0)
    woke = d.wakes() - s0
    check("with the server down it doesn't spin", woke < 200, f"{woke} wakes in 4 s, with a 1.5 s cap on retries here")
    check("and says it can't reach it", "reach" in (status_of("iiii") or ["", "", "", ""])[3], str(status_of("iiii")))
    # up again at the same address
    server = None
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    sock.listen(4)
    sock.settimeout(5)
    try:
        conn, _ = sock.accept()
        check("and comes back when the server does", True)
        conn.close()
    except socket.timeout:
        check("and comes back when the server does", False)
    sock.close()
    d.stop()
    reset()

    # a stream that goes silent without closing is given up on
    server = Server()
    write_conf(sub("jjjj", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    if s:
        s.line(opened())
        t0 = time.monotonic()
        s2 = server.wait_stream(2, 8)
        took = time.monotonic() - t0
        check("a dead connection is noticed from its silence", s2 is not None and 2.5 <= took <= 6, f"{took:.1f} s")
    d.stop()
    reset()


def test_tls():
    openssl = shutil.which("openssl")
    if not openssl:
        print("SKIP the TLS part: no openssl program here")
        return
    key, cert = os.path.join(root, "k.pem"), os.path.join(root, "c.pem")
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "2",
                    "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    server = Server(ctx)
    fake.got.clear()
    write_conf(sub("kkkk", server.url, token=TOKEN))
    d = Daemon()
    check("a self-signed certificate is refused",
          wait_for(lambda: "certificate" in (status_of("kkkk") or ["", "", "", ""])[3]), str(status_of("kkkk")))
    check("it was tried, and nothing was asked over it", server.accepted >= 1 and server.stream_count() == 0,
          f"{server.accepted} connections, {server.stream_count()} requests")
    check("and nothing was shown", not fake.got)
    code = subprocess.run([DAEMON, "--publish", "kkkk"], env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30).returncode
    check("--publish won't either", code == 3 and not server.posts, str(code))
    d.stop()
    with open(log_path, "rb") as f:
        check("the token still isn't logged", TOKEN.encode() not in f.read())
    reset()


def test_tls_trusted():
    """The real road: libcurl's own list of trusted certificates, replaced by one in this
    mount namespace alone, so that no check in the daemon had to be changed for a test."""
    openssl = shutil.which("openssl")
    bundle = "/etc/ssl/certs/ca-certificates.crt"
    if not IN_NS or not openssl or not os.path.exists(bundle):
        print("SKIP https against a trusted server: no namespace, openssl or bundle here")
        return
    run = lambda *a: subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=root)
    run(openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "2",
        "-subj", "/CN=ztos test ca")

    def signed(name, san):
        run(openssl, "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{name}.key", "-out", f"{name}.csr", "-subj", f"/CN={name}")
        with open(os.path.join(root, f"{name}.ext"), "w") as f:
            f.write(f"subjectAltName={san}\n")
        run(openssl, "x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "2",
            "-out", f"{name}.pem", "-extfile", f"{name}.ext")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(os.path.join(root, f"{name}.pem"), os.path.join(root, f"{name}.key"))
        return ctx

    good, wrong = signed("good", "IP:127.0.0.1"), signed("wrong", "DNS:somewhere-else.example")
    if subprocess.run(["mount", "--bind", os.path.join(root, "ca.pem"), bundle], stderr=subprocess.DEVNULL).returncode != 0:
        print("SKIP https against a trusted server: can't mount here")
        return
    try:
        fake.got.clear()
        server, other = Server(good), Server(wrong)
        write_conf(sub("nnnn", server.url, token=TOKEN), sub("oooo", other.url, topic="wrongname", token=TOKEN))
        d = Daemon()
        s = server.wait_stream(1)
        check("https to a trusted certificate works", s is not None)
        if s:
            check("signed in", s.headers.get("authorization") == f"Bearer {TOKEN}", str(s.headers))
            s.line(opened())
            s.line(msg("tls1", "over https"))
            check("a message arrives over it", wait_for(lambda: [g["body"] for g in fake.got] == ["over https"]), str(fake.got))
        check("a trusted certificate for another name is refused",
              wait_for(lambda: "certificate" in (status_of("oooo") or ["", "", "", ""])[3]), str(status_of("oooo")))
        check("and the token wasn't sent to it", other.stream_count() == 0, str(other.stream_count()))
        code = subprocess.run([DAEMON, "--publish", "nnnn"], env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30).returncode
        check("--publish works over https", code == 0 and len(server.posts) == 1, str(code))
        d.stop()
    finally:
        subprocess.run(["umount", bundle], check=False)
    reset()


def test_real_timings():
    """With nothing shortened: how often it wakes on an open, silent stream."""
    server = Server()
    fake.got.clear()
    write_conf(sub("pppp", server.url))
    env = {k: v for k, v in ENV.items() if k != "ZTOS_NTFYD_TEST_FAST"}
    log = open(log_path, "ab")
    p = subprocess.Popen([DAEMON], env=env, stdout=log, stderr=log)
    d = Daemon.__new__(Daemon)
    d.p, d.log = p, log
    s = server.wait_stream(1)
    if s:
        s.line(opened())
        s.line(msg("rt1", "at real speed"))
        check("real timings: a backlog's wait, then it's shown", wait_for(lambda: len(fake.got) == 1, 4), str(fake.got))
        time.sleep(1.0)
        before = d.wakes()
        time.sleep(20.0)
        woke = d.wakes() - before
        check("real timings: an open, silent stream doesn't wake it", woke <= 1, f"{woke} wakes in 20 s")
        print(f"     (one topic: {d.rss_kb()} kB resident, {woke} wakes in 20 s)")
    d.stop()
    reset()


def test_network_change():
    if not IN_NS:
        print("SKIP the network change: no network namespace here")
        return
    server = Server()
    write_conf(sub("llll", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    if s:
        s.line(opened())
        time.sleep(0.8)     # older than "fresh"
        subprocess.run(["ip", "addr", "add", "10.77.0.1/24", "dev", "lo"], check=True)
        s2 = server.wait_stream(2, 3)
        check("a new address: a quiet stream reconnects at once", s2 is not None and s2.query == "since=1700000000", s2.query if s2 else "")
        if s2:
            s2.line(opened())
            time.sleep(0.8)
            # and a refreshed one (same address) isn't news
            n = server.stream_count()
            subprocess.run(["ip", "addr", "change", "10.77.0.1/24", "dev", "lo", "valid_lft", "100", "preferred_lft", "100"], check=True)
            time.sleep(0.6)
            check("the same address told again isn't a change", server.stream_count() == n, str(server.stream_count()))
    d.stop()
    subprocess.run(["ip", "addr", "del", "10.77.0.1/24", "dev", "lo"], check=False)
    reset()


def test_app():
    """The file as ztntfy really writes it, through the code behind its add button."""
    app = os.path.join(HERE, "..", "ntfy", "ztntfy")
    if not os.path.exists(app):
        print("SKIP the app: not built")
        return
    add = lambda *a: subprocess.run([app, "--add", *a], env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
    server = Server()
    d = Daemon()
    t0 = int(time.time())
    r = add(server.url, "from-the-app")
    check("the app adds a topic", r.returncode == 0, r.stdout)
    check("its file is private", stat.S_IMODE(os.stat(conf_path).st_mode) == 0o600, oct(os.stat(conf_path).st_mode))
    check("and its directory", stat.S_IMODE(os.stat(conf_dir).st_mode) == 0o700, oct(os.stat(conf_dir).st_mode))
    s = server.wait_stream(1)
    check("the daemon follows it, unprompted", s is not None and s.path == "/from-the-app/json", s.path if s else "")
    if s:
        since = int(s.query.split("=", 1)[1]) if s.query.startswith("since=") else 0
        check("from about now, not the topic's whole past", abs(since - t0) <= 5, s.query)
    check("the same topic twice is refused", add(server.url, "from-the-app").returncode == 1)
    for bad in (("ftp://127.0.0.1", "x"), ("https://a@b", "x"), (server.url, "a/b"), (server.url, "")):
        check(f"refused: {bad}", add(*bad).returncode == 1)
    check("a bare name means https, not http", add("bare.invalid", "t2").returncode == 0 and "https://bare.invalid\t" in open(conf_path).read())
    png = os.path.join(root, "list.png")
    r = subprocess.run([app, "--render", "list", png], env=dict(ENV, ZTMETRO_SCALE="2"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    check("its list page draws", r.returncode == 0 and os.path.getsize(png) > 1000, str(r.returncode))
    d.stop()
    reset()


def test_real_ztnotifyd():
    """The whole road to the shell's control pipe, through the real ztnotifyd on this private
    bus and a fifo standing in for the shell. Run under another name, so the shell's own
    pkill can't find it."""
    if not os.path.exists(ZTNOTIFYD):
        print("SKIP the real ztnotifyd: not built")
        return
    fake.stop()
    time.sleep(0.2)
    copy = os.path.join(root, "ztnotifyd-test")
    shutil.copy(ZTNOTIFYD, copy)
    fifo = os.path.join(runtime, "ztmetro.ctl")
    os.mkfifo(fifo)
    said = []

    def read_fifo():
        with open(fifo, "r", errors="replace") as f:
            while True:
                line = f.readline()
                if line:
                    said.append(line.rstrip("\n"))
                else:
                    time.sleep(0.02)
    threading.Thread(target=read_fifo, daemon=True).start()
    log = open(os.path.join(root, "ztnotifyd.log"), "wb")
    zn = subprocess.Popen([copy], env=ENV, stdout=log, stderr=log)
    owner = lambda: main_conn.name_has_owner("org.freedesktop.Notifications")
    check("the real ztnotifyd takes the name", wait_for(owner))

    server = Server()
    write_conf(sub("mmmm", server.url))
    d = Daemon()
    s = server.wait_stream(1)
    try:
        if not s:
            check("real ztnotifyd: connects", False)
            return
        s.line(opened())
        time.sleep(0.5)
        s.line(msg("r1", "x < 5 and more | text", title="Quiet one", priority=1))
        s.line(msg("r2", "é" * 500, title="€" * 100, priority=4))
        notes = lambda: [x for x in said if x.startswith("notekey ")]
        wait_for(lambda: len(notes()) >= 2)
        check("the shell is told both", len(notes()) >= 2, str(said))
        if len(notes()) >= 2:
            a = notes()[0][len("notekey "):].split("|")
            b = notes()[1][len("notekey "):].split("|")
            check("priority 1 reaches the shell as quiet", a[1] == "q", str(a))
            check("priority 4 reaches the shell as alerting", b[1] == "-", str(b))
            check("who it is reaches the shell", a[3] == "ztntfy", str(a))
            check("the topic is the app's label", a[4] == "Phone", str(a))
            check("an open < doesn't cost the rest of the text", "and more" in a[6], str(a))
            # what the shell keeps, in bytes (ztmetro.c's notes[]), holds whole characters
            title, body = b[5].encode(), b[6].encode()
            check("the title fits the shell's field", len(title) < 48, str(len(title)))
            check("the text fits the shell's field", len(body) < 200, str(len(body)))
            title.decode("utf-8")
            body.decode("utf-8")
            check("and neither is cut through a character", True)
    finally:
        d.stop()
        zn.terminate()
        zn.wait()
        log.close()
    reset()


def main():
    for t in (test_no_topics, test_stream, test_backlog, test_hostile, test_service_absent, test_errors_and_secrets,
              test_backoff_and_down, test_tls, test_tls_trusted, test_real_timings, test_network_change, test_app, test_real_ztnotifyd):
        print(f"--- {t.__name__}", flush=True)
        t()
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    if failures:
        for f in failures:
            print("  " + f)
    bus_proc.terminate()
    if os.environ.get("ZTOS_NTFYD_TEST_KEEP"):
        print(root)
    else:
        shutil.rmtree(root, ignore_errors=True)
    # (threads and the GLib loop are daemons: nothing to wait for)
    sys.stdout.flush()
    os._exit(1 if failures else 0)


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, lambda *_: (print("timed out"), os._exit(2)))
    signal.alarm(300)
    main()
