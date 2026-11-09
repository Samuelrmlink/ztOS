"""patch-bluetooth.py on a pretend image tree (no root needed)."""
import hashlib
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader("patch_bluetooth", str(HERE.parent / "patch-bluetooth.py"))
spec = importlib.util.spec_from_loader("patch_bluetooth", loader)
pb = importlib.util.module_from_spec(spec)
loader.exec_module(pb)
PREBUILTS = HERE.parents[2] / "prebuilts" / "waydroid-bt"


class PatchBluetooth(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lib = self.root / pb.APEX / "lib64"
        self.lib.mkdir(parents=True)
        self.stock = b"pretend stock library"
        (self.lib / "libbluetooth_jni.so").write_bytes(self.stock)
        self.old = pb.STOCK_JNI
        pb.STOCK_JNI = hashlib.sha256(self.stock).hexdigest()

    def tearDown(self):
        pb.STOCK_JNI = self.old
        self.tmp.cleanup()

    def test_system_twice(self):
        for _ in range(2):
            self.assertEqual(pb.system(str(self.root), str(PREBUILTS)), 0)
        self.assertEqual((self.lib / "libbluetooth_jni_stock.so").read_bytes(), self.stock)
        self.assertEqual(pb.sha(self.lib / "libbluetooth_jni.so"), pb.WRAPPER)
        apk = self.root / pb.APEX / "app" / "Bluetooth" / "Bluetooth.apk"
        self.assertEqual(pb.sha(apk), pb.APP)
        self.assertEqual(os.stat(apk).st_mode & 0o777, 0o644)

    def test_unknown_stock_refused(self):
        (self.lib / "libbluetooth_jni.so").write_bytes(b"another build")
        self.assertEqual(pb.system(str(self.root), str(PREBUILTS)), 3)
        self.assertFalse((self.lib / "libbluetooth_jni_stock.so").exists())

    def test_prebuilts_pinned(self):
        bad = self.root / "bad"
        bad.mkdir()
        (bad / "Bluetooth.apk").write_bytes(b"x")
        (bad / "libbluetooth_jni.so").write_bytes(b"x")
        with self.assertRaises(SystemExit):
            pb.system(str(self.root), str(bad))
        self.assertEqual((self.lib / "libbluetooth_jni.so").read_bytes(), self.stock)

    def test_prebuilt_pins_match_sources(self):
        text = (PREBUILTS / "SOURCES").read_text()
        self.assertIn(pb.WRAPPER, text)
        self.assertIn(pb.APP, text)

    def test_vendor(self):
        perms = self.root / "etc" / "permissions"
        perms.mkdir(parents=True)
        (perms / "android.hardware.wifi.xml").write_text("<permissions/>")
        for _ in range(2):
            self.assertEqual(pb.vendor(str(self.root)), 0)
        self.assertIn('feature name="android.hardware.bluetooth_le"',
                      (perms / "android.hardware.bluetooth_le.xml").read_text())


if __name__ == "__main__":
    unittest.main()
