#!/usr/bin/env python3
"""ztoscsf-sensord's proximity in a call, on the host: the sensor on only for
a call held to the ear with the screen on, the stock thresholds before its
ADD, near and far each held before the shell hears "prox_blank on|off", and
the screen left on whenever the sensor, the call or the daemon goes. Also the
pressure and proximity records' sizes. The hub is a pty, the shell a FIFO."""
import importlib.util
import os
import pty
import select
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
spec = importlib.util.spec_from_file_location('queue', HERE / 'test-sensord-queue.py')
queue = importlib.util.module_from_spec(spec)
spec.loader.exec_module(queue)
Hub, check, failures = queue.Hub, queue.check, queue.failures


def header(hub, timeout=1.5):
    """the next header; the time message (sent every 16 frames) is skipped"""
    until = time.monotonic() + timeout
    while True:
        t = hub.token(max(0.0, until - time.monotonic()))
        if not t or t[0] != 'time':
            return t if t and t[0] == 'header' else (t or ('none', b'', 0))


class Shell:
    """The shell's control pipe: what sensord tells it, line by line."""
    def __init__(self, path):
        os.mkfifo(path)
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)	# never an EOF
        self.buf = b''

    def line(self, timeout):
        until = time.monotonic() + timeout
        while b'\n' not in self.buf:
            left = until - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                self.buf += os.read(self.fd, 4096)
        out, self.buf = self.buf.split(b'\n', 1)
        return out.decode(), time.monotonic()

    def prox(self, timeout):
        """the next prox_blank line, skipping brightness and the like"""
        until = time.monotonic() + timeout
        while True:
            got = self.line(until - time.monotonic())
            if got is None or got[0].startswith('prox_blank'):
                return got


