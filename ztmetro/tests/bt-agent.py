"""ztos-bt-agent's decisions, with BlueZ and the shell played by this test. It
needs neither D-Bus nor a phone: `make check-bt-pair`."""
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "rootfs/overlay/usr/local/bin/ztos-bt-agent"
loader = importlib.machinery.SourceFileLoader("ztos_bt_agent", str(path))
spec = importlib.util.spec_from_loader("ztos_bt_agent", loader)
agent = importlib.util.module_from_spec(spec)
loader.exec_module(agent)

DEV = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"


class FakeIO:
    def __init__(self):
        self.lines = []
        self.shell_up = True
        self.on = True
        self.props = {"Alias": "Speaker", "Address": "AA:BB:CC:DD:EE:FF", "Paired": False, "Trusted": False}
        self.timers = {}
        self.next_timer = 0
        self.aborted = []
        self.android = None
        self.settings_pairing = None   # the address `bluetoothctl pair` is running for
        self.clock = 1000.0

    def show(self, line):
        if self.shell_up:
            self.lines.append(line)
        return self.shell_up

    def device(self, path, cb):
        cb(self.props)

    def discoverable(self, path, cb):
        cb(self.on)

    def expected(self, address):
        return address == self.android

    def after(self, seconds, fn):
        self.next_timer += 1
        self.timers[self.next_timer] = (seconds, fn)
        return self.next_timer

    def stop(self, handle):
        del self.timers[handle]

    def abort(self, device):
        self.aborted.append(device)

    def local_pair(self, address):
        return address == self.settings_pairing

    def now(self):
        return self.clock

    def fire(self):
        (handle, (_seconds, fn)), = self.timers.items()
        del self.timers[handle]
        fn()


class Reply:
    def __init__(self):
        self.oks = []
        self.errs = []

    def ok(self, value):
        self.oks.append(value)

    def err(self, which, why):
        self.errs.append(which)


class Base(unittest.TestCase):
    def setUp(self):
        self.io = FakeIO()
        self.broker = agent.Broker(self.io)
        self.reply = Reply()

    def ask(self, mode, value="", device=DEV):
        self.broker.ask(mode, device, value, self.reply.ok, self.reply.err)

    def answered(self, accepted, text=""):
        self.broker.answered(self.broker.cur.id, accepted, text)


class Entry(Base):
    def test_pin_reaches_bluez(self):
        self.ask("pin")
        self.assertEqual(self.io.lines, ["bt-pair 1|pin|55|AA:BB:CC:DD:EE:FF||Speaker\n"])
        self.assertEqual(self.reply.oks, [])
        self.answered(True, "0000")
        self.assertEqual(self.reply.oks, ["0000"])
        self.assertEqual(self.io.lines[-1], "bt-pair-close 1\n")
        self.assertIsNone(self.broker.cur)

    def test_passkey_is_a_number(self):
        self.ask("passkey")
        self.answered(True, "012345")
        self.assertEqual(self.reply.oks, [12345])

    def test_bad_answers_are_refused(self):
        for mode, text in (("pin", ""), ("pin", "x" * 17), ("pin", "12 34"), ("pin", "a|b"),
                           ("passkey", "12345"), ("passkey", "1234567"), ("passkey", "12345a")):
            with self.subTest(mode=mode, text=text):
                self.setUp()
                self.ask(mode)
                self.answered(True, text)
                self.assertEqual(self.reply.oks, [])
                self.assertEqual(self.reply.errs, ["Rejected"])


