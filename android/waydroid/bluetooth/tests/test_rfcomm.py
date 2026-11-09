"""Classic RFCOMM through the bridge (stage 3): Android's BluetoothSocket on a
socketpair, BlueZ's RFCOMM link behind it, policy as for GATT."""
import os
import socket
import struct
import unittest

from fakes import AIRPODS, PRINTER, SPEAKER, SPP, FakeClient, P, make

NO_UUID = "00000000-0000-0000-0000-000000000000"


def pair():
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    return a, b.detach()      # Android's end (a socket), the bridge's (an fd)


class Rfcomm(unittest.TestCase):
    def connect(self, addr=PRINTER, uuid=SPP, channel=-1, flags=0x03):
        be, store, br, c = make()
        android, fd = pair()
        rid = c.request("RFCOMM_CONNECT", addr, uuid, channel, flags, fds=[fd])
        return be, store, br, c, android, rid

    def test_by_uuid_then_signal_and_relay(self):
        be, store, br, c, android, rid = self.connect()
        st, (sid,) = c.reply(rid, "RFCOMM_CONNECT")
        self.assertEqual(st, P.ST_OK)
        self.assertEqual(be.called("rfcomm"), [("rfcomm", PRINTER, SPP, -1, True)])
        self.assertEqual(store.expect, {PRINTER})       # BlueZ's agent may ask about it
        printer, rfd = pair()
        be.answer("rfcomm", values=(rfd, 1))
        (name, a, b, prefix), = be.called("relay")
        self.assertEqual(b, rfd)
        chan, = struct.unpack("<i", prefix[:4])
        size, addr, chan2, status, tx, rx = struct.unpack("<h6siiHH", prefix[4:])
        self.assertEqual((chan, size, chan2, status), (1, 20, 1, 0))
        self.assertEqual(P.addr_text(addr), PRINTER)
        self.assertEqual(len(prefix), 24)
        self.assertIn(PRINTER, br.holds)
        # Android closes: the relay says so, the device is let go
        be.relays[0]["on_close"]()
        self.assertEqual(br.streams, {})
        be.linger()
        self.assertNotIn(PRINTER, br.holds)
        self.assertEqual(be.called("disconnect"), [("disconnect", PRINTER)])
        android.close()
        printer.close()

    def test_pair_then_connect_at_once(self):
        # the PM220 on 8 Nov: Nelko pairs, then opens its RFCOMM socket 1.3 s
        # later. The device Pair() connected must not be disconnected under it.
        be, store, br, c = make()
        rid = c.request("BOND_CREATE", PRINTER, P.T_BREDR)
        self.assertEqual(c.reply(rid, "BOND_CREATE")[0], P.ST_OK)
        br.device_changed(PRINTER, {"Connected": True})
        br.device_changed(PRINTER, {"Paired": True, "Bonded": True})
        be.answer("pair")
        android, fd = pair()
        rid = c.request("RFCOMM_CONNECT", PRINTER, SPP, -1, 0x03, fds=[fd])
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_OK)
        self.assertEqual(be.linger(), 0)                  # cancelled by the new use
        self.assertEqual(be.called("disconnect"), [])
        self.assertEqual(be.called("rfcomm"), [("rfcomm", PRINTER, SPP, -1, True)])
        android.close()

    def test_client_gone_lets_go_at_once(self):
        be, store, br, c, android, rid = self.connect()
        printer, rfd = pair()
        be.answer("rfcomm", values=(rfd, 1))
        be.relays[0]["on_close"]()
        c.close()                                         # Android's Bluetooth is gone
        self.assertEqual(be.called("disconnect"), [("disconnect", PRINTER)])
        android.close()
        printer.close()

    def test_failure_closes_androids_end(self):
        be, store, br, c, android, rid = self.connect()
        be.answer("rfcomm", error=("org.bluez.Error.Failed", "Host is down"))
        android.settimeout(1)
        self.assertEqual(android.recv(10), b"")     # EOF: Android throws IOException
        self.assertEqual(store.expect, set())
        self.assertNotIn(PRINTER, br.holds)
        android.close()

    def test_by_channel(self):
        be, store, br, c, android, rid = self.connect(uuid=NO_UUID, channel=1, flags=0)
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_OK)
        self.assertEqual(be.called("rfcomm"), [("rfcomm", PRINTER, None, 1, False)])
        android.close()

    def test_needs_uuid_or_channel(self):
        be, store, br, c, android, rid = self.connect(uuid=NO_UUID, channel=-1)
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_INVALID)
        android.settimeout(1)
        self.assertEqual(android.recv(10), b"")
        android.close()

    def test_linux_connected_device_busy(self):
        be, store, br, c, android, rid = self.connect(addr=AIRPODS)
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_BUSY)
        self.assertEqual(be.called("rfcomm"), [])
        android.close()

    def test_bonded_device_expected_too(self):
        # a bonded device may need pairing again (lost keys): the person decides
        be, store, br, c, android, rid = self.connect(addr=SPEAKER)
        self.assertEqual(store.expect, {SPEAKER})
        android.close()

    def test_unknown_device_by_uuid(self):
        be, store, br, c, android, rid = self.connect(addr="F1:F1:F1:F1:F1:F1")
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_NOT_FOUND)
        android.close()

    def test_freeze_closes_streams(self):
        be, store, br, c, android, rid = self.connect()
        printer, rfd = pair()
        be.answer("rfcomm", values=(rfd, 1))
        br.set_frozen(True)
        self.assertIn(("unrelay",), be.calls)
        self.assertEqual(br.streams, {})
        self.assertEqual(be.called("disconnect"), [("disconnect", PRINTER)])
        android.close()
        printer.close()

    def test_remote_disconnect_closes_streams(self):
        be, store, br, c, android, rid = self.connect()
        printer, rfd = pair()
        be.answer("rfcomm", values=(rfd, 1))
        br.device_changed(PRINTER, {"Connected": True})
        br.device_changed(PRINTER, {"Connected": False})
        self.assertEqual(br.streams, {})
        android.close()
        printer.close()

    def test_connect_finishing_after_close(self):
        be, store, br, c, android, rid = self.connect()
        br.set_frozen(True)                    # stream ended while BlueZ connects
        printer, rfd = pair()
        be.answer("rfcomm", values=(rfd, 1))
        self.assertEqual(be.called("relay"), [])
        with self.assertRaises(OSError):
            os.fstat(rfd)                      # closed, not leaked
        android.close()
        printer.close()

    def test_fd_rules(self):
        be, store, br, c = make()
        # RFCOMM_CONNECT without its fd: the connection is dropped
        packet = P.encode("RFCOMM_CONNECT", 9, P.typed("aUiu", [PRINTER, SPP, -1, 0]))
        br.handle(c, packet)
        self.assertTrue(c.closed)
        # an fd with an op that takes none: closed, connection dropped
        be, store, br, c = make()
        a, fd = pair()
        br.handle(c, P.encode("BONDED_LIST", 3, [], P.FLAG_FD), [fd])
        self.assertTrue(c.closed)
        with self.assertRaises(OSError):
            os.fstat(fd)
        a.close()

    def test_not_enabled(self):
        be, store, br, c = make()
        c.request("ENABLE", False)
        android, fd = pair()
        rid = c.request("RFCOMM_CONNECT", PRINTER, SPP, -1, 0, fds=[fd])
        self.assertEqual(c.reply(rid, "RFCOMM_CONNECT")[0], P.ST_NOT_READY)
        android.close()


if __name__ == "__main__":
    unittest.main()
