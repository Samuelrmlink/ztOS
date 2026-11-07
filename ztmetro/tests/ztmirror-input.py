#!/usr/bin/env python3
"""ztmirror's input path, with no shell and no compositor: a FIFO stands in for
the control pipe, and we read exactly what ztmirror writes to it.

    ztmetro/tests/ztmirror-input.py ztmetro/mirror/ztmirror
"""
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

PORT = 27999
DOWN, MOVE, UP, CANCEL, KEY, PING = 1, 2, 3, 4, 5, 6
failures = 0


def check(name, got, want):
    global failures
    ok = got == want
    print(("ok  " if ok else "FAIL") + " " + name + ("" if ok else f"\n  got  {got!r}\n  want {want!r}"))
    failures += not ok


def msg(t, i=0, x=0, y=0):
    return struct.pack(">BBHHH", t, i, x, y, 0)


class Pipe:
    def __init__(self, path):
        os.mkfifo(path, 0o600)
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self.buf = b""

    def lines(self, wait=0.3):
        """Everything written within `wait` of the last line."""
        out = []
        while select.select([self.fd], [], [], wait)[0]:
            self.buf += os.read(self.fd, 65536)
            *done, self.buf = self.buf.split(b"\n")
            out += [d.decode() for d in done]
        return out


def connect(token, line=None):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.sendall(line if line is not None else f"ZTMIRROR 1 {token} scale=2 quality=70 fps=30\n".encode())
    return s


def recv_msg(s):
    def need(n):
        b = b""
        while len(b) < n:
            d = s.recv(n - len(b))
            if not d:
                raise EOFError
            b += d
        return b
    t, n = struct.unpack(">BI", need(5))
    return t, need(n)


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as run:
        env = dict(os.environ, XDG_RUNTIME_DIR=run, WAYLAND_DISPLAY="no-such-display")
        with open(run + "/ztmetro.state", "w") as f:
            f.write("points 360 740\nscale 4.00\npixels 1440 2960\nscreen on\n")
        pipe = Pipe(run + "/ztmetro.ctl")

        # never on a wildcard, nor an address not on this machine
        r = subprocess.run([binary, "--bind", "0.0.0.0", "--port", str(PORT), "--idle-exit", "1"], env=env,
                           capture_output=True, text=True, timeout=10)
        check("no wildcard listening", "is not up here" in r.stderr, True)

        d = subprocess.Popen([binary, "--bind", "127.0.0.1", "--port", str(PORT)], env=env)
        try:
            for _ in range(50):
                if os.path.exists(run + "/ztmirror.token"):
                    break
                time.sleep(0.1)
            time.sleep(0.2)
            token = open(run + "/ztmirror.token").read().strip()
            check("token is 32 hex", len(token), 32)
            check("token is private", oct(os.stat(run + "/ztmirror.token").st_mode & 0o777), "0o600")

            s = connect("0" * 32)
            t, why = recv_msg(s)
            check("wrong token refused", (t, why), (3, b"wrong token"))
            s.close()

            # raw lines must not reach the pipe, whatever they say
            s = connect(token, b"launch evil\nZTMIRROR 1 nope\n")
            s.close()
            s = connect(token)
            s.sendall(b"launch evil\n" * 20)
            check("text is not passed through", [l for l in pipe.lines() if "launch" in l or "evil" in l], [])
            s.close()
            pipe.lines()

            s = connect(token)
            t, status = recv_msg(s)
            st = json.loads(status)
            check("status: points", st["points"], [360, 740])
            check("status: screen", st["screen"], "on")

            # order preserved, overlapping fingers, nothing merged
            s.sendall(msg(DOWN, 0, 0, 0) + msg(DOWN, 1, 65535, 65535) + msg(MOVE, 0, 32768, 0) +
                      msg(MOVE, 0, 32768, 32768) + msg(UP, 1) + msg(MOVE, 0, 65535, 0) + msg(UP, 0))
            check("two fingers in order", pipe.lines(), [
                "down 100 0.00 0.00", "down 101 360.00 740.00", "move 100 180.00 0.00",
                "move 100 180.00 370.01", "up 101", "move 100 360.00 0.00", "up 100"])

            # ten at once, an eleventh refused
            s.sendall(b"".join(msg(DOWN, i, 100, 100) for i in range(11)))
            check("ten fingers, not eleven", len(pipe.lines()), 10)

            # the client goes away with them down: all lifted, keys too
            s.sendall(msg(KEY, 3, 1))
            check("key down", pipe.lines(), ["key 172 down"])
            s.close()
            lines = pipe.lines()
            check("disconnect lifts every finger and key", sorted(lines),
                  sorted([f"cancel {100 + i}" for i in range(10)] + ["key 172 up"]))

            s = connect(token)
            s.sendall(msg(MOVE, 0, 1, 1) + msg(UP, 3) + msg(CANCEL, 4) + msg(DOWN, 200, 1, 1) + msg(KEY, 99, 1))
            check("moves and lifts of no finger, bad IDs and keys: nothing", pipe.lines(), [])

            s.sendall(msg(KEY, 0, 1) + msg(KEY, 0, 1) + msg(KEY, 0, 0) + msg(KEY, 1, 1) + msg(KEY, 1, 0) +
                      msg(KEY, 2, 1) + msg(KEY, 2, 0) + msg(KEY, 4, 1) + msg(KEY, 4, 0))
            check("keys", pipe.lines(), ["key 116 down", "key 116 up", "key 115 down", "key 115 up",
                                         "key 114 down", "key 114 up", "key 158 down", "key 158 up"])

            s.sendall(msg(DOWN, 0, 0, 0) + msg(DOWN, 0, 65535, 0) + msg(CANCEL, 0))
            check("a second down of one ID cancels the first", pipe.lines(),
                  ["down 100 0.00 0.00", "cancel 100", "down 100 360.00 0.00", "cancel 100"])

            s.sendall(msg(PING, 0, 7, 9))
            while True:
                t, body = recv_msg(s)
                if t == 4:
                    break
            check("ping", struct.unpack(">HH", body[:4]), (7, 9))

            # a new client replaces the old one, whose fingers are lifted
            s.sendall(msg(DOWN, 2, 0, 0))
            pipe.lines()
            s2 = connect(token)
            check("a new client lifts the old one's fingers", pipe.lines(), ["cancel 102"])
            s2.settimeout(3)

            # a silent client (a cable pulled: no FIN) is noticed and lifted
            s2.sendall(msg(DOWN, 5, 0, 0))
            pipe.lines(0.2)
            check("a silent link lifts its fingers", pipe.lines(7), ["cancel 105"])
            s2.close()
            s.close()

            # killed politely with a finger down: lifted
            s = connect(token)
            s.sendall(msg(DOWN, 7, 0, 0))
            pipe.lines(0.2)
            d.send_signal(signal.SIGTERM)
            d.wait(5)
            check("SIGTERM lifts", pipe.lines(), ["cancel 107"])
            check("token gone at exit", os.path.exists(run + "/ztmirror.token"), False)
        finally:
            if d.poll() is None:
                d.kill()
    print("all good" if not failures else f"{failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