class Decisions(Base):
    def test_no_question_answers_itself(self):
        for mode, value in (("confirm", "123456"), ("authorize", "")):
            with self.subTest(mode=mode):
                self.setUp()
                self.ask(mode, value)
                self.assertEqual(self.reply.oks + self.reply.errs, [])
                self.io.fire()
                self.assertEqual(self.reply.oks, [])
                self.assertEqual(self.reply.errs, ["Canceled"])
                self.assertTrue(self.io.lines[-1].startswith("bt-pair-close"))

    def test_yes_and_no(self):
        self.ask("confirm", "123456")
        self.answered(True)
        self.assertEqual(len(self.reply.oks), 1)
        self.ask("confirm", "123456")
        self.answered(False)
        self.assertEqual(self.reply.errs, ["Rejected"])

    def test_not_discoverable_asks_nothing(self):
        self.io.on = False
        for mode, value in (("pin", ""), ("passkey", ""), ("confirm", "123456"), ("authorize", ""),
                            ("show-pin", "123456"), ("show-passkey", "123456")):
            with self.subTest(mode=mode):
                self.reply = Reply()
                self.ask(mode, value)
                self.assertEqual(self.io.lines, [])
                self.assertEqual(self.reply.errs, ["Rejected"])
                self.assertEqual(self.reply.oks, [])
                self.assertIsNone(self.broker.cur)

    def test_no_shell_refuses_at_once(self):
        self.io.shell_up = False
        self.ask("pin")
        self.assertEqual(self.reply.errs, ["Rejected"])
        self.assertEqual(self.io.timers, {})

    def test_one_at_a_time(self):
        self.ask("pin")
        other = Reply()
        self.broker.ask("confirm", DEV, "123456", other.ok, other.err)
        self.assertEqual(other.errs, ["Rejected"])
        self.assertEqual(self.reply.errs, [])
        self.assertEqual(len(self.io.lines), 1)

    def test_a_stale_answer_does_nothing(self):
        self.ask("pin")
        first = self.broker.cur.id
        self.broker.cancel()
        self.reply = Reply()
        self.ask("pin")
        self.broker.answered(first, True, "1234")
        self.assertEqual(self.reply.oks, [])
        self.assertIsNotNone(self.broker.cur)


class EndOfRequest(Base):
    def test_cancel_closes_and_is_canceled(self):
        self.ask("pin")
        self.broker.cancel()
        self.assertEqual(self.reply.errs, ["Canceled"])
        self.assertEqual(self.io.lines[-1], "bt-pair-close 1\n")
        self.assertEqual(self.io.timers, {})

    def test_cancel_with_nothing_open(self):
        self.broker.cancel()
        self.assertEqual(self.io.lines, [])

    def test_timeout_is_before_bluez_gives_up(self):
        self.ask("pin")
        (seconds, _fn), = self.io.timers.values()
        self.assertLess(seconds, 60)

    def test_cancel_during_the_trust_wait(self):
        self.ask("service")
        self.broker.cancel()
        self.assertEqual(self.io.timers, {})
        self.assertEqual(self.reply.errs, ["Canceled"])
        self.assertEqual(self.io.lines, [])


class Showing(Base):
    def test_pin_replies_at_once_and_stays_up(self):
        self.ask("show-pin", "123456")
        self.assertEqual(len(self.reply.oks), 1)
        self.assertIsNotNone(self.broker.cur)
        self.assertIn("|show-pin|60|AA:BB:CC:DD:EE:FF|123456|Speaker", self.io.lines[0])

    def test_closes_when_paired(self):
        self.ask("show-pin", "123456")
        self.broker.device_changed(DEV, {"Paired": True})
        self.assertIsNone(self.broker.cur)
        self.assertTrue(self.io.lines[-1].startswith("bt-pair-close"))

    def test_closes_when_the_link_drops(self):
        self.ask("show-passkey", "000042")
        self.broker.device_changed(DEV, {"Connected": False})
        self.assertIsNone(self.broker.cur)

    def test_another_device_does_not_close_it(self):
        self.ask("show-pin", "123456")
        self.broker.device_changed("/org/bluez/hci0/dev_11_22_33_44_55_66", {"Paired": True})
        self.assertIsNotNone(self.broker.cur)

    def test_closes_on_cancel_and_timeout_without_a_second_reply(self):
        self.ask("show-pin", "123456")
        self.broker.cancel()
        self.assertIsNone(self.broker.cur)
        self.ask("show-pin", "123456")
        self.io.fire()
        self.assertIsNone(self.broker.cur)
        self.assertEqual(self.reply.errs, [])
        self.assertEqual(len(self.reply.oks), 2)

    def test_passkey_counts_digits(self):
        self.broker.display_passkey(DEV, 42, 0, self.reply.ok, self.reply.err)
        self.assertIn("|show-passkey|60|AA:BB:CC:DD:EE:FF|000042|Speaker", self.io.lines[0])
        self.broker.display_passkey(DEV, 42, 3, self.reply.ok, self.reply.err)
        self.assertEqual(self.io.lines[-1], "bt-pair-update 1|3\n")
        self.assertEqual(len([x for x in self.io.lines if x.startswith("bt-pair ")]), 1)

    def test_cancel_button_ends_the_pairing(self):
        self.ask("show-pin", "123456")
        self.answered(False)
        self.assertEqual(self.io.aborted, [DEV])
        self.assertIsNone(self.broker.cur)

    def test_cancel_button_leaves_a_bond_alone(self):
        # CancelPairing removes the bond of a device with no pairing in progress
        self.io.props["Paired"] = True
        self.ask("show-pin", "123456")
        self.answered(False)
        self.assertEqual(self.io.aborted, [])

    def test_a_new_request_replaces_the_number(self):
        self.ask("show-pin", "123456")
        other = Reply()
        self.broker.ask("confirm", DEV, "654321", other.ok, other.err)
        self.assertEqual(other.errs, [])
        self.assertEqual(self.io.lines[-2], "bt-pair-close 1\n")
        self.assertTrue(self.io.lines[-1].startswith("bt-pair 2|confirm|"))


