#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The doctor's checks, each made to fail. No system setting is touched: the
binfmt entry, the ID files and the OS name are all stand-ins.

    python3 installer/tests/test_host.py
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ztos_installer import host  # noqa: E402

GOOD_ENTRY = ("enabled\ninterpreter /usr/bin/qemu-aarch64-static\nflags: F\noffset 0\n"
              "magic 7f454c46\nmask ffffffff\n")


class Hints(unittest.TestCase):
    def hint(self, fam, *needs):
        with mock.patch.object(host, "family", return_value=fam):
            return host.install_hint(*needs)

    def test_each_family_gets_its_own_words(self):
        self.assertEqual(self.hint("arch", "bsdtar"), "sudo pacman -S --needed libarchive")
        self.assertIn("sudo dnf install bsdtar", self.hint("redhat", "bsdtar"))
        self.assertEqual(self.hint("debian", "bsdtar"), "sudo apt-get install libarchive-tools")

    def test_unknown_family_names_no_package_manager(self):
        self.assertEqual(self.hint(None, "bsdtar"), "install: bsdtar")

    def test_every_build_tool_has_packages(self):
        for tool in host.BUILD_TOOLS + host.PHONE_TOOLS:
            self.assertIn(host.PROVIDED_BY.get(tool, tool), host.PACKAGES)

    def test_family_is_read_from_os_release(self):
        cases = {'ID="rocky"\nID_LIKE="rhel centos fedora"\n': "redhat", "ID=fedora\n": "redhat",
                 "ID=arch\n": "arch", "ID=archarm\n": "arch", "ID=ubuntu\nID_LIKE=debian\n": "debian",
                 "ID=plan9\n": None}
        for text, want in cases.items():
            with mock.patch.object(Path, "read_text", return_value=text):
                self.assertEqual(host.family(), want, text)


class IdRanges(unittest.TestCase):
    def test_a_missing_subgid_range_is_seen(self):
        with tempfile.TemporaryDirectory() as tmp:
            sub = Path(tmp) / "subuid"
            sub.write_text("someone:100000:65536\n")
            self.assertTrue(host.has_range(sub, "someone"))
            self.assertFalse(host.has_range(sub, "other"))
            sub.write_text("other:100000:0\n")      # an empty range is no range
            self.assertFalse(host.has_range(sub, "other"))
            self.assertFalse(host.has_range(Path(tmp) / "missing", "other"))


class Qemu(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.entry = Path(self.tmp.name) / "qemu-aarch64"
        for patch in (mock.patch.object(host, "BINFMT", self.entry),
                      mock.patch.object(host.platform, "machine", return_value="x86_64")):
            patch.start()
            self.addCleanup(patch.stop)

    def checks(self):
        return host.check_qemu()

    def test_no_entry(self):
        self.assertIn("no qemu-aarch64 entry", host.problems(self.checks())[0].detail)

    def test_disabled_entry(self):
        self.entry.write_text(GOOD_ENTRY.replace("enabled", "disabled"))
        self.assertIn("disabled", host.problems(self.checks())[0].detail)

    def test_no_f_flag(self):
        self.entry.write_text(GOOD_ENTRY.replace("flags: F", "flags: "))
        self.assertIn("no F flag", host.problems(self.checks())[0].detail)

    def test_an_interpreter_that_does_not_run_fails_though_the_entry_looks_right(self):
        self.entry.write_text(GOOD_ENTRY)
        with mock.patch.object(host, "run_aarch64", return_value="Exec format error"):
            self.assertIn("Exec format error", host.problems(self.checks())[0].detail)

    def test_a_removed_interpreter_only_warns(self):
        self.entry.write_text(GOOD_ENTRY.replace("/usr/bin/qemu-aarch64-static", "/nonexistent/qemu"))
        with mock.patch.object(host, "run_aarch64", return_value=None):
            checks = self.checks()
        self.assertEqual(host.problems(checks), [])
        self.assertTrue(any(c.warning and "next reboot" in c.detail for c in checks))

    def test_a_dynamic_interpreter_fails(self):
        self.entry.write_text(GOOD_ENTRY.replace("/usr/bin/qemu-aarch64-static", sys.executable))
        with mock.patch.object(host, "run_aarch64", return_value=None):
            if host.elf_has_interpreter(sys.executable):
                self.assertIn("dynamically linked", host.problems(self.checks())[0].detail)


class Program(unittest.TestCase):
    def test_the_test_program_is_a_static_aarch64_elf(self):
        data = host.aarch64_program()
        self.assertEqual(data[:4], b"\x7fELF")
        self.assertEqual(int.from_bytes(data[18:20], "little"), 183)    # EM_AARCH64
        with tempfile.NamedTemporaryFile() as f:
            f.write(data)
            f.flush()
            self.assertFalse(host.elf_has_interpreter(f.name))


class Tools(unittest.TestCase):
    def test_a_missing_tool_says_how_to_get_it(self):
        with mock.patch.dict(os.environ, {"PATH": "/nonexistent"}), \
                mock.patch.object(host, "sbin_which", return_value=None), \
                mock.patch.object(host, "family", return_value="debian"):
            check = host.check_tool("socat")
        self.assertFalse(check.ok)
        self.assertEqual(check.detail, "sudo apt-get install socat")


if __name__ == "__main__":
    unittest.main()
