# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos mirror and ztos touch: the device's screen and fingers, from this computer.

The device runs ztmirror (ztmetro/mirror/, protocol in its README). SSH is used
once, to start it and fetch its token; after that it is a plain TCP socket. This
file needs only the standard library. The window is in mirror_view.py (PySide6).
"""
from __future__ import annotations

import json
import os
import shlex
import socket
import struct
import subprocess
import threading
import time

from .build import Fail

PORT = 27183
DOWN, MOVE, UP, CANCEL, KEY, PING = 1, 2, 3, 4, 5, 6
S_FRAME, S_STATUS, S_ERROR, S_PONG = 1, 2, 3, 4
MAX_FINGERS = 10

# id on the wire, and the evdev code for timelines that use the number
KEYS = {"power": (0, 116), "volup": (1, 115), "voldown": (2, 114), "home": (3, 172), "back": (4, 158)}

TARGETS = {
    "phone": {"host": "172.16.42.1", "ssh": "-i ~/.ssh/ztos_phone -o IdentitiesOnly=yes samuel@172.16.42.1"},
    # the forwarded ports in targets.qemu_command; the user is the one installed
    "qemu": {"host": "127.0.0.1", "ssh": "-p 2222 ztos@localhost"},
}

START = ('systemctl --user is-active -q ztmirror && echo running; '
         'systemctl --user start ztmirror || exit 1; '
         'for i in $(seq 50); do [ -s "$XDG_RUNTIME_DIR/ztmirror.token" ] && '
         'exec cat "$XDG_RUNTIME_DIR/ztmirror.token"; sleep 0.1; done; exit 1')
STOP = 'systemctl --user stop ztmirror'


def ssh_command(target: str, override: str | None = None) -> list:
    spec = override or os.environ.get("ZT_SSH") or TARGETS[target]["ssh"]
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6", "-o", "StrictHostKeyChecking=accept-new",
            "-o", "UserKnownHostsFile=" + os.path.expanduser("~/.ssh/known_hosts_ztos"),
            *[os.path.expanduser(a) for a in shlex.split(spec)]]


class Device:
    """How to reach one device, and how to start and stop its ztmirror."""

    def __init__(self, target="phone", host=None, port=PORT, token=None, ssh=None, keep=False):
        self.target, self.host, self.port = target, host or TARGETS[target]["host"], port
        self.fixed_token, self.token = token, token
        self.ssh = ssh_command(target, ssh)
        self.keep = keep
        self.started = False

    def start(self):
        """Start ztmirror on the device and read its token (not if one was given)."""
        if self.fixed_token:
            return
        p = subprocess.run(self.ssh + [START], capture_output=True, text=True, timeout=30)
        if p.returncode != 0:
            raise Fail(f"could not start ztmirror over ssh: {p.stderr.strip() or 'is it installed on the device?'}")
        lines = p.stdout.split()
        self.token = lines[-1]
        # one that was running before us stays running after us
        self.started = self.started or "running" not in lines

    def stop(self):
        if self.started and not self.keep:
            subprocess.run(self.ssh + [STOP], capture_output=True, timeout=15, check=False)


class Refused(Exception):
    """ztmirror said no (a wrong token, usually): trying again as it is won't help."""


