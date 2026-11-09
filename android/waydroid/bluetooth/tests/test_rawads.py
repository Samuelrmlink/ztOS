"""Advertisements as received (ztos-bt-adwatch): parsing its packets, and the
bridge giving Android those bytes rather than a rebuilt record."""
import struct
import unittest

from fakes import LOCK, SENSOR, P, make
from ztbt import rawads as R
from ztbt.core import build_ad

ADV = bytes.fromhex("020106") + bytes.fromhex("1107") + bytes(range(16))      # flags, a 128-bit UUID
RSP = bytes.fromhex("0809") + b"RM_2685"                                       # name, in the scan response


def packet(addr, data, flags=0, addr_type=2, rssi=-61, index=0):
    raw = bytes(int(x, 16) for x in reversed(addr.split(":")))
    return struct.pack("<H6sBbIH", index, raw, addr_type, rssi, flags, len(data)) + data


class Parse(unittest.TestCase):
    def test_device_found(self):
        self.assertEqual(R.parse(packet(SENSOR, ADV + RSP)), (0, SENSOR, 2, -61, 0, ADV + RSP))

    def test_bad_packets(self):
        good = packet(SENSOR, ADV)
        for bad in (b"", good[:10], good[:-1], good + b"x", packet(SENSOR, ADV, addr_type=3)):
            self.assertIsNone(R.parse(bad), bad)

    def test_event_types(self):
        self.assertEqual(R.event_type(0), 0x13)
        self.assertEqual(R.event_type(R.NOT_CONNECTABLE), 0x10)
        self.assertEqual(R.event_type(R.SCAN_RSP), 0x1b)
        self.assertEqual(R.event_type(R.SCAN_RSP | R.NOT_CONNECTABLE), 0x1a)

    def test_watch_passes_le_only(self):
        got = []
        w = R.Watch(lambda *a: got.append(a))
        w.feed(packet(SENSOR, ADV, flags=R.NOT_CONNECTABLE))
        w.feed(packet("AA:00:00:00:00:01", b"\x04\x0d\x14\x04\x24", addr_type=0))   # BR/EDR inquiry
        w.feed(b"junk")
        self.assertEqual(got, [(SENSOR, 2, -61, R.NOT_CONNECTABLE, ADV)])


class Merge(unittest.TestCase):
    def scanning(self, version=2):
        be, store, br, c = make(version)
        c.request("SCAN_START", P.T_LE)
        self.assertEqual(be.called("watch_ads"), [("watch_ads", True)])
        return be, store, br, c

    def found(self, c, version=2):
        op = "EV_DEVICE_FOUND2" if version == 2 else "EV_DEVICE_FOUND"
        return [e for e in c.events(op) if e[0][0] == SENSOR]

    def test_rebuilt_until_a_raw_one_comes(self):
        be, store, br, c = self.scanning()
        br.device_changed(SENSOR, {"RSSI": -60})
        (ev,) = self.found(c)
        self.assertEqual(ev[3], 0x13)
        self.assertEqual(ev[4], build_ad(br.devs[SENSOR]))
        br.raw_ad(SENSOR, 2, -58, 0, ADV + RSP)
        ev = self.found(c)[-1]
        self.assertEqual((ev[1], ev[3], ev[4]), (-58, 0x13, ADV + RSP))
        br.device_changed(SENSOR, {"RSSI": -55})       # BlueZ's RSSI updates carry it too
        ev = self.found(c)[-1]
        self.assertEqual((ev[1], ev[4]), (-55, ADV + RSP))

    def test_exact_bytes_even_past_62(self):
        be, store, br, c = self.scanning()
        br.device_changed(SENSOR, {"RSSI": -60})
        odd = ADV + RSP + bytes(40) + b"\x05\xff\x01"       # padding and a broken structure: as sent
        br.raw_ad(SENSOR, 2, -60, R.NOT_CONNECTABLE, odd)
        ev = self.found(c)[-1]
        self.assertEqual((ev[3], ev[4]), (0x10, odd))

    def test_a_lone_scan_response_follows_its_advertisement(self):
        be, store, br, c = self.scanning()
        br.device_changed(SENSOR, {"RSSI": -60})
        br.raw_ad(SENSOR, 2, -60, 0, ADV)
        br.raw_ad(SENSOR, 2, -60, R.SCAN_RSP, RSP)
        ev = self.found(c)[-1]
        self.assertEqual((ev[3], ev[4]), (0x1b, ADV + RSP))

    def test_before_bluez_knows_it(self):
        be, store, br, c = self.scanning()
        br.raw_ad("F0:00:00:00:00:09", 2, -60, 0, ADV)     # no Device1 yet: kept, not sent
        self.assertEqual(c.events("EV_DEVICE_FOUND2"), [])
        br.device_changed("F0:00:00:00:00:09", {"Address": "F0:00:00:00:00:09", "AddressType": "random",
                                                "RSSI": -60, "Paired": False, "Connected": False})
        (ev,) = [e for e in c.events("EV_DEVICE_FOUND2") if e[0][0] == "F0:00:00:00:00:09"]
        self.assertEqual(ev[4], ADV)

    def test_hidden_devices_never(self):
        be, store, br, c = self.scanning()
        br.raw_ad("AA:00:00:00:00:01", 1, -60, 0, ADV)     # the AirPods: Linux's
        self.assertEqual([e for e in c.events("EV_DEVICE_FOUND2") if e[0][0] == "AA:00:00:00:00:01"], [])

    def test_connected_devices_get_their_last_advertisement(self):
        from ztbt.core import SILENT_SECONDS
        be, store, br, c = self.scanning()
        br.device_changed(LOCK, {"RSSI": -50})
        br.raw_ad(LOCK, 1, -50, 0, ADV + RSP)
        br.device_changed(LOCK, {"Connected": True})       # Settings left it connected: it's silent now
        be.fire(SILENT_SECONDS)
        ev = [e for e in c.events("EV_DEVICE_FOUND2") if e[0][0] == LOCK][-1]
        self.assertEqual(ev[4], ADV + RSP)

    def test_a_version_1_shim_gets_the_old_event(self):
        be, store, br, c = self.scanning(version=1)
        br.device_changed(SENSOR, {"RSSI": -60})
        br.raw_ad(SENSOR, 2, -58, 0, ADV)
        self.assertEqual(self.found(c, 1)[-1][3], ADV)
        self.assertEqual(c.events("EV_DEVICE_FOUND2"), [])

    def test_stops_with_the_scan(self):
        be, store, br, c = self.scanning()
        c.request("SCAN_STOP", P.T_LE)
        self.assertEqual(be.called("watch_ads")[-1], ("watch_ads", False))

    def test_forgotten_with_the_device(self):
        be, store, br, c = self.scanning()
        br.raw_ad(SENSOR, 2, -60, 0, ADV)
        br.device_removed(SENSOR)
        self.assertNotIn(SENSOR, br.raw)


if __name__ == "__main__":
    unittest.main()
