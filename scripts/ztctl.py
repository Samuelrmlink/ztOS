#!/usr/bin/env python3
"""Drive a ztOS device over SSH: see its screen and touch it.

  scripts/ztctl.py shot [-s N] out.png    capture the screen (1/N size)
  scripts/ztctl.py ctl 'LINE' ['LINE'...]  send lines to the shell's control pipe
  scripts/ztctl.py tap X Y                 tap, in the shell's points
  scripts/ztctl.py swipe X0 Y0 X1 Y1 [MS]  drag one finger
  scripts/ztctl.py pinch CX CY R0 R1 [MS]  move two fingers apart (or together)
  scripts/ztctl.py play FILE|-             play a timeline of overlapping fingers
  scripts/ztctl.py wake | sleep            screen on or off
  scripts/ztctl.py size                    points, scale, pixels and whether the screen is on

For a live view and input without SSH, see ztos mirror (ztmetro/mirror/README.md).

Control lines include "down ID X Y", "move ID X Y", "up ID" and "cancel ID", so
up to ten fingers can be down at once, each moving and lifting on its own, and
"key CODE down|up" for power (116), volume up (115), volume down (114), home (172)
and back (158). They go through the same code as the touchscreen. A timeline has one "MS LINE" per line, where MS
is the time from the start. For example, a second finger taps while one is held:

    0   down 0 180 600
    200 down 1 60 100
    260 up 1
    500 up 0

The device plays a timeline itself, so SSH delay doesn't change its timing. The
screen is 360x740 points on the phone.

The device is ZT_SSH, as ssh arguments. The default is the phone over USB; for
the emulator use ZT_SSH='-p 2222 user@localhost'. Nothing is installed on the
device: the capture client below is sent to its python3 over the connection.
"""
import getpass
import os
import shlex
import struct
import subprocess
import sys
import zlib

DEFAULT_SSH = f"-i ~/.ssh/ztos_phone -o IdentitiesOnly=yes {os.environ.get('ZT_USER') or getpass.getuser()}@172.16.42.1"
CTL = 'timeout 5 tee "$XDG_RUNTIME_DIR/ztmetro.ctl" >/dev/null'

# Runs on the device. A minimal Wayland client for wlr-screencopy: writes
# "w h\n" and then RGB rows to stdout, using only the standard library.
DEVICE_CAPTURE = r'''
import mmap, os, socket, struct, sys
step = int(sys.argv[1])
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ["XDG_RUNTIME_DIR"] + "/" + os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
next_id = [2]
def new():
    next_id[0] += 1
    return next_id[0] - 1
def send(obj, op, body=b"", fds=()):
    msg = struct.pack("=IHH", obj, op, 8 + len(body)) + body
    if fds:
        s.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("=%di" % len(fds), *fds))])
    else:
        s.sendall(msg)
def wstr(t):
    b = t.encode() + b"\0"
    return struct.pack("=I", len(b)) + b + b"\0" * (-len(b) % 4)
def rstr(b):
    n = struct.unpack_from("=I", b)[0]
    return b[4:4 + n - 1].decode(), b[4 + ((n + 3) & ~3):]
buf = b""
def events():
    global buf
    while True:
        while len(buf) >= 8:
            obj, op, size = struct.unpack_from("=IHH", buf)
            if len(buf) < size:
                break
            body, buf = buf[8:size], buf[size:]
            yield obj, op, body
        d = s.recv(65536)
        if not d:
            sys.exit("the compositor closed the connection")
        buf += d
ev = events()
reg, cb = new(), new()
send(1, 1, struct.pack("=I", reg))
send(1, 0, struct.pack("=I", cb))
g = {}
for obj, op, body in ev:
    if obj == 1 and op == 0:
        sys.exit("wayland error: %r" % (struct.unpack_from("=II", body),))
    if obj == reg and op == 0:
        name = struct.unpack_from("=I", body)[0]
        iface, rest = rstr(body[4:])
        g.setdefault(iface, (name, struct.unpack_from("=I", rest)[0]))
    if obj == cb and op == 0:
        break
if "zwlr_screencopy_manager_v1" not in g:
    sys.exit("this compositor offers no wlr-screencopy")
def bind(iface, ver):
    i, name = new(), g[iface][0]
    send(reg, 0, struct.pack("=I", name) + wstr(iface) + struct.pack("=II", min(ver, g[iface][1]), i))
    return i
shm, out, mgr = bind("wl_shm", 1), bind("wl_output", 1), bind("zwlr_screencopy_manager_v1", 3)
frame = new()
send(mgr, 0, struct.pack("=IiI", frame, 0, out))
fmt = None
flags = 0
done = False
for obj, op, body in ev:
    if obj == 1 and op == 0:
        sys.exit("wayland error: %r" % (struct.unpack_from("=II", body),))
    if obj != frame:
        continue
    if op == 0:
        if fmt is None:
            fmt = struct.unpack("=IIII", body)
    elif op == 6 or (op == 0 and g["zwlr_screencopy_manager_v1"][1] < 3):
        f, w, h, stride = fmt
        size = stride * h
        fd = os.memfd_create("ztshot")
        os.ftruncate(fd, size)
        mem = mmap.mmap(fd, size)
        pool, wbuf = new(), new()
        send(shm, 0, struct.pack("=Ii", pool, size), [fd])
        send(pool, 0, struct.pack("=IiiiiI", wbuf, 0, w, h, stride, f))
        send(frame, 0, struct.pack("=I", wbuf))
    elif op == 1:
        flags = struct.unpack("=I", body)[0]
    elif op == 3:
        sys.exit("the capture failed (is the screen off?)")
    elif op == 2:
        break
f, w, h, stride = fmt
if f in (0, 1):
    r, b = 2, 0
elif f in (0x34324241, 0x34324258):
    r, b = 0, 2
else:
    sys.exit("unexpected pixel format 0x%x" % f)
rows = range(h - 1, -1, -step) if flags & 1 else range(0, h, step)
ow = (w + step - 1) // step
o = sys.stdout.buffer
o.write(b"%d %d\n" % (ow, len(rows)))
line = bytearray(ow * 3)
for y in rows:
    start = y * stride
    px = mem[start:start + w * 4]
    line[0::3] = px[r::4 * step]
    line[1::3] = px[1::4 * step]
    line[2::3] = px[b::4 * step]
    o.write(line)
o.flush()
'''


