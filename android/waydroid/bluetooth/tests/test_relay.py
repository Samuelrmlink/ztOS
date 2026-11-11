"""The RFCOMM relay: both ways, the signal first, back-pressure, ends."""
import socket
import time
import unittest

from fakes import P  # noqa: F401  (sets sys.path)

try:
    from gi.repository import GLib
    from ztbt.relay import LIMIT, Relay
except ImportError:
    GLib = None


def spin(until, timeout=5):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + timeout
    while not until() and time.monotonic() < end:
        ctx.iteration(False)
        time.sleep(0.001)
    return until()


def recv_all(s, n, timeout=5):
    s.setblocking(False)
    got = bytearray()

    def more():
        try:
            data = s.recv(65536)
            if data:
                got.extend(data)
        except BlockingIOError:
            pass
        return len(got) >= n
    spin(more, timeout)
    return bytes(got)


@unittest.skipIf(GLib is None, "PyGObject missing")
class RelayTests(unittest.TestCase):
    def setUp(self):
        self.android, a = socket.socketpair()
        self.printer, b = socket.socketpair()
        self.closed = []
        self.relay = Relay(a.detach(), b.detach(), b"SIGNAL", lambda: self.closed.append(1))

    def tearDown(self):
        self.relay.close()
        self.android.close()
        self.printer.close()

    def test_signal_then_both_ways(self):
        self.assertEqual(recv_all(self.android, 6), b"SIGNAL")
        self.android.sendall(b"print this")
        self.assertEqual(recv_all(self.printer, 10), b"print this")
        self.printer.sendall(b"ok")
        self.assertEqual(recv_all(self.android, 2), b"ok")

    def test_big_job_with_a_slow_printer(self):
        recv_all(self.android, 6)
        job = bytes(range(256)) * 4096          # 1 MiB, more than LIMIT
        self.assertGreater(len(job), LIMIT)
        self.android.setblocking(False)
        sent = 0
        got = bytearray()
        self.printer.setblocking(False)

        def step():
            nonlocal sent
            if sent < len(job):
                try:
                    sent += self.android.send(job[sent:sent + 65536])
                except BlockingIOError:
                    pass
            try:
                got.extend(self.printer.recv(8192))   # slow reader
            except BlockingIOError:
                pass
            return len(got) == len(job)
        self.assertTrue(spin(step, 20))
        self.assertEqual(bytes(got), job)

    def test_android_closes_after_writing(self):
        recv_all(self.android, 6)
        self.android.sendall(b"last bytes")
        self.android.close()
        self.assertEqual(recv_all(self.printer, 10), b"last bytes")
        self.assertTrue(spin(lambda: self.closed))
        self.printer.settimeout(1)
        self.assertEqual(self.printer.recv(10), b"")

    def test_printer_goes_away(self):
        recv_all(self.android, 6)
        self.printer.close()
        self.assertTrue(spin(lambda: self.closed))
        self.android.settimeout(1)
        self.assertEqual(self.android.recv(10), b"")

    def test_close_does_not_call_back(self):
        self.relay.close()
        spin(lambda: False, 0.05)
        self.assertEqual(self.closed, [])


if __name__ == "__main__":
    unittest.main()