class Service(Base):
    UUID = "0000110d-0000-1000-8000-00805f9b34fb"

    def test_asks_after_the_trust_wait(self):
        self.ask("service", agent.service_label(self.UUID))
        self.assertEqual(self.io.lines, [])
        (seconds, _fn), = self.io.timers.values()
        self.assertLess(seconds, 5)
        self.io.fire()
        self.assertIn("|service|55|AA:BB:CC:DD:EE:FF|audio|Speaker", self.io.lines[0])
        self.assertEqual(self.reply.oks, [])
        self.answered(True)
        self.assertEqual(len(self.reply.oks), 1)

    def test_just_trusted_needs_no_question(self):
        self.ask("service", "audio")
        self.io.props["Trusted"] = True
        self.io.fire()
        self.assertEqual(self.io.lines, [])
        self.assertEqual(len(self.reply.oks), 1)

    def test_paired_is_not_enough(self):
        self.io.props["Paired"] = True
        self.ask("service", "audio")
        self.io.fire()
        self.assertEqual(self.reply.oks, [])
        self.assertEqual(len(self.io.lines), 1)

    def test_refused_when_not_discoverable(self):
        self.io.on = False
        self.ask("service", "audio")
        self.io.fire()
        self.assertEqual(self.reply.errs, ["Rejected"])
        self.assertEqual(self.io.lines, [])

    def test_labels(self):
        self.assertEqual(agent.service_label(self.UUID), "audio")
        self.assertEqual(agent.service_label("0000112f-0000-1000-8000-00805f9b34fb"), "your contacts")
        self.assertEqual(agent.service_label("not a uuid|x"), "")
        self.assertEqual(agent.service_label("00001234-0000-1000-8000-00805f9b34fb"), "")