# Runs on the device. Plays "MS LINE" from stdin to the control pipe, on time.
DEVICE_PLAY = r'''
import os, sys, time
steps = []
for raw in sys.stdin:
    raw = raw.split("#")[0].strip()
    if raw:
        at, line = raw.split(None, 1)
        steps.append((float(at) / 1000, line))
steps.sort(key=lambda s: s[0])
fd = os.open(os.environ["XDG_RUNTIME_DIR"] + "/ztmetro.ctl", os.O_WRONLY | os.O_NONBLOCK)
held = set()
start = time.monotonic()
try:
    for at, line in steps:
        wait = start + at - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        os.write(fd, line.encode() + b"\n")
        w = line.split()
        if w[0] == "down":
            held.add(w[1])
        elif w[0] in ("up", "cancel"):
            held.discard(w[1])
finally:
    # no finger stays on the glass if this is cut short
    for f in held:
        os.write(fd, b"up %s\n" % f.encode())
'''


def ssh_args():
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6", "-o", "StrictHostKeyChecking=accept-new",
            "-o", "UserKnownHostsFile=" + os.path.expanduser("~/.ssh/known_hosts_ztos"),
            *[os.path.expanduser(a) for a in shlex.split(os.environ.get("ZT_SSH", DEFAULT_SSH))]]


def write_png(path, w, h, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw += b"\0" + rows[y * stride:(y + 1) * stride]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def shot(args):
    step = 1
    if args[:1] == ["-s"]:
        step, args = int(args[1]), args[2:]
    if len(args) != 1:
        sys.exit(__doc__)
    cmd = "exec python3 -c " + shlex.quote(DEVICE_CAPTURE) + " " + str(step)
    p = subprocess.run(ssh_args() + [cmd], capture_output=True, timeout=120)
    if p.returncode != 0:
        sys.exit(p.stderr.decode(errors="replace").strip() or "capture failed")
    head, _, rows = p.stdout.partition(b"\n")
    w, h = map(int, head.split())
    if len(rows) != w * h * 3:
        sys.exit(f"incomplete capture: {len(rows)} of {w * h * 3} bytes")
    write_png(args[0], w, h, rows)
    print(f"{args[0]}: {w}x{h}")


def ctl(lines):
    subprocess.run(ssh_args() + [CTL], input="".join(l + "\n" for l in lines).encode(), check=True, timeout=30)


def play(timeline):
    last = max((float(l.split()[0]) for l in timeline.splitlines() if l.split("#")[0].strip()), default=0)
    cmd = "exec python3 -c " + shlex.quote(DEVICE_PLAY)
    subprocess.run(ssh_args() + [cmd], input=timeline.encode(), check=True, timeout=30 + last / 1000)


def path(finger, x0, y0, x1, y1, ms):
    """One finger's down, moves about every 16 ms, and up."""
    steps = max(2, int(ms) // 16)
    lines = [f"0 down {finger} {x0:.1f} {y0:.1f}"]
    for i in range(1, steps + 1):
        t = i / steps
        lines.append(f"{ms * t:.0f} move {finger} {x0 + (x1 - x0) * t:.1f} {y0 + (y1 - y0) * t:.1f}")
    lines.append(f"{ms + 1:.0f} up {finger}")
    return lines


def swipe(args):
    x0, y0, x1, y1 = map(float, args[:4])
    play("\n".join(path(0, x0, y0, x1, y1, float(args[4]) if len(args) > 4 else 300)) + "\n")


def pinch(args):
    cx, cy, r0, r1 = map(float, args[:4])
    ms = float(args[4]) if len(args) > 4 else 400
    play("\n".join(path(0, cx - r0, cy, cx - r1, cy, ms) + path(1, cx + r0, cy, cx + r1, cy, ms)) + "\n")


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd, rest = a[0], a[1:]
    if cmd == "shot":
        shot(rest)
    elif cmd == "ctl" and rest:
        ctl(rest)
    elif cmd == "tap" and len(rest) == 2:
        ctl([f"tap {rest[0]} {rest[1]}"])
    elif cmd == "swipe" and len(rest) >= 4:
        swipe(rest)
    elif cmd == "pinch" and len(rest) >= 4:
        pinch(rest)
    elif cmd == "play" and len(rest) == 1:
        play(sys.stdin.read() if rest[0] == "-" else open(rest[0]).read())
    elif cmd == "wake":
        ctl(["screen on"])
    elif cmd == "sleep":
        ctl(["screen off"])
    elif cmd == "size":
        subprocess.run(ssh_args() + ['cat "$XDG_RUNTIME_DIR/ztmetro.state"'], check=True, timeout=30)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
