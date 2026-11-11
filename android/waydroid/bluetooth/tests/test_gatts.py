"""Android's GATT servers through BlueZ (stage 2): the object tree, requests
and responses, CCCDs, notifications, connections, lifetime."""
import unittest

from fakes import AIRPODS, SENSOR, FakeClient, P, make
from ztbt.gatts import ServerSvc, server_tree

HR = "0000180d-0000-1000-8000-00805f9b34fb"
HRM = "00002a37-0000-1000-8000-00805f9b34fb"
CTRL = "00002a39-0000-1000-8000-00805f9b34fb"
CCCD = "00002902-0000-1000-8000-00805f9b34fb"
DESC = "00002901-0000-1000-8000-00805f9b34fb"

# heart rate: measurement (notify) with its CCCD, control point (write, encrypted), a user description
ELEMENTS = [(P.DB_PRIMARY, HR, 0, 0),
            (P.DB_CHARACTERISTIC, HRM, 0x12, 0x01),
            (P.DB_DESCRIPTOR, CCCD, 0, 0x11),
            (P.DB_CHARACTERISTIC, CTRL, 0x08, 0x20),
            (P.DB_DESCRIPTOR, DESC, 0, 0x01)]


class Tree(unittest.TestCase):
    def test_shape_and_flags(self):
        svc = ServerSvc(1, None, ELEMENTS, [10, 11, 12, 13, 14])
        tree = server_tree(svc)
        self.assertEqual([n["kind"] for n in tree], ["service", "char", "char", "desc"])   # no CCCD
        self.assertEqual(tree[1]["flags"], ["notify", "read"])
        self.assertEqual(tree[2]["flags"], ["encrypt-write", "write"])
        self.assertEqual(tree[3]["parent"], 13)
        self.assertEqual(svc.cccd, {11: 12})

    def test_refused_shapes(self):
        for els in ([(P.DB_CHARACTERISTIC, HRM, 2, 1)],
                    [(P.DB_PRIMARY, HR, 0, 0), (P.DB_DESCRIPTOR, DESC, 0, 1)],
                    [(P.DB_PRIMARY, HR, 0, 0), (P.DB_PRIMARY, HR, 0, 0)],
                    [(P.DB_PRIMARY, HR, 0, 0), (P.DB_INCLUDED, HR, 0, 0)]):
            with self.assertRaises(ValueError):
                server_tree(ServerSvc(1, None, els, list(range(1, len(els) + 1))))


