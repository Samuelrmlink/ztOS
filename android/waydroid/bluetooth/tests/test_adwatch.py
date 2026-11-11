"""The privileged helpers, ztos-bt-adwatch (adwatch/) and ztos-bt-connparam
(connparam/): built with the warnings on, their decisions tested."""
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")


class AdWatch(unittest.TestCase):
    def setUp(self):
        self.cc = shutil.which("cc") or shutil.which("gcc")
        if not self.cc:
            self.skipTest("no C compiler")

    def build(self, source, out, *extra):
        subprocess.run([self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *extra,
                        "-o", out, os.path.join(ROOT, source)], check=True)

    def test_filter(self):
        with tempfile.TemporaryDirectory() as d:
            self.build("adwatch/test-adwatch.c", os.path.join(d, "t"))
            r = subprocess.run([os.path.join(d, "t")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("PASS", r.stdout)

    def test_builds_and_refuses_to_run_by_hand(self):
        with tempfile.TemporaryDirectory() as d:
            self.build("adwatch/ztos-bt-adwatch.c", os.path.join(d, "a"))
            r = subprocess.run([os.path.join(d, "a")], capture_output=True, text=True,
                               env={"PATH": os.environ.get("PATH", "")})
            self.assertEqual(r.returncode, 1)
            self.assertIn("ztos-bt-adwatch.socket", r.stderr)


    def test_connparam_decisions(self):
        with tempfile.TemporaryDirectory() as d:
            self.build("connparam/test-connparam.c", os.path.join(d, "t"))
            r = subprocess.run([os.path.join(d, "t")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("PASS", r.stdout)

    def test_connparam_builds_and_refuses_to_run_by_hand(self):
        with tempfile.TemporaryDirectory() as d:
            self.build("connparam/ztos-bt-connparam.c", os.path.join(d, "c"))
            r = subprocess.run([os.path.join(d, "c")], capture_output=True, text=True,
                               env={"PATH": os.environ.get("PATH", "")})
            self.assertEqual(r.returncode, 1)
            self.assertIn("ztos-bt-connparam.socket", r.stderr)


if __name__ == "__main__":
    unittest.main()