class Link:
    """One connection. Frames and status arrive on a thread of its own, through
    the callbacks; send() may be called from any thread, and goes at once."""

    def __init__(self, device: Device, scale=2, quality=70, fps=20, on_frame=None, on_status=None, on_closed=None):
        self.points = None
        self.status = {}
        self.on_frame, self.on_status, self.on_closed = on_frame, on_status, on_closed
        self.closed = threading.Event()
        self.rtt_ms = None
        self.taken_over = False
        self._send_lock = threading.Lock()
        self.sock = socket.create_connection((device.host, device.port), timeout=4)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.sendall(f"ZTMIRROR 1 {device.token} scale={scale} quality={quality} fps={fps}\n".encode())
        # the first message says whether we are in
        self.sock.settimeout(4)
        kind, body = self._read_message()
        if kind == S_ERROR:
            self.sock.close()
            raise Refused(body.decode(errors="replace"))
        self._handle(kind, body)
        self.sock.settimeout(None)
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._pinger, daemon=True).start()

    def _read_exactly(self, n):
        chunks, left = [], n
        while left:
            d = self.sock.recv(min(left, 1 << 20))
            if not d:
                raise ConnectionError("closed")
            chunks.append(d)
            left -= len(d)
        return b"".join(chunks)

    def _read_message(self):
        kind, size = struct.unpack(">BI", self._read_exactly(5))
        if size > 64 << 20:
            raise ConnectionError("a message too large to be real")
        return kind, self._read_exactly(size)

    def _handle(self, kind, body):
        if kind == S_FRAME:
            seq, device_us, encode_us, w, h = struct.unpack_from(">IQIHH", body)
            if self.on_frame:
                self.on_frame(body[20:], w, h, seq, device_us, encode_us)
        elif kind == S_STATUS:
            self.status = json.loads(body)
            self.points = self.status.get("points")
            if self.on_status:
                self.on_status(self.status)
        elif kind == S_ERROR:
            self.taken_over = True
        elif kind == S_PONG:
            sent = struct.unpack_from(">I", body)[0]
            now = int(time.monotonic() * 1000) & 0xFFFFFFFF
            self.rtt_ms = (now - sent) & 0xFFFFFFFF

    def _reader(self):
        try:
            # a dead link shows as silence: the status arrives every second
            self.sock.settimeout(5)
            while True:
                self._handle(*self._read_message())
        except (OSError, ConnectionError, struct.error, ValueError):
            pass
        self.close()

    def _pinger(self):
        while not self.closed.wait(1):
            now = int(time.monotonic() * 1000) & 0xFFFFFFFF
            self.send(PING, 0, now >> 16, now & 0xFFFF)

    def send(self, kind, finger=0, a=0, b=0):
        try:
            with self._send_lock:
                self.sock.sendall(struct.pack(">BBHHH", kind, finger, a, b, 0))
        except OSError:
            self.close()

    def close(self):
        if self.closed.is_set():
            return
        self.closed.set()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        if self.on_closed:
            self.on_closed()

    # fingers, with x and y from 0 to 1 across the screen
    def down(self, finger, x, y):
        self.send(DOWN, finger, _unit(x), _unit(y))

    def move(self, finger, x, y):
        self.send(MOVE, finger, _unit(x), _unit(y))

    def up(self, finger):
        self.send(UP, finger)

    def cancel(self, finger):
        self.send(CANCEL, finger)

    def key(self, name, down):
        self.send(KEY, KEYS[name][0], 1 if down else 0)


def _unit(v):
    return max(0, min(65535, round(v * 65535)))


# ---- a timeline: the format of scripts/ztctl.py ------------------------------

def parse_timeline(text):
    """[(seconds, line)], sorted. One "MS LINE" per line, # for comments, where
    LINE is down ID X Y, move ID X Y, up ID, cancel ID (in points), tap X Y, or
    key NAME down|up. Anything else is an error: nothing reaches the pipe as text."""
    steps = []
    for number, raw in enumerate(text.splitlines(), 1):
        raw = raw.split("#")[0].strip()
        if not raw:
            continue
        try:
            at, *w = raw.split()
            at = float(at) / 1000
            if w[0] in ("down", "move") and len(w) == 4:
                step = (w[0], int(w[1]), float(w[2]), float(w[3]))
            elif w[0] in ("up", "cancel") and len(w) == 2:
                step = (w[0], int(w[1]))
            elif w[0] == "tap" and len(w) == 3:
                x, y = float(w[1]), float(w[2])
                steps += [(at, ("down", MAX_FINGERS - 1, x, y)), (at + 0.03, ("up", MAX_FINGERS - 1))]
                continue
            elif w[0] == "key" and len(w) == 3 and w[2] in ("down", "up"):
                step = ("key", key_name(w[1]), w[2] == "down")
            else:
                raise ValueError
        except (ValueError, IndexError):
            raise Fail(f"timeline line {number}: can't read {raw!r}") from None
        if step[0] != "key" and not 0 <= step[1] < MAX_FINGERS:
            raise Fail(f"timeline line {number}: fingers are 0 to {MAX_FINGERS - 1}")
        steps.append((at, step))
    steps.sort(key=lambda s: s[0])
    return steps