class Server(unittest.TestCase):
    def added(self):
        be, store, br, c = make()
        rid = c.request("SERVER_ADD", ELEMENTS)
        (name, svc_id, tree), = [x[:3] for x in be.called("serve")]
        be.answer("serve")
        st, (sid, handles) = c.reply(rid, "SERVER_ADD")
        self.assertEqual(st, P.ST_OK)
        self.assertEqual(len(handles), len(ELEMENTS))
        return be, store, br, c, sid, handles

    def test_add(self):
        be, store, br, c, sid, handles = self.added()
        self.assertEqual(handles, [1, 2, 3, 4, 5])
        # a second service gets handles of its own
        rid = c.request("SERVER_ADD", ELEMENTS[:2])
        be.answer("serve")
        self.assertEqual(c.reply(rid, "SERVER_ADD")[1][1], [6, 7])

    def test_bluez_refuses(self):
        be, store, br, c = make()
        rid = c.request("SERVER_ADD", ELEMENTS)
        be.answer("serve", error=("org.bluez.Error.Failed", "Failed to create entry in database"))
        self.assertEqual(c.reply(rid, "SERVER_ADD")[0], P.ST_FAIL)
        self.assertEqual(br.services, {})

    def test_bad_shape(self):
        be, store, br, c = make()
        rid = c.request("SERVER_ADD", ELEMENTS[1:])
        self.assertEqual(c.reply(rid, "SERVER_ADD")[0], P.ST_INVALID)
        self.assertEqual(be.called("serve"), [])

    def test_read_answered_by_android(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        got = []
        br.server_read(sid, hm, SENSOR, 0, 185, lambda v: got.append(v), lambda e: got.append(("err", e)))
        (conn, addr, up), = c.events("EV_SERVER_CONN")
        self.assertEqual((addr, up), (SENSOR, True))
        self.assertEqual(c.events("EV_SERVER_MTU"), [[conn, 185]])
        (conn2, req, handle, offset), = c.events("EV_SERVER_READ")
        self.assertEqual((conn2, handle, offset), (conn, hm, 0))
        self.assertEqual(got, [])
        rid = c.request("SERVER_RESPONSE", req, 0, b"\x00\x48")
        self.assertEqual(c.reply(rid, "SERVER_RESPONSE")[0], P.ST_OK)
        self.assertEqual(got, [b"\x00\x48"])
        # the same central again: no second connection event
        br.server_read(sid, hm, SENSOR, 0, 185, lambda v: None, lambda e: None)
        self.assertEqual(len(c.events("EV_SERVER_CONN")), 1)

    def test_att_error_and_late_answer(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        got = []
        br.server_read(sid, hm, SENSOR, 0, 0, got.append, lambda e: got.append(("err", e)))
        req = c.events("EV_SERVER_READ")[0][1]
        c.request("SERVER_RESPONSE", req, 0x05, b"")
        self.assertEqual(got, [("err", 0x05)])
        rid = c.request("SERVER_RESPONSE", req, 0, b"")
        self.assertEqual(c.reply(rid, "SERVER_RESPONSE")[0], P.ST_NOT_FOUND)

    def test_unanswered_times_out(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        got = []
        br.server_read(sid, hm, SENSOR, 0, 0, got.append, lambda e: got.append(("err", e)))
        be.timers[-1][1]()
        self.assertEqual(got, [("err", 0x0e)])

    def test_writes(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        got = []
        br.server_write(sid, hctl, SENSOR, b"\x01", 0, True, 0, lambda v: got.append("ok"), got.append)
        conn, req, handle, offset, need, value = c.events("EV_SERVER_WRITE")[0]
        self.assertEqual((handle, need, value), (hctl, True, b"\x01"))
        c.request("SERVER_RESPONSE", req, 0, b"")
        self.assertEqual(got, ["ok"])
        # write command: BlueZ answered at once, Android told with no request id
        br.server_write(sid, hctl, SENSOR, b"\x02", 0, False, 0, lambda v: got.append("cmd"), got.append)
        self.assertEqual(got, ["ok", "cmd"])
        self.assertEqual(c.events("EV_SERVER_WRITE")[1][1], 0)

    def test_subscription_becomes_cccd_write(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        be.peers = [SENSOR]
        br.server_notify_state(sid, hm, True)
        conn, req, handle, offset, need, value = c.events("EV_SERVER_WRITE")[0]
        self.assertEqual((req, handle, need, value), (0, hc, False, b"\x01\x00"))
        br.server_notify_state(sid, hm, False)
        self.assertEqual(c.events("EV_SERVER_WRITE")[1][5], b"\x00\x00")
        # the central is a connection now
        self.assertEqual(c.events("EV_SERVER_CONN")[0][1], SENSOR)

    def test_notify(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        rid = c.request("SERVER_NOTIFY", 1, hm, False, b"\x00\x50")
        self.assertEqual(c.reply(rid, "SERVER_NOTIFY")[0], P.ST_OK)
        self.assertEqual(be.called("server_value"), [("server_value", sid, hm, b"\x00\x50", False)])
        rid = c.request("SERVER_NOTIFY", 1, 999, False, b"")
        self.assertEqual(c.reply(rid, "SERVER_NOTIFY")[0], P.ST_NOT_FOUND)

    def test_central_disconnects(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        br.server_read(sid, hm, SENSOR, 0, 0, lambda v: None, lambda e: None)
        br.device_changed(SENSOR, {"Connected": True})
        br.device_changed(SENSOR, {"Connected": False})
        self.assertEqual(c.events("EV_SERVER_CONN")[-1][1:], [SENSOR, False])

    def test_devices_that_never_touch_androids_attributes_stay_unseen(self):
        be, store, br, c, sid, handles = self.added()
        br.device_changed(AIRPODS, {"Connected": False})
        br.device_changed(AIRPODS, {"Connected": True})
        self.assertEqual(c.events("EV_SERVER_CONN"), [])

    def test_freeze_pauses_and_thaw_serves_again(self):
        be, store, br, c, sid, (hs, hm, hc, hctl, hd) = self.added()
        got = []
        br.server_read(sid, hm, SENSOR, 0, 0, got.append, lambda e: got.append(("err", e)))
        br.set_frozen(True)
        self.assertEqual(be.called("unserve"), [("unserve", sid)])
        self.assertEqual(got, [("err", 0x0e)])                 # the open request ends
        br.server_read(sid, hm, SENSOR, 0, 0, got.append, lambda e: got.append(("err2", e)))
        self.assertEqual(got[-1], ("err2", 0x0e))
        br.set_frozen(False)
        self.assertEqual(len(be.called("serve")), 2)

    def test_client_gone_or_disabled(self):
        be, store, br, c, sid, handles = self.added()
        c.request("ENABLE", False)
        self.assertEqual(be.called("unserve"), [("unserve", sid)])
        self.assertEqual(br.services, {})

    def test_remove_and_ownership(self):
        be, store, br, c, sid, handles = self.added()
        other = FakeClient(br)
        other.request("HELLO", P.VERSION, "x")
        other.request("ENABLE", True)
        self.assertEqual(other.reply(other.request("SERVER_REMOVE", sid), "SERVER_REMOVE")[0], P.ST_NOT_FOUND)
        self.assertEqual(c.reply(c.request("SERVER_REMOVE", sid), "SERVER_REMOVE")[0], P.ST_OK)
        self.assertEqual(be.called("unserve"), [("unserve", sid)])


if __name__ == "__main__":
    unittest.main()
