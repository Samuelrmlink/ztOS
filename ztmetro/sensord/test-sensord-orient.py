#!/usr/bin/env python3
"""ztoscsf-sensord's orientation for auto-rotation, on the host: the
accelerometer on only while the shell asks (orient on) and the screen is on,
at 100 ms; the first reading taken at once, a turn held 300 ms before the
shell hears "orientation N"; flat, shaken or upside down changes nothing; the
45-degree boundary has hysteresis; the axes map. The hub is a pty, the shell
a FIFO (the helpers are test-sensord-prox.py's)."""
import importlib.util
import math
import os
import pty
import signal
import struct
import subprocess
import sys
import tempfile
import time
import tty
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / 'ztoscsf-sensord.c'


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


queue = load('queue', 'test-sensord-queue.py')
prox = load('prox', 'test-sensord-prox.py')
Hub, check, failures = queue.Hub, queue.check, queue.failures
header, Shell, record = prox.header, prox.Shell, prox.record
G = 4096


def accel(hub, deg, tilt=0.0, g=1.0, raw=None):
    """a reading with the phone turned deg counter-clockwise from upright
    and tilted back tilt degrees from vertical, in Android's frame"""
    if raw is None:
        a, t = math.radians(deg), math.radians(tilt)
        x, y, z = math.sin(a) * math.cos(t), math.cos(a) * math.cos(t), math.sin(t)
        raw = (round(x * g * G), round(y * g * G), round(z * g * G))
    hub.frame(record(0, struct.pack('<3h', *raw)))


def orientation(shell, timeout):
    until = time.monotonic() + timeout
    while True:
        got = shell.line(until - time.monotonic())
        if got is None or got[0].startswith('orientation'):
            return got