def key_name(word):
    """A key by name or by evdev code."""
    if word in KEYS:
        return word
    for name, (_, code) in KEYS.items():
        if word == str(code):
            return name
    raise ValueError(word)


def play_timeline(link: Link, steps, log=None):
    """Plays on this computer's clock, each step sent as its moment comes."""
    w, h = link.points
    held, keys = set(), set()
    start = time.monotonic()
    try:
        for at, step in steps:
            wait = start + at - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            kind = step[0]
            if kind == "down":
                link.down(step[1], step[2] / w, step[3] / h)
                held.add(step[1])
            elif kind == "move":
                link.move(step[1], step[2] / w, step[3] / h)
            elif kind == "up":
                link.up(step[1])
                held.discard(step[1])
            elif kind == "cancel":
                link.cancel(step[1])
                held.discard(step[1])
            elif kind == "key":
                link.key(step[1], step[2])
                (keys.add if step[2] else keys.discard)(step[1])
            if log:
                log(f"{time.monotonic() - start:7.3f} {step}")
    finally:
        # ztmirror would do this too when we hang up
        for f in held:
            link.up(f)
        for k in keys:
            link.key(k, False)


def path(finger, x0, y0, x1, y1, ms):
    """One finger's down, moves about every 16 ms, and up, as timeline lines."""
    steps = max(2, int(ms) // 16)
    lines = [f"0 down {finger} {x0:.2f} {y0:.2f}"]
    for i in range(1, steps + 1):
        t = i / steps
        lines.append(f"{ms * t:.0f} move {finger} {x0 + (x1 - x0) * t:.2f} {y0 + (y1 - y0) * t:.2f}")
    lines.append(f"{ms + 1:.0f} up {finger}")
    return lines


# ---- no window ---------------------------------------------------------------

def _connect_once(device, **kw):
    device.start()
    try:
        return Link(device, **kw)
    except OSError as e:
        # just started, it may have a moment to go; or the token was an old one
        time.sleep(0.5)
        try:
            return Link(device, **kw)
        except OSError:
            raise Fail(f"could not reach ztmirror at {device.host}:{device.port}: {e}") from None
    except Refused as e:
        raise Fail(f"ztmirror refused the connection: {e}") from None


def save_image(path_out, jpeg):
    """As it came for .jpg; .png needs Qt to decode it."""
    if path_out.lower().endswith((".jpg", ".jpeg")):
        with open(path_out, "wb") as f:
            f.write(jpeg)
        return
    try:
        from PySide6.QtGui import QImage
    except ImportError:
        raise Fail(f"{path_out}: without PySide6 only .jpg can be saved") from None
    image = QImage.fromData(jpeg, "JPEG")
    if image.isNull() or not image.save(path_out):
        raise Fail(f"could not save {path_out}")


def snapshot(device, out, scale=2, quality=85, wait=5.0):
    """Save the next frame. Returns (w, h), or fails saying why none came."""
    got = threading.Event()
    frame = {}

    def on_frame(jpeg, w, h, seq, device_us, encode_us):
        frame.update(jpeg=jpeg, w=w, h=h)
        got.set()

    link = _connect_once(device, scale=scale, quality=quality, fps=30, on_frame=on_frame)
    try:
        if not got.wait(wait):
            if link.status.get("screen") == "off":
                raise Fail("the screen is off: no frames while it is off (ztos touch key power)")
            raise Fail(f"no frame in {wait:.0f} s; the device says {link.status}")
        save_image(out, frame["jpeg"])
        return frame["w"], frame["h"]
    finally:
        link.close()


def stats(device, seconds, scale=2, quality=70, fps=20, say=print):
    """Count what arrives for a while, with no window."""
    times, sizes, encode, age = [], [], [], []

    def on_frame(jpeg, w, h, seq, device_us, encode_us):
        now = time.monotonic()
        times.append(now)
        sizes.append(len(jpeg))
        encode.append(encode_us / 1000)
        age.append((now, device_us))

    link = _connect_once(device, scale=scale, quality=quality, fps=fps, on_frame=on_frame)
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds and not link.closed.is_set():
            time.sleep(0.2)
        n = len(times)
        took = time.monotonic() - start
        total = sum(sizes)
        result = {"frames": n, "seconds": round(took, 2), "fps": round(n / took, 1),
                  "mbit_s": round(total * 8 / took / 1e6, 1), "kb_per_frame": round(total / max(n, 1) / 1000, 1),
                  "encode_ms": round(sum(encode) / max(n, 1), 1), "rtt_ms": link.rtt_ms, "status": link.status}
        say(json.dumps(result))
        return result
    finally:
        link.close()


# ---- the commands ------------------------------------------------------------

def device_from(args) -> Device:
    return Device(args.target, host=args.host, port=args.port, token=args.token, ssh=args.ssh, keep=args.keep)


def cmd_mirror(args):
    device = device_from(args)
    try:
        if args.snapshot:
            w, h = snapshot(device, args.snapshot, scale=args.scale, quality=max(args.quality, 85))
            print(f"{args.snapshot}: {w}x{h}")
        elif args.stats:
            stats(device, args.stats, scale=args.scale, quality=args.quality, fps=args.fps)
        else:
            try:
                from . import mirror_view
            except ImportError as e:
                raise Fail(f"the window needs PySide6 ({e}): dnf install python3-pyside6, or pacman -S pyside6. "
                           "Without it: ztos mirror --snapshot out.jpg, ztos mirror --stats 5, ztos touch") from None
            mirror_view.run(device, scale=args.scale, quality=args.quality, fps=args.fps)
    finally:
        device.stop()


def cmd_touch(args):
    device = device_from(args)
    w = args.what
    try:
        if w == "tap":
            text = f"0 tap {args.args[0]} {args.args[1]}\n"
        elif w == "swipe":
            x0, y0, x1, y1 = map(float, args.args[:4])
            text = "\n".join(path(0, x0, y0, x1, y1, float(args.args[4]) if len(args.args) > 4 else 300)) + "\n"
        elif w == "pinch":
            cx, cy, r0, r1 = map(float, args.args[:4])
            ms = float(args.args[4]) if len(args.args) > 4 else 400
            text = "\n".join(path(0, cx - r0, cy, cx - r1, cy, ms) + path(1, cx + r0, cy, cx + r1, cy, ms)) + "\n"
        elif w == "key":
            name = args.args[0]
            how = args.args[1] if len(args.args) > 1 else "press"
            if how == "press":
                text = f"0 key {name} down\n60 key {name} up\n"
            else:
                text = f"0 key {name} {how}\n"
        elif w == "play":
            with (open(args.args[0]) if args.args[0] != "-" else os.fdopen(0)) as f:
                text = f.read()
        else:
            raise Fail(f"unknown: {w}")
    except (IndexError, ValueError):
        raise Fail(f"ztos touch {w}: see ztos touch --help") from None
    steps = parse_timeline(text)
    link = _connect_once(device)
    try:
        for _ in range(20):     # a shell with no state file: from the first look at its output
            if link.points and link.points[0] > 0:
                break
            time.sleep(0.1)
        else:
            raise Fail(f"the device doesn't know its screen size yet: {link.status}")
        play_timeline(link, steps, print if args.verbose else None)
    finally:
        link.close()
        device.stop()
