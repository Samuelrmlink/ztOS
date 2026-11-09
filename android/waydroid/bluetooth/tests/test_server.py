"""ztos-btbridge's socket: who may connect, packets in and out, a reader that stalls."""
import os
import socket
import struct
import tempfile
import time
import unittest

from fakes import P  # noqa: F401  (sets sys.path)

try:
    from gi.repository import GLib
    from ztbt import server as S
except ImportError:      # no PyGObject: these tests need it
    GLib = None


class FakeBridge:
    def __init__(self):
        self.packets = []
        self.attached = []
        self.detached = []
        self.frozen = []

    def handle(self, client, packet, fds=()):
        self.packets.append(packet)
        self.fds = list(fds)
        client.send(b"echo" + packet)

    def attach(self, c):
        self.attached.append(c)

    def detach(self, c):
        self.detached.append(c)

    def set_frozen(self, f):
        self.frozen.append(f)


def spin(n=50):
    ctx = GLib.MainContext.default()
    for _ in range(n):
        ctx.iteration(False)


@unittest.skipIf(GLib is None, "PyGObject missing")
class ServerTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "misc", "ztos-btbridge", "bridge.sock")
        self.bridge = FakeBridge()

    def tearDown(self):
        self.dir.cleanup()

    def server(self, uid):
        s = S.Server(self.bridge, self.path, allow_uid=uid, container_check=False)
        s.listen()
        return s

    def connect(self):
        c = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        c.connect(self.path)
        spin()
        return c

    def test_modes(self):
        self.server(os.getuid())
        self.assertEqual(os.stat(os.path.dirname(self.path)).st_mode & 0o777, 0o711)
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o666)

    def test_wrong_uid_refused(self):
        self.server(os.getuid() + 1)
        c = self.connect()
        c.settimeout(1)
        self.assertEqual(c.recv(10), b"")
        self.assertEqual(self.bridge.attached, [])

    def test_packets_round_trip_and_close_detaches(self):
        srv = self.server(os.getuid())
        c = self.connect()
        self.assertEqual(len(self.bridge.attached), 1)
        c.send(b"\x02\x00\x00\x00\x01\x00\x00\x00")
        spin()
        self.assertEqual(c.recv(100), b"echo\x02\x00\x00\x00\x01\x00\x00\x00")
        c.close()
        spin()
        self.assertEqual(len(self.bridge.detached), 1)
        self.assertEqual(srv.clients, set())

    def test_stalled_reader_drops_droppable_and_queues_the_rest(self):
        srv = self.server(os.getuid())
        c = self.connect()
        client, = srv.clients
        big = b"x" * 60000
        sent = 0
        while not client.queue and sent < 1000:      # fill the socket buffer
            client.send(big, droppable=False)
            sent += 1
        self.assertTrue(client.queue)
        queued = len(client.queue)
        client.send(b"scan result", droppable=True)
        self.assertEqual(len(client.queue), queued)
        self.assertEqual(client.dropped, 1)
        client.send(b"link down", droppable=False)
        self.assertEqual(client.queue[-1], b"link down")
        got = []
        c.setblocking(False)
        for _ in range(sent + 10):
            spin(5)
            try:
                while True:
                    got.append(c.recv(65536))
            except BlockingIOError:
                pass
            if got and got[-1] == b"link down":
                break
        self.assertEqual(got[-1], b"link down")
        self.assertEqual(len(got), sent + 1)

    def test_an_fd_comes_with_its_packet(self):
        self.server(os.getuid())
        c = self.connect()
        a, b = socket.socketpair()
        c.sendmsg([b"\x50\x00\x01\x00\x01\x00\x00\x00"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                                struct.pack("i", b.fileno()))])
        spin()
        fd, = self.bridge.fds
        os.write(fd, b"through")
        self.assertEqual(a.recv(10), b"through")
        os.close(fd)
        a.close()
        b.close()
        c.close()

    def test_the_agent_reads_what_the_store_writes(self):
        import importlib.machinery
        import importlib.util
        from pathlib import Path
        src = Path(__file__).resolve().parents[4] / "rootfs/overlay/usr/local/bin/ztos-bt-agent"
        loader = importlib.machinery.SourceFileLoader("ztos_bt_agent", str(src))
        agent = importlib.util.module_from_spec(importlib.util.spec_from_loader("ztos_bt_agent", loader))
        loader.exec_module(agent)
        st = S.Store(os.path.join(self.dir.name, "run"), os.path.join(self.dir.name, "state"))
        st.expected(["b0:e8:e8:92:bb:fd"], 60)
        self.assertTrue(agent.expected(st.expected_path, "B0:E8:E8:92:BB:FD"))
        self.assertFalse(agent.expected(st.expected_path, "C0:00:00:00:00:02"))
        self.assertFalse(agent.expected(st.expected_path, "B0:E8:E8:92:BB:FD", now=time.time() + 61))
        st.expected([], 60)
        self.assertFalse(agent.expected(st.expected_path, "B0:E8:E8:92:BB:FD"))

    def test_in_container(self):
        self.assertFalse(S.in_container(os.getpid()))

    def test_store(self):
        old = os.path.join(self.dir.name, "run", "ztos-btbridge", "pairing")
        os.makedirs(os.path.dirname(old))
        open(old, "w").close()
        st = S.Store(os.path.join(self.dir.name, "run"), os.path.join(self.dir.name, "state"),
                     clock=lambda: 1000.4)
        self.assertFalse(os.path.exists(old))                  # the older note goes
        st.expected(["aa:bb:cc:dd:ee:ff", "11:22:33:44:55:66"], 60)
        with open(st.expected_path) as f:
            self.assertEqual(f.read(), "AA:BB:CC:DD:EE:FF 1060\n11:22:33:44:55:66 1060\n")
        self.assertEqual(os.stat(st.expected_path).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(os.path.dirname(st.expected_path)).st_mode & 0o777, 0o700)
        st.expected([], 60)
        self.assertFalse(os.path.exists(st.expected_path))
        st.add_android_bond("aa:bb:cc:dd:ee:ff")
        self.assertTrue(st.is_android_bond("AA:BB:CC:DD:EE:FF"))
        st.forget_android_bond("AA:BB:CC:DD:EE:FF")
        self.assertFalse(st.is_android_bond("AA:BB:CC:DD:EE:FF"))


if __name__ == "__main__":
    unittest.main()
