#!/usr/bin/env python3
"""ztoscsf-sensord's pocket checks for the always-on display, on the host:
nothing runs until the shell says "pocket on"; then a check (proximity and
light added, the first reading of each, both removed) at once and every
ZTOSCSF_POCKET_EVERY_MS; near in the dark is "pocket covered", anything else
"pocket clear", each said once; a check with no proximity reading changes
nothing; a check due while the SoC slept runs at the resume. The hub is a pty,
the shell a FIFO, the resume counter a file."""
import importlib.util
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
spec = importlib.util.spec_from_file_location('prox', HERE / 'test-sensord-prox.py')
prox = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prox)
Hub, Shell, check, failures, record = prox.Hub, prox.Shell, prox.check, prox.failures, prox.record
EVERY_MS = 2500
LIGHT, PROX = 9, 7


class Link:
    """Answers every header the daemon sends, and notes what came."""
    def __init__(self, hub):
        self.hub = hub
        self.seen = []		# (time, cmd, sensor or None)

    def serve(self, seconds):
        until = time.monotonic() + seconds
        while True:
            left = until - time.monotonic()
            if left <= 0:
                return
            t = self.hub.token(left)
            if not t or t[0] != 'header':
                continue
            head = t[1]
            length = struct.unpack_from('<H', head, 1)[0]
            if not length:
                self.seen.append((t[2], head[0], None))
                continue
            self.hub.echo(head)
            p = self.hub.token(1)
            payload = p[1] if p and p[0] == 'payload' else b''
            self.seen.append((t[2], head[0], payload[0] if payload else None))

    def take(self):
        if self.seen:
            self.last = max(t for t, _, _ in self.seen)	# when the last header came
        out, self.seen = [(c, s) for _, c, s in self.seen], []
        return out


def light(lux):
    return record(LIGHT, struct.pack('<Ii4HBB', lux, 4000, 1, 1, 1, 1, 0, 0))


