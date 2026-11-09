"""ztos-btbridge's wire format."""
import os
import re
import unittest

from fakes import P

HERE = os.path.dirname(os.path.abspath(__file__))


class ProtoTests(unittest.TestCase):
    def test_round_trip_every_type(self):
        sig = "yquiBaUsbLUL(yq)T(as)"
        values = [7, 0x1234, 0xdeadbeef, -40, True, "AA:BB:CC:DD:EE:0F", "180d", "héllo", b"\x00\xff",
                  ["2a37", "6e400001-b5a3-f393-e0a9-e50e24dcca9e"], [(1, 2), (3, 4)], ("00:11:22:33:44:55", "x")]
        packet = P.encode(0x0002, 9, P.typed(sig, values))
        _, msg_id, out = P.decode_with(sig, packet)
        self.assertEqual(msg_id, 9)
        self.assertEqual(out[6], "0000180d-0000-1000-8000-00805f9b34fb")
        self.assertEqual(out[9][0], "00002a37-0000-1000-8000-00805f9b34fb")
        self.assertEqual(out[:6], values[:6])
        self.assertEqual(out[7:9], values[7:9])
        self.assertEqual(out[10:], values[10:])

    def test_address_order(self):
        packet = P.encode(0x0011, 1, [P.A("01:02:03:04:05:06")])
        self.assertEqual(packet[P.HEADER.size:], b"a\x01\x02\x03\x04\x05\x06")

    def test_request_checked_against_signature(self):
        good = P.message("GATT_READ", 3, [1, 2, 0])
        self.assertEqual(P.decode_request(good), ("GATT_READ", 3, [1, 2, 0]))
        bad = P.encode("GATT_READ", 3, [1, P.Q(2)])               # one value short
        self.assertRaises(P.ProtoError, P.decode_request, bad)
        wrong = P.encode("GATT_READ", 3, [1, 2, P.Q(0)])          # u where q is due
        self.assertRaises(P.ProtoError, P.decode_request, wrong)

    def test_refuses_events_replies_unknown_and_id_zero(self):
        for packet in (P.message("EV_ADAPTER", 0, [True, "x"]), P.encode("REPLY", 1, [0]),
                       P.encode(0x7777, 1, []), P.message("SCAN_STOP", 0, [2])):
            self.assertRaises(P.ProtoError, P.decode_request, packet)

    def test_truncated_and_garbage(self):
        packet = P.message("HELLO", 1, [1, "shim"])
        for cut in range(len(packet) - 1):
            try:
                P.decode_request(packet[:cut])
            except P.ProtoError:
                continue
            self.fail("decoded a truncated packet at %d" % cut)
        self.assertRaises(P.ProtoError, P.decode_request, P.HEADER.pack(2, 0, 1) + b"Z")
        self.assertRaises(P.ProtoError, P.decode_request, P.HEADER.pack(2, 0, 1) + b"B\x02")
        self.assertRaises(P.ProtoError, P.decode_request, P.HEADER.pack(2, 0, 1) + b"s\x02\x00\xff\xfe")

    def test_reply_shapes(self):
        packet = P.reply("GATT_READ", 4, P.ST_OK, [b"\x01"])
        self.assertEqual(P.decode_with("ub", packet)[2], [0, b"\x01"])
        packet = P.reply("GATT_READ", 4, P.ST_ATT, [5])
        self.assertEqual(P.decode_with("uy", packet)[2], [P.ST_ATT, 5])
        packet = P.reply("GATT_READ", 4, P.ST_BUSY)
        self.assertEqual(P.decode_with("u", packet)[2], [P.ST_BUSY])

    def test_numbers_unique(self):
        nums = [v[0] for v in P.OPS.values()]
        self.assertEqual(len(nums), len(set(nums)))

    def test_shim_header_agrees(self):
        header = os.path.join(HERE, "..", "shim", "ztbt_proto.h")
        if not os.path.exists(header):
            self.skipTest("no shim header yet")
        text = open(header).read()
        found = dict((m.group(1), int(m.group(2), 16))
                     for m in re.finditer(r"ZTBT_OP_([A-Z0-9_]+)\s*=\s*0x([0-9a-fA-F]+)", text))
        self.assertEqual(found, {k: v[0] for k, v in P.OPS.items()})
        sigs = dict(re.findall(r'ZTBT_SIG_([A-Z0-9_]+)\s+"([^"]*)"', text))
        for name, (_, req, rep) in P.OPS.items():
            if req is not None:
                self.assertEqual(sigs.get(name), req, name)
            if rep is not None:
                self.assertEqual(sigs.get(name + "_REPLY"), rep, name)
        for name, value in re.findall(r"ZTBT_(ST_[A-Z_]+)\s*=\s*(\d+)", text):
            self.assertEqual(getattr(P, name), int(value), name)


if __name__ == "__main__":
    unittest.main()