def record(sensor, data):
    return bytes([0x37, sensor]) + data + struct.pack('<q', 0)


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
        for sub in ('run/user/1000', 'var/lib', 'run/ztoscorenetworking'):
            (root / sub).mkdir(parents=True)
        (root / 'run/zrnperfd.status').write_text('screen=on\n')
        (root / 'var/lib/ztoscsf-sensord').mkdir()
        (root / 'var/lib/ztoscsf-sensord/state').write_text('auto off\n')
        (root / 'run/ztoscorenetworking/status').write_text('call_result=\ncall=idle\ncall_number=\n')
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

        def prox_on(what):
            txn(what + ': stock thresholds first', 0xf2,
                lambda p: p == bytes([0x09, 0x60, 0x06, 0x40, 0x3f, 0xf0, 0x03, 0xe8]))
            txn(what + ': ADD prox', 0xa1, lambda p: p[0] == 7 and len(p) == 10)

        def prox_off(what):
            txn(what + ': REMOVE prox', 0xa2, lambda p: p[0] == 7 and len(p) == 5)

        def near(n, adc=3000):
            hub.frame(record(7, bytes([n]) + struct.pack('<H', adc)))

        try:
            t = hub.token(3)
            check(t is not None and t[0] == 'time', 'start: the time')
            txn('start', 0xa1, lambda p: p[0] == 9)

            # an incoming call ringing: in no one's ear, the sensor stays off
            say('call incoming earpiece')
            check(header(hub, 0.4)[0] == 'none', 'incoming: proximity stays off')

            # answered: on, with the thresholds before its ADD
            say('call active earpiece')
            prox_on('active')
            status = (out / 'status').read_text()
            check('call=active earpiece' in status and 'prox=on' in status, 'status: call and prox ' + repr(status))

            # near: held 150 ms, then the shell hears it
            t0 = time.monotonic()
            near(1, 4000)
            got = shell.prox(1.5)
            check(got is not None and got[0] == 'prox_blank on' and 0.12 <= got[1] - t0 <= 0.5,
                  f'near: "prox_blank on" after {got[1] - t0 if got else -1:.3f} s (0.15)')
            check((out / 'prox').read_text() == 'near 4000\n', 'prox file: near 4000')
            check('prox=near' in (out / 'status').read_text(), 'status: prox=near')

            # a blip of far shorter than the hold: nothing
            near(0, 100)
            time.sleep(0.1)
            near(1, 4000)
            check(shell.prox(0.6) is None, 'far for 0.1 s: the panel stays off')

            # far: held 300 ms
            t0 = time.monotonic()
            near(0, 100)
            got = shell.prox(1.5)
            check(got is not None and got[0] == 'prox_blank off' and 0.27 <= got[1] - t0 <= 0.7,
                  f'far: "prox_blank off" after {got[1] - t0 if got else -1:.3f} s (0.3)')

            # a near blip shorter than its hold: nothing
            near(1)
            time.sleep(0.05)
            near(0)
            check(shell.prox(0.5) is None, 'near for 0.05 s: the panel stays on')

            # near, then the speaker: the sensor off, the panel back at once
            near(1)
            got = shell.prox(1)
            check(got is not None and got[0] == 'prox_blank on', 'near again: on')
            say('call active speaker')
            got = shell.prox(0.3)
            check(got is not None and got[0] == 'prox_blank off', 'speaker: "prox_blank off" at once')
            prox_off('speaker')
            say('call active bluetooth')
            check(header(hub, 0.3)[0] == 'none', 'a headset: proximity stays off')

            # back to the earpiece, near, then the screen goes off (power key)
            say('call active earpiece')
            prox_on('earpiece again')
            near(1)
            got = shell.prox(1)
            check(got is not None and got[0] == 'prox_blank on', 'near: on')
            say('screen off')
            got = shell.prox(0.3)
            check(got is not None and got[0] == 'prox_blank off', 'screen off: "prox_blank off"')
            kinds = []
            for _ in range(2):	# light and prox come off, one at a time
                kind, h, _ = header(hub, 2)
                if kind == 'header':
                    hub.echo(h)
                    t = hub.token(1)
                    kinds.append((h[0], t[1][0] if t else None))
            check(sorted(kinds) == [(0xa2, 7), (0xa2, 9)], f'screen off: light and prox removed {kinds}')
            say('screen on')
            got = [txn('screen on', 0xa1, lambda p: p[0] == 9)]
            prox_on('screen on')

            # the hub restarts while near: the panel back
            near(1)
            got = shell.prox(1)
            check(got is not None and got[0] == 'prox_blank on', 'near: on before the hub restarts')
            hub.frame(bytes([0x0a]))
            got = shell.prox(0.3)
            check(got is not None and got[0] == 'prox_blank off', 'hub restart: "prox_blank off"')
            txn('hub restart', 0xa1, lambda p: p[0] == 9)
            prox_on('hub restart')

            # never a reading: nothing told, the screen stays on
            check(shell.prox(0.5) is None, 'no reading: the shell hears nothing')

            # the pressure record: 6 bytes, then the next record still parses
            raw = int(1013.25 * 4096)
            hub.frame(record(5, struct.pack('<ih', raw, 2345)) + record(7, bytes([1]) + struct.pack('<H', 5)))
            got = shell.prox(1)
            time.sleep(0.05)
            check((out / 'pressure').read_text() == f'1013.25 {raw} 23.45\n',
                  'pressure file: hPa, raw, degrees ' + repr((out / 'pressure').read_text()))
            check(got is not None and got[0] == 'prox_blank on', 'a record after pressure parses (near)')

            # the call ends while near: the panel back at once
            say('call idle off')
            got = shell.prox(0.3)
            check(got is not None and got[0] == 'prox_blank off', 'call over: "prox_blank off" at once')
            prox_off('call over')

            # dialling: on again; then sensord stops while near
            say('call dialing earpiece')
            prox_on('dialing')
            near(1)
            got = shell.prox(1)
            check(got is not None and got[0] == 'prox_blank on', 'dialing, near: on')
            proc.send_signal(signal.SIGTERM)
            got = shell.prox(1)
            check(got is not None and got[0] == 'prox_blank off', 'stopping: "prox_blank off"')
            # (it takes its sensors off as it goes; answer them so it can)
            for _ in range(2):
                kind, h, _ = header(hub, 1)
                if kind == 'header':
                    hub.echo(h)
                    hub.token(0.5)
            proc.wait(3)
            check(proc.returncode == 0, 'stops cleanly')

            # a restart in a call reads the modem's status
            (root / 'run/ztoscorenetworking/status').write_text(
                'call_result=\ncall=active\ncall_number=1\ncall_audio=earpiece\n')
            err2 = open(d / 'stderr2', 'w')
            proc = subprocess.Popen([str(exe)], env=env, stderr=err2, stdout=err2)
            hub.token(3)
            seen = []
            for _ in range(3):
                kind, h, _ = header(hub, 2)
                if kind != 'header':
                    break
                seen.append(h[0])
                hub.echo(h)
                hub.token(1)
            check(seen == [0xa1, 0xf2, 0xa1], f'restart in a call: light, thresholds, prox {seen}')
            proc.send_signal(signal.SIGTERM)
            for _ in range(2):
                kind, h, _ = header(hub, 1)
                if kind == 'header':
                    hub.echo(h)
                    hub.token(0.5)
            proc.wait(3)
            err2.close()
        finally:
            if proc.poll() is None:
                proc.kill()
            err.close()
            print((d / 'stderr').read_text(), end='')
    print('PASS' if not failures else f'{len(failures)} FAILED')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