def near(n):
    return record(PROX, bytes([n]) + struct.pack('<H', 3000 if n else 50))


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
        (root / 'run/zrnperfd.status').write_text('screen=off\n')
        (root / 'var/lib/ztoscsf-sensord/state').write_text('auto off\n')
        shell = Shell(root / 'run/user/1000/ztmetro.ctl')
        seq = d / 'resume_seq'
        seq.write_text('0\n')
        master, slave = pty.openpty()
        tty.setraw(slave)
        tty.setraw(master)
        env = dict(os.environ, ZTOSCSF_SSP=os.ttyname(slave), ZTOSCSF_RESUME_SEQ=str(seq),
                   ZTOSCSF_ROOT=str(root), ZTOSCSF_POCKET_EVERY_MS=str(EVERY_MS))
        err = open(d / 'stderr', 'w')
        proc = subprocess.Popen([str(exe)], env=env, stderr=err, stdout=err)
        hub = Hub(master)
        link = Link(hub)
        ctl = root / 'run/ztoscsf-sensord.ctl'
        out = root / 'run/ztoscsf-sensors'

        def say(line):
            fd = os.open(ctl, os.O_WRONLY)
            os.write(fd, (line + '\n').encode())
            os.close(fd)

        def pocket_line(timeout):
            until = time.monotonic() + timeout
            while True:
                link.serve(0.05)
                got = shell.line(0.05)
                if got and got[0].startswith('pocket'):
                    return got[0]
                if time.monotonic() > until:
                    return None

        def check_burst(what, frames, expect_line, timeout=2.5):
            """a check: both sensors added, the readings sent, both removed"""
            link.serve(0.4)	# the ADDs (thresholds first for proximity)
            added = link.take()
            check((0xa1, LIGHT) in added and (0xa1, PROX) in added and (0xf2, 0x09) in added,
                  f'{what}: light and proximity added, thresholds sent {added}')
            for f in frames:
                hub.frame(f)
            got = pocket_line(timeout) if expect_line else None
            if expect_line:
                check(got == expect_line, f'{what}: shell told {got!r}, expected {expect_line!r}')
            else:
                link.serve(timeout)
                check(shell.line(0.1) is None, f'{what}: the shell told nothing')
            link.serve(0.5)
            removed = link.take()
            check((0xa2, LIGHT) in removed and (0xa2, PROX) in removed,
                  f'{what}: both removed after {removed}')

        try:
            link.serve(1.5)
            started = link.take()
            check(not any(c == 0xa1 for c, _ in started), f'start, screen off: nothing added {started}')

            # nothing at all until the shell asks
            link.serve(1.0)
            check(not link.take(), 'no pocket asked: the hub hears nothing')

            # pocket on: a check at once; near in the dark
            t0 = time.monotonic()
            say('pocket on')
            check_burst('first check, covered', [near(1), light(0)], 'pocket covered')
            first_end = link.last	# the last REMOVE
            check(time.monotonic() - t0 < 2.0, f'the first check is at once ({time.monotonic() - t0:.2f} s)')
            check('pocket=on covered 1' in (out / 'status').read_text(), 'status: pocket=on covered 1')

            # nothing between checks
            link.serve(1.0)
            check(not link.take(), 'between checks: nothing on the hub')

            # the next one by itself, EVERY_MS later; still covered: nothing said
            t1 = time.monotonic()
            added = []
            while (0xa1, PROX) not in added and time.monotonic() - t1 < EVERY_MS / 1000 + 1.5:
                link.serve(0.05)
                added += link.take()
            gap = time.monotonic() - first_end
            check((0xa1, PROX) in added and EVERY_MS / 1000 - 0.3 <= gap <= EVERY_MS / 1000 + 0.6,
                  f'the next check by itself, {gap:.2f} s after the last ended (about {EVERY_MS} ms)')
            hub.frame(near(1) + light(1))
            link.serve(0.8)
            check(shell.line(0.2) is None, 'still covered: nothing said again')
            removed = link.take()
            check((0xa2, PROX) in removed, f'and removed {removed}')

            # near but a lit room: not a pocket (a hand over the sensor)
            say('pocket check')
            check_burst('near in a lit room', [near(1), light(300)], 'pocket clear')

            # far in the dark: clear, nothing said (already clear)
            say('pocket check')
            check_burst('far in the dark', [near(0), light(0)], None, timeout=0.8)

            # proximity never answers: nothing changes
            say('pocket check')
            link.serve(0.3)
            link.take()
            hub.frame(light(0))
            link.serve(1.6)
            removed = link.take()
            check((0xa2, PROX) in removed, f'no proximity reading: given up, removed {removed}')
            check(shell.line(0.2) is None, 'no proximity reading: the shell told nothing')

            # light never answers: proximity alone decides, after a moment
            say('pocket check')
            link.serve(0.3)
            link.take()
            hub.frame(near(1))
            got = pocket_line(1.5)
            check(got == 'pocket covered', f'no light reading: proximity alone ({got!r})')
            link.serve(0.4)
            link.take()

            # (a check due during a SoC sleep: CLOCK_BOOTTIME runs on while
            # poll's timeout does not, so it runs at the next wake of the loop,
            # the resume counter's notify among them. A host cannot suspend
            # under the test: that part is checked on the phone.)

            # pocket off: the checks stop, a burst under way ends
            say('pocket off')
            link.serve(EVERY_MS / 1000 + 0.8)
            seen = link.take()
            check(not any(c == 0xa1 for c, _ in seen), f'pocket off: no more checks {seen}')
            check('pocket=off' in (out / 'status').read_text(), 'status: pocket=off')

            # on again and covered, then the daemon stops: the AOD back
            say('pocket on')
            check_burst('on again', [near(1), light(0)], 'pocket covered')
            proc.send_signal(signal.SIGTERM)
            got = pocket_line(1.5)
            check(got == 'pocket clear', f'stopping while covered: {got!r}')
            link.serve(1.0)
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
