#!/usr/bin/env python3
"""zrn-sleepd's ShellWatch (a hung shell with the panel lit, burn-in) on the
host: a pretend /run/user, /proc and /sys/class/drm, no signals sent."""
import importlib.machinery
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'os/overlay/usr/lib/ztos/python'))
loader = importlib.machinery.SourceFileLoader('sleepd', str(ROOT / 'devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleepd'))
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)


class Phone:
	def __init__(self, base):
		self.base = Path(base)
		self.run = self.base / 'run'
		self.proc = self.base / 'proc'
		self.drm = self.base / 'drm'
		self.fb = self.base / 'fb0-blank'
		self.fb.write_text('0')
		self.now = 10000.0
		self.signals = []
		self.alive = {}
		self.lines, self.told = [], []
		self.session_stopped = 0
		self.panel(True)
		self.dp(True)

	def panel(self, lit, dpms='On'):
		d = self.drm / 'card0-DSI-1'
		d.mkdir(parents=True, exist_ok=True)
		(d / 'enabled').write_text('enabled' if lit else 'disabled')
		(d / 'dpms').write_text(dpms if lit else 'Off')

	def dp(self, lit):
		d = self.drm / 'card0-DP-1'
		d.mkdir(parents=True, exist_ok=True)
		(d / 'enabled').write_text('enabled' if lit else 'disabled')
		(d / 'dpms').write_text('On' if lit else 'Off')

	def shell(self, pid, name='ztmetro', tracer=0, state='S', dies_on=signal.SIGKILL):
		p = self.proc / str(pid)
		p.mkdir(parents=True, exist_ok=True)
		(p / 'status').write_text(f'Name:\t{name}\nState:\t{state} (x)\nTracerPid:\t{tracer}\n')
		self.alive[pid] = dies_on

	def beat(self, pid, age=0, waiting=0, uid=1000):
		d = self.run / str(uid)
		d.mkdir(parents=True, exist_ok=True)
		(d / 'ztmetro.alive').write_text(f'pid {pid}\nmono {self.now - age:.0f}\nlit 1\nwaiting {waiting:.0f}\nstep 180\n')

	def kill(self, pid, sig):
		self.signals.append((pid, sig))
		if self.alive.get(pid) == sig:
			import shutil
			shutil.rmtree(self.proc / str(pid))
			del self.alive[pid]
			self.panel(False)	# DRM's own dark, once the display file closes

	def watch(self):
		return sleepd.ShellWatch(runtime=self.run, proc=self.proc, drm=self.drm, fb_blank=self.fb,
					 clock=lambda: self.now, kill=self.kill, wait=lambda s: None,
					 stop_session=self.stop, tell=self.told.append, say=self.lines.append)

	def stop(self):
		self.session_stopped += 1


