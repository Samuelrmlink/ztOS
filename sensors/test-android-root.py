#!/usr/bin/env python3
"""zrn-android-root with every mount mocked: no phone, no root, nothing mounted.

A refused mount must fail the script and leave no half-made root behind."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "devices/samsung-starlte/overlay/usr/local/sbin/zrn-android-root"

# mountpoint answers from a list of "mounted" paths; mount adds to it, umount
# clears it, and a mount whose arguments contain $FAIL is refused.
MOUNT = r"""#!/bin/sh
echo "mount $*" >> "$LOG"
case " $* " in *"$FAIL"*) [ -n "$FAIL" ] && exit 32 ;; esac
for last; do :; done
case " $* " in *--make-*) ;; *) echo "$last" >> "$MOUNTED"; mkdir -p "$last" ;; esac
"""
UMOUNT = r"""#!/bin/sh
echo "umount $*" >> "$LOG"
case " $* " in *binderfs*) exit 0 ;; esac
for last; do :; done
grep -vx "$last" "$MOUNTED" > "$MOUNTED.new"; mv "$MOUNTED.new" "$MOUNTED"
"""
MOUNTPOINT = r"""#!/bin/sh
for last; do :; done
grep -qx "$last" "$MOUNTED"
"""


class AndroidRoot(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = pathlib.Path(temp.name)
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        for name, text in (("mount", MOUNT), ("umount", UMOUNT), ("mountpoint", MOUNTPOINT)):
            (bin_dir / name).write_text(text)
            (bin_dir / name).chmod(0o755)
        self.root = self.tmp / "run/android"
        (self.tmp / "run").mkdir()
        self.mounted = self.tmp / "mounted"
        self.mounted.write_text("")
        self.log = self.tmp / "log"
        self.log.write_text("")
        source = SOURCE.read_text()
        for path, new in (("/run/android", self.root), ("/run/zrn-android-root.lock", self.tmp / "lock"),
                          ("/mnt/", self.tmp / "mnt/"), ("/var/lib/", self.tmp / "var/")):
            source = source.replace(path, str(new) + ("/" if path.endswith("/") else ""))
        self.script = self.tmp / "zrn-android-root"
        self.script.write_text(source)
        self.env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}", LOG=str(self.log),
                        MOUNTED=str(self.mounted), FAIL="")

    def run_up(self, fail=""):
        return subprocess.run(["sh", str(self.script), "up"], text=True, capture_output=True,
                              env=dict(self.env, FAIL=fail))

    def test_all_good(self):
        r = self.run_up()
        self.assertEqual(r.returncode, 0, r.stderr)
        log = self.log.read_text()
        self.assertIn("--bind", log)
        self.assertIn(str(self.root), self.mounted.read_text().split("\n"))

    def test_any_refused_mount_fails_and_leaves_no_root(self):
        self.run_up()
        steps = [line for line in self.log.read_text().splitlines() if line.startswith("mount ")]
        self.assertGreaterEqual(len(steps), 9)
        for step in steps:
            with self.subTest(step):
                self.mounted.write_text("")
                self.log.write_text("")
                fail = " ".join(step.split()[1:])
                r = self.run_up(fail)
                self.assertNotEqual(r.returncode, 0, "reported success")
                self.assertIn("failed:", r.stderr)
                self.assertNotIn(str(self.root), self.mounted.read_text().split("\n"))

    def test_no_apex_on_an_older_system(self):
        # the script's own check: nothing was created at the release path here
        r = self.run_up()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("com.android.runtime.release", self.log.read_text())

    def test_apex_mounted_when_present(self):
        apex = self.tmp / "mnt/system/system/apex/com.android.runtime.release"
        apex.mkdir(parents=True)
        r = self.run_up()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("com.android.runtime.release", self.log.read_text())

    def test_already_up_is_left_alone(self):
        self.mounted.write_text(f"{self.root}\n")
        (self.root / "system/bin").mkdir(parents=True)
        r = self.run_up(fail="--bind")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("mount --bind", self.log.read_text())


if __name__ == "__main__":
    unittest.main()