def main():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        exe = d / 'sensord'
        build = subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wno-unused-parameter',
                                '-Wno-missing-field-initializers', '-Werror',
                                '-o', str(exe), str(SOURCE), '-lm'], capture_output=True, text=True)
        check(build.returncode == 0, 'builds with -Wall, no warnings ' + build.stderr.strip())
        if build.returncode:
            return
        root = d / 'root'
        for sub in ('run/user/1000', 'var/lib/ztoscsf-sensord', 'run/ztoscorenetworking'):
            (root / sub).mkdir(parents=True)
        (root / 'run/zrnperfd.status').write_text('screen=on\n')
        (root / 'var/lib/ztoscsf-sensord/state').write_text('auto off\n')
        shell = Shell(root / 'run/user/1000/ztmetro.ctl')
        seq = d / 'resume_seq'
        seq.write_text('0\n')
        master, slave = pty.openpty()
        tty.setraw(slave)
        tty.setraw(master)
        env = dict(os.environ, ZTOSCSF_SSP=os.ttyname(slave), ZTOSCSF_RESUME_SEQ=str(seq),
                   ZTOSCSF_ROOT=str(root))
        err = open(d / 'stderr', 'w')
        proc = subprocess.Popen([str(exe)], env=env, stderr=err, stdout=err)
        hub = Hub(master)
        ctl = root / 'run/ztoscsf-sensord.ctl'
        out = root / 'run/ztoscsf-sensors'

        def say(line):
            fd = os.open(ctl, os.O_WRONLY)
            os.write(fd, (line + '\n').encode())
            os.close(fd)

        def txn(what, cmd, payload_check):
            kind, h, _ = header(hub, 2)
            check(kind == 'header' and h[0] == cmd, f'{what}: header 0x{cmd:02x} ({h.hex(" ") if h else ""})')
            if kind != 'header':
                return None
            hub.echo(h)
            t = hub.token(1)
            ok = t is not None and t[0] == 'payload' and payload_check(t[1])
            check(ok, f'{what}: payload {t[1].hex(" ") if t else None}')
            return t[1] if t else None

        def turn(deg, what, expect, hold=(0.25, 0.8), **kw):
            """readings every 100 ms at deg until the shell hears, or 1.2 s"""
            t0 = time.monotonic()
            while time.monotonic() - t0 < 1.2:
                accel(hub, deg, **kw)
                got = orientation(shell, 0.1)
                if got:
                    break
            else:
                got = None
            if expect is None:
                check(got is None, f'{what}: nothing told ({got})')
                return
            dt = got[1] - t0 if got else -1
            check(got is not None and got[0] == f'orientation {expect}' and hold[0] <= dt <= hold[1],
                  f'{what}: "orientation {expect}" after {dt:.2f} s ({got[0] if got else None})')

        try:
            t = hub.token(3)
            check(t is not None and t[0] == 'time', 'start: the time')
            txn('start', 0xa1, lambda p: p[0] == 9)
            check(header(hub, 0.3)[0] == 'none', 'not asked: the accelerometer stays off')

            say('orient on')
            txn('orient on: ADD accel at 100 ms', 0xa1,
                lambda p: p[0] == 0 and struct.unpack('<i', p[1:5])[0] == 100)
            check('orient=on -1' in (out / 'status').read_text(), 'status: orient=on, nothing yet')

            # flat on the table: no evidence, nothing told
            turn(0, 'flat', None, tilt=80)
            # the first upright reading: at once
            turn(0, 'first reading', 0, hold=(0, 0.15))
            check((out / 'orientation').read_text() == '0\n', 'orientation file: 0')
            check(not (out / 'accel').exists(), 'no accel file for orientation alone')
            # turned left: held 300 ms
            turn(90, 'turned left', 1)
            check('orient=on 1' in (out / 'status').read_text(), 'status: orient=on 1')
            # back past the boundary but not far (40 degrees from portrait):
            # hysteresis keeps 1
            turn(40, '50 degrees back', None)
            # a shake (2 g) toward portrait: nothing
            turn(0, 'shaken', None, g=2.0)
            # upside down: off by default
            turn(180, 'upside down', None)
            # turned right
            turn(-90, 'turned right', 3)
            # a blip shorter than the hold: nothing
            accel(hub, 0)
            time.sleep(0.1)
            accel(hub, -90)
            check(orientation(shell, 0.5) is None, 'portrait for 0.1 s: stays 3')
            # upside down allowed
            say('orient_180 on')
            turn(180, 'upside down, allowed', 2)
            say('orient_180 off')

            # the axes mapped: raw (-y, x, z) is the screen's (x, y, z)
            say('orient_axes -y+x+z')
            turn(0, 'axes -y+x+z, portrait', 0, raw=(G, 0, 0))
            state = (root / 'var/lib/ztoscsf-sensord/state').read_text()
            check('orient_axes -y+x+z' in state and 'orient_180 off' in state, 'axes remembered ' + repr(state))
            say('orient_axes +x+y+z')

            # screen off: the accelerometer goes with the light
            say('screen off')
            kinds = []
            for _ in range(2):
                kind, h, _ = header(hub, 2)
                if kind == 'header':
                    hub.echo(h)
                    t = hub.token(1)
                    kinds.append((h[0], t[1][0] if t else None))
            check(sorted(kinds) == [(0xa2, 0), (0xa2, 9)], f'screen off: accel and light removed {kinds}')
            say('screen on')
            seen = []
            for _ in range(2):
                kind, h, _ = header(hub, 2)
                if kind == 'header':
                    hub.echo(h)
                    t = hub.token(1)
                    seen.append(t[1][0] if t else None)
            check(sorted(seen) == [0, 9], f'screen on: accel and light back {seen}')
            turn(90, 'after the screen came on: at once', 1, hold=(0, 0.15))

            # a watch at 200 ms with orientation on keeps 100 ms
            say('watch accel 200')
            check(header(hub, 0.3)[0] == 'none', 'watch at 200 ms: no change of rate')
            accel(hub, 90)
            time.sleep(0.1)
            check((out / 'accel').exists(), 'watched: the accel file is written')

            # off: removed
            say('orient off')
            say('unwatch accel')
            txn('orient off and unwatched: REMOVE accel', 0xa2, lambda p: p[0] == 0)
            check('orient=off' in (out / 'status').read_text(), 'status: orient=off')

            proc.send_signal(signal.SIGTERM)
            for _ in range(2):
                kind, h, _ = header(hub, 1)
                if kind == 'header':
                    hub.echo(h)
                    hub.token(0.5)
            proc.wait(3)
            check(proc.returncode == 0, 'stops cleanly')
        finally:
            if proc.poll() is None:
                proc.kill()
            err.close()
            print((d / 'stderr').read_text(), end='')
    print('PASS' if not failures else f'{len(failures)} FAILED')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