class ShellWatchTest(unittest.TestCase):
	def setUp(self):
		self.tmp = tempfile.TemporaryDirectory()
		self.phone = Phone(self.tmp.name)

	def tearDown(self):
		self.tmp.cleanup()

	def test_a_live_shell_is_left_alone(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=59)
		self.assertIsNone(ph.watch().check())
		self.assertEqual(ph.signals, [])

	def test_hung_with_the_panel_lit_is_ended(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 1)
		self.assertEqual(ph.watch().check(), 'hung')
		self.assertEqual(ph.signals[:2], [(500, signal.SIGTERM), (500, signal.SIGCONT)])
		self.assertIn((500, signal.SIGKILL), ph.signals)
		self.assertEqual(ph.fb.read_text(), '4')
		self.assertTrue(any('dark' in l for l in ph.lines), ph.lines)

	def test_a_shell_that_takes_sigterm_gets_no_sigkill(self):
		ph = self.phone
		ph.shell(500, dies_on=signal.SIGTERM)
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 1)
		ph.watch().check()
		self.assertNotIn((500, signal.SIGKILL), ph.signals)

	def test_a_stale_heartbeat_with_the_panel_dark_is_nothing(self):
		# screen off and the SoC asleep: no frames, no ticks, nothing lit
		ph = self.phone
		ph.panel(False)
		ph.shell(500)
		ph.beat(500, age=3 * 3600)
		self.assertIsNone(ph.watch().check())
		self.assertEqual(ph.signals, [])

	def test_only_the_phone_panel_counts_not_dp(self):
		ph = self.phone
		ph.panel(False)
		ph.dp(True)
		ph.shell(500)
		ph.beat(500, age=3600)
		self.assertIsNone(ph.watch().check())

	def test_the_aod_counts_as_lit(self):
		ph = self.phone
		ph.panel(True)	# the AOD: enabled, dpms On
		ph.shell(500)
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 30)
		self.assertEqual(ph.watch().check(), 'hung')

	def test_a_frame_waiting_too_long(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=40, waiting=sleepd.SHELL_HUNG_S - 30)
		self.assertEqual(ph.watch().check(), 'hung')

	def test_a_frame_waiting_briefly_is_fine(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=40, waiting=60)
		self.assertIsNone(ph.watch().check())

	def test_not_a_shell_or_gone(self):
		ph = self.phone
		ph.beat(500, age=3600)
		self.assertIsNone(ph.watch().check())	# no such process
		ph.shell(500, name='bash')		# the pid reused
		self.assertIsNone(ph.watch().check())
		ph.shell(501, state='Z')
		ph.beat(501, age=3600)
		self.assertIsNone(ph.watch().check())
		self.assertEqual(ph.signals, [])

	def test_no_heartbeat_file(self):
		ph = self.phone
		ph.shell(500)
		self.assertIsNone(ph.watch().check())

	def test_under_a_debugger(self):
		ph = self.phone
		ph.shell(500, tracer=42)
		ph.beat(500, age=3600)
		w = ph.watch()
		self.assertIsNone(w.check())
		self.assertEqual(ph.signals, [])
		self.assertEqual(len(ph.lines), 1)

	def test_a_stopped_shell_is_hung(self):
		# SIGSTOP (how the device test makes a hang): SIGCONT lets SIGTERM land
		ph = self.phone
		ph.shell(500, state='T', dies_on=signal.SIGCONT)
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 1)
		self.assertEqual(ph.watch().check(), 'hung')
		self.assertNotIn((500, signal.SIGKILL), ph.signals)

	def test_looks_at_most_every_30_s(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=100)
		w = ph.watch()
		self.assertIsNone(w.check())
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 100)
		ph.now += sleepd.SHELL_WATCH_EVERY_S - 1
		self.assertIsNone(w.check())
		ph.now += 1
		self.assertEqual(w.check(), 'hung')

	def test_the_next_shell_hears_of_it(self):
		ph = self.phone
		ph.shell(500)
		ph.beat(500, age=sleepd.SHELL_HUNG_S + 1)
		w = ph.watch()
		w.check()
		ph.now += 60
		ph.panel(True)
		ph.shell(600)
		ph.beat(600, age=5)
		w.check()
		self.assertEqual(len(ph.told), 1)
		self.assertTrue(ph.told[0].startswith('notify '))

	def test_hanging_again_and_again_stops_the_session(self):
		ph = self.phone
		w = ph.watch()
		results = []
		for i in range(sleepd.SHELL_KILLS_MAX):
			ph.panel(True)
			ph.shell(500 + i)
			ph.beat(500 + i, age=sleepd.SHELL_HUNG_S + 1)
			results.append(w.check())
			ph.now += sleepd.SHELL_HUNG_S + 60
		self.assertEqual(results[-1], 'stopped')
		self.assertEqual(results[:-1], ['hung'] * (sleepd.SHELL_KILLS_MAX - 1))
		self.assertEqual(ph.session_stopped, 1)
		ph.panel(True)
		ph.shell(900)
		ph.beat(900, age=3600)
		ph.now += 60
		self.assertIsNone(w.check())	# stopped: nothing more

	def test_kills_far_apart_never_stop_it(self):
		ph = self.phone
		w = ph.watch()
		for i in range(sleepd.SHELL_KILLS_MAX + 2):
			ph.panel(True)
			ph.shell(500 + i)
			ph.beat(500 + i, age=sleepd.SHELL_HUNG_S + 1)
			self.assertEqual(w.check(), 'hung')
			ph.now += sleepd.SHELL_KILLS_WINDOW_S
		self.assertEqual(ph.session_stopped, 0)


if __name__ == '__main__':
	unittest.main(verbosity=1)