class Untrusted(Base):
    def test_a_hostile_name_stays_one_field(self):
        for name in ("a|b\ncd", "a\r\nbt-pair 9|confirm|5||123456|x", "‮evil​", "x" * 300, "\x00\x7f|"):
            with self.subTest(name=name):
                self.setUp()
                self.io.props["Alias"] = name
                self.ask("confirm", "123456")
                line = self.io.lines[0]
                self.assertEqual(line.count("\n"), 1)
                self.assertTrue(line.endswith("\n"))
                self.assertEqual(line.count("|"), 5)
                self.assertLessEqual(len(line.encode()), 255)
                self.assertNotIn("‮", line)

    def test_cut_at_a_whole_character(self):
        cleaned = agent.clean_name("é" * 100)
        self.assertLessEqual(len(cleaned.encode()), agent.NAME_BYTES)
        self.assertEqual(set(cleaned), {"é"})

    def test_no_name_falls_back_to_the_address(self):
        self.io.props = {}
        self.ask("pin")
        self.assertIn("|AA:BB:CC:DD:EE:FF||AA:BB:CC:DD:EE:FF", self.io.lines[0])

    def test_addresses(self):
        self.assertEqual(agent.address_of(DEV), "AA:BB:CC:DD:EE:FF")
        self.assertEqual(agent.address_of("/org/bluez/hci0/dev_nonsense"), "")
        self.assertEqual(agent.adapter_of(DEV), "/org/bluez/hci0")

    def test_replies(self):
        self.assertEqual(agent.parse_reply("3\tOK\t1234\n"), ("3", True, "1234", False))
        self.assertEqual(agent.parse_reply("3\tOK\t\n"), ("3", True, "", False))
        self.assertEqual(agent.parse_reply("3\tCANCEL\n"), ("3", False, "", False))
        self.assertEqual(agent.parse_reply("3\tOK\t1234\tremember\n"), ("3", True, "1234", True))
        for bad in ("", "3", "3\tYES\t1", "3\tOK", "3\tCANCEL\tx", "3\tOK\t1\tx", "3\tCANCEL\tremember"):
            self.assertIsNone(agent.parse_reply(bad))


