#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What scripts/collect-logs.py keeps out, and what it must leave in. The numbers
below are made up; no phone is needed.

    python3 scripts/tests/test_collect_logs.py
"""
import importlib.util
import unittest
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "collect-logs.py"
spec = importlib.util.spec_from_file_location("collect_logs", path)
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)

IDENTITY = {
    "imei line": "ztoscorenetworkingd: IMEI 490154203237518",
    "imei key": "modem: imei=490154203237518 ok",
    "imeisv": "ril: imeisv: 4901542032375101",
    "imsi": "modem: IMSI read 310150123456789",
    "iccid": "sim: iccid 8901260123456789012",
    "at command": "AT+CGSN -> 490154203237518",
    "bare imei": "cbd: reply 490154203237518 done",
    "bare, joined to text": "rx=490154203237518;",
    "spaced imei": "line 49 015420 323751 8 end",
    "efs path": "cbd: opened /efs/imei/mps_code.dat",
    "efs mount": "mount /dev/sda3 on /run/android/mnt/vendor/efs type ext4",
    "cpefs": "partition CPEFS is sda4",
    "by-name": "link /dev/block/platform/11120000.ufs/by-name/EFS",
    "nv data": "rild: loaded nv_data.bin hash 1234",
    "serial": "kernel: serial=R38K1234567",
    "serialno": "ro.serialno R38K1234567",
    "phone": "ztmessages: from +14155552671 hello",
    "phone number key": "phone_number 4155552671 saved",
}
SECRETS = {
    "androidboot": ("Kernel command line: androidboot.em.did=3a4b5c6d root=/dev/x", "3a4b5c6d"),
    "wifi mac": ("wlan0: associated with 3c:37:86:aa:bb:cc", "3c:37:86:aa:bb:cc"),
    "bluez path": ("bluetoothd: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF connected", "AA_BB_CC_DD_EE_FF"),
    "ssid quoted": ("wpa_supplicant: Trying SSID='My Home Wifi' freq=5280", "My Home Wifi"),
    "ssid plain": ("nm: ssid=MyHomeWifi", "MyHomeWifi"),
    "nm connection": ("NetworkManager: Activation: connection 'My Home Wifi' started", "My Home Wifi"),
    "psk": ("psk=hunter2hunter2", "hunter2hunter2"),
    "password": ("config: password: hunter2", "hunter2"),
    "token": ("push: token abcdef123456", "abcdef123456"),
    "email": ("sync: signed in as someone@example.org", "someone@example.org"),
    # as NetworkManager really prints them
    "nm config": ("<info>  [1794061817.4301] Config: added 'ssid' value 'My Home Wifi'", "My Home Wifi"),
    "nm access point": ("Activation: (wifi) access point 'My Home Wifi' has security, but secrets", "My Home Wifi"),
    "nm stage 2": ("Stage 2 of 5 (Device Configure) successful. Connected to wireless network \"My Home Wifi\"", "My Home Wifi"),
    "nm policy": ("policy: set 'My Home Wifi' (wlp1s0) as default for IPv4 routing and DNS", "My Home Wifi"),
}
KEEP = [
    "Nov 07 10:00:01 ztos kernel: Booting Linux on physical CPU 0x0000000000",
    "[   12.345678] zrn-flash: LDO9 control 00 -> 40",
    "systemd[1]: Started ztoscsf-sensorhub.service",
    "2026-11-07T10:00:01-0500 ztos kernel: memory: 3752304K/3932160K available",
    "kernel: serial8250: ttyS0 at MMIO 0x10000000",
    "ttyGS0 is the USB serial console",
    "inet 172.16.42.1/24 scope global usb0",
    "sudo[8731]:   samuel : PWD=/home/samuel ; USER=root ; COMMAND=/usr/bin/true",
    "brcmfmac: brcmf_cfg80211_scan: error (-52)",
    "kernel: OF: reserved mem: 0x0000000080000000..0x0000000080001fff (8 KiB) nomap sec-debug@80000000",
    "NetworkManager[367]: <info>  [1794061813.1478] hostname: using hostnamed",
    "NetworkManager[367]: Config: added 'scan_ssid' value '1'",
]


class Redaction(unittest.TestCase):
    def test_identity_lines_leave_no_digits_behind(self):
        for name, line in IDENTITY.items():
            out = c.redact_line(line)
            for secret in ("490154203237518", "310150123456789", "8901260123456789012", "4901542032375101",
                           "R38K1234567", "+14155552671", "4155552671", "mps_code", "49 015420 323751 8"):
                self.assertNotIn(secret, out, name)

    def test_lines_about_identity_or_efs_go_whole(self):
        for name in ("imei line", "imsi", "iccid", "efs path", "efs mount", "cpefs", "by-name", "nv data", "serial"):
            self.assertEqual(c.redact_line(IDENTITY[name]), c.DROPPED, name)

    def test_secrets_are_replaced_and_the_rest_of_the_line_stays(self):
        for name, (line, secret) in SECRETS.items():
            out = c.redact_line(line)
            self.assertNotIn(secret, out, name)
            self.assertNotEqual(out, c.DROPPED, name)

    def test_ordinary_diagnostics_are_left_alone(self):
        for line in KEEP:
            self.assertEqual(c.redact_line(line), line)

    def test_no_long_number_survives_anywhere(self):
        for line in ("a490154203237518b", "x=490154203237518.5", "id-490154203237518"):
            self.assertNotRegex(c.redact_line(line), r"\d{14}")


    def test_a_saved_network_name_goes_however_it_is_worded(self):
        self.assertEqual(c.redact_line("joined CoffeeShop today", ("CoffeeShop",)), "joined [wifi name] today")


class Commands(unittest.TestCase):
    def test_no_command_reaches_for_identity_data(self):
        for _, command, _ in c.COMMANDS + [c.PREVIOUS_BOOT]:
            self.assertIsNone(c.FORBIDDEN_COMMAND.search(command), command)

    def test_the_guard_refuses(self):
        for command in ("cat /efs/imei/mps_code.dat", "dd if=/dev/sda3", "cat /run/android/mnt/vendor/efs/x", "getprop"):
            with self.assertRaises(SystemExit):
                c.run(command, False)

    def test_commands_only_read(self):
        verbs = re.compile(r"(^|[;|&]\s*)(rm|mv|cp|tee|install|mount|umount|mkfs|sed -i|touch|chmod|chown|reboot)\b")
        for _, command, _ in c.COMMANDS + [c.PREVIOUS_BOOT]:
            self.assertIsNone(verbs.search(command), command)
            self.assertNotRegex(command, r"[^2]>\s*/")


class Prompts(unittest.TestCase):
    def test_password_prompts_are_cleaned(self):
        self.assertEqual(c.clean_prompts("Password: hello\n"), "hello\n")


import re  # noqa: E402

if __name__ == "__main__":
    unittest.main()
