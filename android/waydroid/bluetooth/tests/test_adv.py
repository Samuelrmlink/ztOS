"""LE advertising for Android apps (stage 2): AD bytes to a BlueZ
LEAdvertisement1, the sets' lifetime, and the policy."""
import unittest

from fakes import FakeClient, P, make
from ztbt.core import Adv, adv_props, parse_ad

# flags (general discoverable), 16-bit UUID 180d, name, manufacturer 0x0059
AD = bytes.fromhex("020106" "03030d18" "0809" + b"zt-test".hex() + "05ff59000102")
# scan response: 128-bit UUID, service data for feaa, tx power
SCAN = (bytes.fromhex("1107") + bytes.fromhex("9ecadc240ee5a9e093f3a3b50100406e")
        + bytes.fromhex("0416aafe10") + bytes.fromhex("020afc"))


def adv(ad=AD, scan=b"", connectable=True, duration=0):
    return Adv(1, None, connectable, True, 160, 160, -7, ad, scan, duration)


class Props(unittest.TestCase):
    def test_fields(self):
        p = adv_props(adv(AD, SCAN))
        self.assertEqual(p["Type"], "peripheral")
        self.assertTrue(p["Discoverable"])
        self.assertEqual(p["LocalName"], "zt-test")
        self.assertEqual(p["ServiceUUIDs"], ["0000180d-0000-1000-8000-00805f9b34fb",
                                             "6e400001-b5a3-f393-e0a9-e50e24dcca9e"])
        self.assertEqual(p["ManufacturerData"], {0x59: b"\x01\x02"})
        self.assertEqual(p["ServiceData"], {"0000feaa-0000-1000-8000-00805f9b34fb": b"\x10"})
        self.assertEqual(p["Includes"], ["tx-power"])
        self.assertNotIn("Timeout", p)
        self.assertNotIn("Data", p)

    def test_broadcast_timeout_and_other_types(self):
        p = adv_props(adv(bytes.fromhex("020104" "032a0102"), connectable=False, duration=250))
        self.assertEqual(p["Type"], "broadcast")
        self.assertEqual(p["Timeout"], 3)
        self.assertEqual(p["Data"], {0x2a: b"\x01\x02"})
        self.assertNotIn("Discoverable", p)          # BR/EDR-not-supported only: not discoverable
        self.assertNotIn("ManufacturerData", p)

    def test_broadcast_never_discoverable(self):
        # BlueZ 5.87: "Broadcast cannot set flags" (found on the phone)
        p = adv_props(adv(AD, connectable=False))
        self.assertEqual(p["Type"], "broadcast")
        self.assertNotIn("Discoverable", p)

    def test_malformed(self):
        with self.assertRaises(ValueError):
            parse_ad(b"\x05\x09ab")
        self.assertEqual(parse_ad(b"\x02\x01\x06\x00\x00\x00"), [(1, b"\x06")])   # zero padding


class Ops(unittest.TestCase):
    def start(self, c, ad=AD, duration=0):
        return c.request("ADV_START", True, True, 160, 160, -7, ad, b"", duration)

    def started(self):
        be, store, br, c = make()
        rid = self.start(c)
        be.answer("advertise", value=-7)
        st, (adv_id, tx) = c.reply(rid, "ADV_START")
        self.assertEqual((st, tx), (P.ST_OK, -7))
        return be, store, br, c, adv_id

    def test_start(self):
        be, store, br, c, adv_id = self.started()
        (name, i, props), = be.called("advertise")
        self.assertEqual(i, adv_id)
        self.assertEqual(props["LocalName"], "zt-test")

    def test_too_long_for_bluez(self):
        be, store, br, c = make()
        rid = self.start(c)
        be.answer("advertise", error=("org.bluez.Error.InvalidLength", "Advertising data too long"))
        self.assertEqual(c.reply(rid, "ADV_START")[0], P.ST_INVALID)
        self.assertEqual(br.advs, {})

    def test_malformed_never_reaches_bluez(self):
        be, store, br, c = make()
        rid = self.start(c, ad=b"\x09\x09abc")
        self.assertEqual(c.reply(rid, "ADV_START")[0], P.ST_INVALID)
        self.assertEqual(be.called("advertise"), [])

    def test_leaves_instances_to_linux(self):
        be, store, br, c = make()
        for _ in range(4):
            self.start(c)
        rid = self.start(c)
        self.assertEqual(c.reply(rid, "ADV_START")[0], P.ST_BUSY)
        self.assertEqual(len(be.called("advertise")), 4)

    def test_set_data_reregisters(self):
        be, store, br, c, adv_id = self.started()
        rid = c.request("ADV_SET_DATA", adv_id, False, bytes.fromhex("0809") + b"renamed")
        self.assertEqual(be.called("unadvertise"), [("unadvertise", adv_id)])
        be.answer("advertise", value=-7)
        self.assertEqual(c.reply(rid, "ADV_SET_DATA")[0], P.ST_OK)
        self.assertEqual(be.called("advertise")[-1][2]["LocalName"], "renamed")

    def test_enable_off_on(self):
        be, store, br, c, adv_id = self.started()
        rid = c.request("ADV_ENABLE", adv_id, False, 0)
        self.assertEqual(c.reply(rid, "ADV_ENABLE")[0], P.ST_OK)
        self.assertEqual(be.called("unadvertise"), [("unadvertise", adv_id)])
        rid = c.request("ADV_ENABLE", adv_id, True, 300)
        be.answer("advertise", value=0)
        self.assertEqual(c.reply(rid, "ADV_ENABLE")[0], P.ST_OK)
        self.assertEqual(be.called("advertise")[-1][2]["Timeout"], 3)

    def test_stop(self):
        be, store, br, c, adv_id = self.started()
        self.assertEqual(c.reply(c.request("ADV_STOP", adv_id), "ADV_STOP")[0], P.ST_OK)
        self.assertEqual(br.advs, {})
        self.assertEqual(c.reply(c.request("ADV_STOP", adv_id), "ADV_STOP")[0], P.ST_NOT_FOUND)

    def test_timeout_from_bluez(self):
        be, store, br, c, adv_id = self.started()
        br.adv_released(adv_id)
        self.assertEqual(c.events("EV_ADV_STOPPED"), [[adv_id, P.ST_TIMEOUT]])

    def test_freeze_stops(self):
        be, store, br, c, adv_id = self.started()
        br.set_frozen(True)
        self.assertEqual(be.called("unadvertise"), [("unadvertise", adv_id)])
        self.assertEqual(c.events("EV_ADV_STOPPED"), [[adv_id, P.ST_NOT_READY]])
        rid = c.request("ADV_ENABLE", adv_id, True, 0)
        self.assertEqual(c.reply(rid, "ADV_ENABLE")[0], P.ST_NOT_READY)

    def test_client_gone_or_disabled(self):
        be, store, br, c, adv_id = self.started()
        c.request("ENABLE", False)
        self.assertEqual(be.called("unadvertise"), [("unadvertise", adv_id)])
        self.assertEqual(br.advs, {})
        be, store, br, c, adv_id = self.started()
        c.close()
        self.assertEqual(be.called("unadvertise"), [("unadvertise", adv_id)])
        self.assertEqual(br.advs, {})

    def test_other_clients_sets_are_not_yours(self):
        be, store, br, c, adv_id = self.started()
        other = FakeClient(br)
        other.request("HELLO", P.VERSION, "x")
        other.request("ENABLE", True)
        rid = other.request("ADV_STOP", adv_id)
        self.assertEqual(other.reply(rid, "ADV_STOP")[0], P.ST_NOT_FOUND)


if __name__ == "__main__":
    unittest.main()
