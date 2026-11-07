#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""fetch_tarball and its signature check, with no network: a throwaway key in a
scratch directory signs small files, and "curl" is a function here.

    python3 installer/tests/test_tarball.py [real-tarball real-signature]

With the two paths it also checks a real tarball against the key in the repository.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ztos_installer import build as b  # noqa: E402

REAL = sys.argv[1:3]
del sys.argv[1:3]


@unittest.skipIf(shutil.which("gpg") is None, "no gpg")
class Signature(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="ztos-gpg-test-")
        cls.home = cls.tmp.name
        cls.gpg = ["gpg", "--homedir", cls.home, "--batch", "--quiet", "--pinentry-mode", "loopback", "--passphrase", ""]
        subprocess.run([*cls.gpg, "--quick-gen-key", "test <test@example.org>", "rsa2048", "sign", "never"],
                       check=True, capture_output=True)
        out = subprocess.run([*cls.gpg, "--with-colons", "--list-keys"], check=True, capture_output=True, text=True).stdout
        cls.fingerprint = next(line.split(":")[9] for line in out.splitlines() if line.startswith("fpr"))
        cls.key = Path(cls.home) / "test.asc"
        cls.key.write_text(subprocess.run([*cls.gpg, "--armor", "--export"], check=True, capture_output=True,
                                          text=True).stdout)

    @classmethod
    def tearDownClass(cls):
        subprocess.run(["gpgconf", "--homedir", cls.home, "--kill", "all"], capture_output=True)
        cls.tmp.cleanup()

    def signed(self, text="the tarball"):
        d = Path(tempfile.mkdtemp(dir=self.home))
        path, sig = d / "file", d / "file.sig"
        path.write_text(text)
        subprocess.run([*self.gpg, "--detach-sign", "-o", str(sig), str(path)], check=True, capture_output=True)
        return path, sig

    def test_a_good_signature_passes(self):
        path, sig = self.signed()
        self.assertIsNone(b.signature_problem(path, sig, self.key, self.fingerprint))

    def test_a_changed_file_fails(self):
        path, sig = self.signed()
        path.write_text("the tarball, altered")
        self.assertIsNotNone(b.signature_problem(path, sig, self.key, self.fingerprint))

    def test_a_good_signature_from_another_key_fails(self):
        path, sig = self.signed()
        self.assertIsNotNone(b.signature_problem(path, sig, self.key, b.TARBALL_FINGERPRINT))

    def test_signed_by_someone_the_repository_key_does_not_know_fails(self):
        path, sig = self.signed()
        self.assertIsNotNone(b.signature_problem(path, sig))

    def test_garbage_for_a_signature_fails(self):
        path, sig = self.signed()
        sig.write_bytes(b"not a signature")
        self.assertIsNotNone(b.signature_problem(path, sig, self.key, self.fingerprint))

    def test_the_repository_key_has_the_pinned_fingerprint(self):
        out = subprocess.run(["gpg", "--homedir", self.home, "--batch", "--with-colons", "--show-keys",
                              str(b.TARBALL_KEY)], check=True, capture_output=True, text=True).stdout
        self.assertEqual(next(line.split(":")[9] for line in out.splitlines() if line.startswith("fpr")),
                         b.TARBALL_FINGERPRINT)

    @unittest.skipUnless(REAL, "no real tarball given")
    def test_a_real_tarball(self):
        self.assertIsNone(b.signature_problem(Path(REAL[0]), Path(REAL[1])))


class Fetch(unittest.TestCase):
    """The order of things in fetch_tarball; the signature check itself is above."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tarball = Path(self.tmp.name) / "alarm.tar.gz"
        self.sig = Path(self.tmp.name) / "alarm.tar.gz.sig"
        self.downloads = []
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(b, "TARBALL", self.tarball).start()
        mock.patch.object(b, "fetch", self.fake_fetch).start()
        mock.patch.object(b, "say").start()

    def fake_fetch(self, url, dest):
        self.downloads.append(url.rsplit(".", 1)[-1])
        dest.write_bytes(b"new")

    def problem_for(self, *bad):
        """A signature check that fails for the file whose content is in bad."""
        def check(path, sig):
            return "bad" if path.read_bytes() in bad else None
        mock.patch.object(b, "signature_problem", check).start()

    def saved(self, content=b"old"):
        self.tarball.write_bytes(content)
        self.sig.write_bytes(b"sig")
        mock.patch.object(b, "TARBALL_MIN_SIZE", 1).start()

    def test_a_good_saved_tarball_is_not_downloaded(self):
        self.saved()
        self.problem_for()
        b.fetch_tarball()
        self.assertEqual(self.downloads, [])
        self.assertEqual(self.tarball.read_bytes(), b"old")

    def test_a_saved_tarball_with_no_signature_gets_one(self):
        self.saved()
        self.sig.unlink()
        self.problem_for()
        b.fetch_tarball()
        self.assertEqual(self.downloads, ["sig"])

    def test_a_saved_tarball_that_fails_is_replaced(self):
        self.saved()
        self.problem_for(b"old")
        b.fetch_tarball()
        self.assertEqual(self.tarball.read_bytes(), b"new")

    def test_a_download_that_fails_is_not_kept(self):
        self.problem_for(b"new")
        with self.assertRaises(b.Fail):
            b.fetch_tarball()
        self.assertEqual(sorted(p.name for p in Path(self.tmp.name).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