class Logs(Base):
    def test_no_secret_is_logged(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stderr(out):
            self.ask("pin")
            self.answered(True, "SECRET9")
            self.ask("show-pin", "778899")
            self.broker.cancel()
            self.broker.display_passkey(DEV, 445566, 0, self.reply.ok, self.reply.err)
            self.broker.cancel()
            self.ask("confirm", "112233")
            self.answered(False)
        text = out.getvalue()
        self.assertIn("AA:BB:CC:DD:EE:FF", text)
        for secret in ("SECRET9", "778899", "445566", "112233"):
            self.assertNotIn(secret, text)


class AndroidPairing(Base):
    """A device ztos-btbridge deals with for an Android app: the agent asks,
    even though the phone isn't discoverable; for that device only."""

    def test_asks_for_the_device_the_bridge_expects(self):
        self.io.on = False
        self.io.android = "AA:BB:CC:DD:EE:FF"
        self.ask("confirm", "123456")
        self.assertEqual(len(self.io.lines), 1)
        self.assertEqual(self.reply.oks + self.reply.errs, [])   # still the person's answer
        self.answered(True)
        self.assertEqual(len(self.reply.oks), 1)

    def test_a_passkey_device_asking_by_itself(self):
        # RM_2685 (8 Nov): connected for an app, it asks for a passkey on its own
        self.io.on = False
        self.io.android = "AA:BB:CC:DD:EE:FF"
        self.ask("passkey")
        self.assertEqual(len(self.io.lines), 1)
        self.assertIn("|passkey|", self.io.lines[0])
        self.assertEqual(self.reply.oks + self.reply.errs, [])

    def test_another_device_is_refused(self):
        self.io.on = False
        self.io.android = "11:22:33:44:55:66"
        self.ask("confirm", "123456")
        self.assertEqual(self.reply.errs, ["Rejected"])
        self.assertEqual(self.io.lines, [])

    def test_the_list(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "expected")
            A, B = "AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66"
            self.assertFalse(agent.expected(path, A, now=1000))
            with open(path, "w") as f:
                f.write("%s 1060\n%s 1030\nnonsense\nCC:CC:CC:CC:CC:CC x\n" % (A, B))
            os.chmod(path, 0o600)
            self.assertTrue(agent.expected(path, A.lower(), now=1000))
            self.assertTrue(agent.expected(path, B, now=1000))
            self.assertFalse(agent.expected(path, B, now=1031))          # expired
            self.assertTrue(agent.expected(path, A, now=1060))
            self.assertFalse(agent.expected(path, A, now=1061))
            self.assertFalse(agent.expected(path, "AA:BB:CC:DD:EE:00", now=1000))
            self.assertFalse(agent.expected(path, "CC:CC:CC:CC:CC:CC", now=1000))
            self.assertFalse(agent.expected(path, A, now=1060 - agent.EXPECT_MAX_SECONDS - 1))  # too far ahead
            os.chmod(path, 0o620)                                         # someone else could write it
            self.assertFalse(agent.expected(path, A, now=1000))
            os.chmod(path, 0o600)
            link = os.path.join(d, "link")
            os.symlink(path, link)
            self.assertFalse(agent.expected(link, A, now=1000))


PRINTER_A = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
PRINTER_B = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_00"
ADV = ["49646561-7346-6c6f-6174-40882af10000", "0000180a-0000-1000-8000-00805f9b34fb"]


class RememberedCodes(Base):
    """A code ticked on the sheet answers the next similar device by itself,
    for 3 hours, only when the phone started the pairing (9 Nov)."""

    def setUp(self):
        super().setUp()
        self.io.on = False
        self.io.android = "AA:BB:CC:DD:EE:FF"
        # the app scanned: BlueZ showed what each printer advertises
        for path in (PRINTER_A, PRINTER_B):
            self.broker.device_changed(path, {"UUIDs": ADV, "ServicesResolved": False})
        # then they connected and resolved: UUIDs is the GATT database now
        self.io.props = {"Alias": "RM_2685", "Address": "AA:BB:CC:DD:EE:FF", "Paired": False,
                         "ServicesResolved": True, "UUIDs": ADV + ["00001800-0000-1000-8000-00805f9b34fb"]}

    def teach(self, mode="passkey", text="123456"):
        self.ask(mode, device=PRINTER_A)
        self.broker.answered(self.broker.cur.id, True, text, True)
        self.broker.device_changed(PRINTER_A, {"Paired": True})

    def second(self, mode="passkey"):
        self.io.props = dict(self.io.props, Alias="RM_2686", Address="AA:BB:CC:DD:EE:00")
        self.io.android = "AA:BB:CC:DD:EE:00"
        self.io.lines = []
        self.reply = Reply()
        self.ask(mode, device=PRINTER_B)

    def test_used_for_a_similar_device(self):
        self.teach()
        self.second()
        self.assertEqual(self.reply.oks, [123456])            # no sheet
        self.assertFalse(any(x.startswith("bt-pair ") for x in self.io.lines))
        self.assertIsNone(self.broker.cur)
        self.broker.device_changed(PRINTER_B, {"Paired": True})
        self.assertEqual(self.io.lines, ["notify Bluetooth|Paired|Paired RM_2686 with the remembered passkey\n"])
        self.assertEqual(self.io.timers, {})

    def test_pin_too_and_kept_apart_from_passkeys(self):
        self.teach("pin", "0000")
        self.second("passkey")                                  # a passkey request: not the PIN
        self.assertEqual(self.reply.oks, [])
        self.assertTrue(self.io.lines[0].startswith("bt-pair "))

    def test_not_ticked_not_remembered(self):
        self.ask("passkey", device=PRINTER_A)
        self.answered(True, "123456")
        self.second()
        self.assertEqual(self.reply.oks, [])
        self.assertTrue(self.io.lines[0].startswith("bt-pair "))

    def test_expires_after_3_hours(self):
        self.teach()
        self.io.clock += agent.REMEMBER_SECONDS + 1
        self.second()
        self.assertEqual(self.reply.oks, [])
        self.assertTrue(self.io.lines[0].startswith("bt-pair "))

    def test_only_when_the_phone_started_it(self):
        self.teach()
        self.io.on = True                                       # discoverable: the sheet may show
        self.io.props = dict(self.io.props, Alias="RM_2686", Address="AA:BB:CC:DD:EE:00")
        self.io.android = None                                  # nothing of ours: unsolicited
        self.io.lines = []
        self.reply = Reply()
        self.ask("passkey", device=PRINTER_B)
        self.assertEqual(self.reply.oks, [])
        self.assertTrue(self.io.lines[0].startswith("bt-pair "))
        self.answered(False)
        self.io.settings_pairing = "AA:BB:CC:DD:EE:00"          # Settings' bluetoothctl pair
        self.reply = Reply()
        self.ask("passkey", device=PRINTER_B)
        self.assertEqual(self.reply.oks, [123456])

    def test_never_unsolicited_even_when_not_discoverable(self):
        self.teach()
        self.io.props = dict(self.io.props, Alias="RM_2686", Address="AA:BB:CC:DD:EE:00")
        self.io.android = None
        self.reply = Reply()
        self.ask("passkey", device=PRINTER_B)
        self.assertEqual(self.reply.oks, [])
        self.assertEqual(self.reply.errs, ["Rejected"])

    def test_the_uuids_must_match_exactly(self):
        self.teach()
        for uuids in (ADV[:1], ADV + ["0000180f-0000-1000-8000-00805f9b34fb"]):
            self.broker.device_changed(PRINTER_B, {"UUIDs": uuids})
            self.second()
            self.assertEqual(self.reply.oks, [], uuids)
            self.answered(False)
        self.broker.device_changed(PRINTER_B, {"UUIDs": [u.upper() for u in reversed(ADV)]})
        self.second()
        self.assertEqual(self.reply.oks, [123456])              # same set, any order or case

    def test_advertised_not_the_resolved_list(self):
        self.teach()
        self.broker.device_changed(PRINTER_B, {"ServicesResolved": True})
        self.broker.device_changed(PRINTER_B, {"UUIDs": ADV + ["00001801-0000-1000-8000-00805f9b34fb"]})
        self.second()
        self.assertEqual(self.reply.oks, [123456])              # resolved services don't count

    def test_failure_falls_back_to_the_sheet(self):
        self.teach()
        self.second()
        self.assertEqual(self.reply.oks, [123456])
        self.broker.device_changed(PRINTER_B, {"Connected": False})   # AuthenticationFailed: dropped
        self.assertFalse(any(x.startswith("notify") for x in self.io.lines))
        self.second()
        self.assertEqual(self.reply.oks, [])
        self.assertTrue(self.io.lines[0].startswith("bt-pair "))

    def test_asked_again_or_no_pairing_counts_as_failed(self):
        self.teach()
        self.second()
        self.second()                                           # asked again at once: it didn't work
        self.assertEqual(self.reply.oks, [])
        self.answered(False)
        self.broker.remembered[0].failed.clear()
        self.second()
        self.assertEqual(self.reply.oks, [123456])
        (_s, fn), = [v for v in self.io.timers.values() if v[0] == agent.REMEMBER_WAIT_SECONDS]
        fn()                                                    # never Paired
        self.second()
        self.assertEqual(self.reply.oks, [])

    def test_a_code_that_failed_where_typed_is_forgotten(self):
        self.ask("passkey", device=PRINTER_A)
        self.broker.answered(self.broker.cur.id, True, "654321", True)
        self.broker.device_changed(PRINTER_A, {"Connected": False})   # mistyped
        self.assertEqual(self.broker.remembered, [])

    def test_settings_pairing_is_seen_in_proc(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            for pid, argv in (("10", [b"timeout", b"120", b"bluetoothctl", b"pair", b"AA:BB:CC:DD:EE:00"]),
                              ("11", [b"bluetoothctl", b"info", b"AA:BB:CC:DD:EE:01"]),
                              ("12", [b"/usr/bin/bluetoothctl", b"pair", b"aa:bb:cc:dd:ee:02"])):
                os.mkdir(os.path.join(d, pid))
                with open(os.path.join(d, pid, "cmdline"), "wb") as f:
                    f.write(b"\0".join(argv) + b"\0")
            os.mkdir(os.path.join(d, "self"))
            self.assertFalse(agent.local_pair("AA:BB:CC:DD:EE:00", d))   # timeout's, not bluetoothctl's
            self.assertFalse(agent.local_pair("AA:BB:CC:DD:EE:01", d))   # not pairing
            self.assertTrue(agent.local_pair("AA:BB:CC:DD:EE:02", d))
            self.assertFalse(agent.local_pair("AA:BB:CC:DD:EE:03", d))

    def test_never_logged(self):
        import contextlib
        import io as sio
        err = sio.StringIO()
        with contextlib.redirect_stderr(err):
            self.teach("pin", "8642")
            self.second("pin")
            self.broker.device_changed(PRINTER_B, {"Paired": True})
        self.assertNotIn("8642", err.getvalue())
        self.assertEqual(self.reply.oks, ["8642"])


if __name__ == "__main__":
    unittest.main()
