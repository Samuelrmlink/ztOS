#!/usr/bin/env python3
"""zrn-sleepd's guards (ROADMAP 8.47): one `run` at a time, no daemon from a
missing or unknown argument, an already-loaded backend with our parameters
counted as up, and why it switched sleeping off kept for the Sleep tile."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

TREE = Path(__file__).resolve().parents[2]
SOURCE = TREE / 'devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleepd'
PYPATH = str(TREE / 'os/overlay/usr/lib/ztos/python')
sys.path.insert(0, PYPATH)
loader = importlib.machinery.SourceFileLoader('sleepd_hardening', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)

SYMS = {'cpu_suspend': 'ffffffc010001000', 'cpu_resume': 'ffffffc010002000'}


class Arguments(unittest.TestCase):
	def call(self, *args):
		return subprocess.run([sys.executable, str(SOURCE), *args], capture_output=True, text=True,
				      env=dict(os.environ, PYTHONPATH=PYPATH), timeout=30)

	def test_no_argument_is_not_the_daemon(self):
		r = self.call()
		self.assertEqual(r.returncode, 2)
		self.assertIn('usage: zrn-sleepd run', r.stderr)

	def test_unknown_argument(self):
		r = self.call('stauts')
		self.assertEqual(r.returncode, 2)
		self.assertIn('usage:', r.stderr)

	def test_unit_says_run(self):
		unit = (TREE / 'devices/samsung-starlte/overlay/etc/systemd/system/zrn-sleepd.service').read_text()
		self.assertIn('ExecStart=/usr/local/sbin/zrn-sleepd run\n', unit)


class RunLock(unittest.TestCase):
	def test_second_run_refused(self):
		with tempfile.TemporaryDirectory() as d:
			lock = Path(d) / 'run.lock'
			first = sleepd.take_run_lock(lock)
			self.assertIsNotNone(first)
			self.assertEqual(sleepd.lock_holder(lock), str(os.getpid()))
			self.assertIsNone(sleepd.take_run_lock(lock))
			os.close(first)			# the first daemon gone: the next may run
			again = sleepd.take_run_lock(lock)
			self.assertIsNotNone(again)
			os.close(again)

	def test_second_process_refused(self):
		"""Another process holding the lock (the real daemon) stops `run`."""
		with tempfile.TemporaryDirectory() as d:
			lock = Path(d) / 'run.lock'
			holder = subprocess.Popen([sys.executable, '-c',
				'import fcntl,os,sys,time; fd=os.open(sys.argv[1], os.O_RDWR|os.O_CREAT); '
				'fcntl.flock(fd, fcntl.LOCK_EX); print("held", flush=True); time.sleep(30)', str(lock)],
				stdout=subprocess.PIPE, text=True)
			try:
				self.assertEqual(holder.stdout.readline().strip(), 'held')
				self.assertIsNone(sleepd.take_run_lock(lock))
			finally:
				holder.kill()
				holder.wait()
				holder.stdout.close()


class OffReason(unittest.TestCase):
	def setUp(self):
		self.tmp = tempfile.TemporaryDirectory()
		base = Path(self.tmp.name)
		self.notes = []
		self.patches = [patch.object(sleepd, 'STATE', base), patch.object(sleepd, 'ENABLED', base / 'enabled'),
				patch.object(sleepd, 'OFF_REASON', base / 'off_reason'),
				patch.object(sleepd, 'PAUSED', base / 'paused'),
				patch.object(sleepd, 'tell_shell', side_effect=self.notes.append)]
		for p in self.patches:
			p.start()

	def tearDown(self):
		for p in self.patches:
			p.stop()
		self.tmp.cleanup()

	def test_switch_off_keeps_why_and_notifies(self):
		sleepd.set_enabled(True)
		sleepd.switch_off('nowake', 'The phone did not wake from its last sleep.')
		self.assertFalse(sleepd.ENABLED.exists())
		self.assertEqual(sleepd.OFF_REASON.read_text(), 'nowake The phone did not wake from its last sleep.\n')
		self.assertEqual(self.notes, ['notify Power|Sleep switched off|The phone did not wake from its last sleep.'])

	def test_by_hand_forgets_why(self):
		sleepd.switch_off('driver', 'Its driver did not load.')
		sleepd.set_enabled(True)
		self.assertTrue(sleepd.ENABLED.exists())
		self.assertFalse(sleepd.OFF_REASON.exists())
		sleepd.switch_off('driver', 'Its driver did not load.')
		sleepd.set_enabled(False)
		self.assertFalse(sleepd.ENABLED.exists())
		self.assertFalse(sleepd.OFF_REASON.exists())

	def test_enable_ends_a_pause(self):
		sleepd.PAUSED.write_text('until=99999\n')
		sleepd.set_enabled(True)
		self.assertFalse(sleepd.PAUSED.exists())


class Backend(unittest.TestCase):
	def params(self, d, **values):
		p = Path(d)
		defaults = dict(enable_deep='Y', eint_wake=str(sleepd.EINT_WAKE),
				suspend_enter=str(int(SYMS['cpu_suspend'], 16)), resume=str(int(SYMS['cpu_resume'], 16)))
		defaults.update(values)
		for k, v in defaults.items():
			if v is not None:
				(p / k).write_text(v + '\n')
		return p

	def loaded(self, syms=SYMS, **values):
		with tempfile.TemporaryDirectory() as d, patch.object(sleepd, 'SLEEP', self.params(d, **values)), \
				patch.object(sleepd, 'module_options', return_value=[]):
			return sleepd.backend_loaded(syms)

	def test_ours(self):
		self.assertTrue(self.loaded())
		self.assertTrue(self.loaded(syms=None))

	def test_hub_bit_is_the_modules_own(self):
		self.assertTrue(self.loaded(eint_wake=str(sleepd.EINT_WAKE ^ (1 << 15))))

	def test_not_ours(self):
		self.assertFalse(self.loaded(enable_deep='N'))
		self.assertFalse(self.loaded(eint_wake=str(sleepd.EINT_WAKE ^ (1 << 20))))
		self.assertFalse(self.loaded(resume='12345'))

	def test_hidden_parameters_not_held_against_it(self):
		self.assertTrue(self.loaded(suspend_enter=None, resume=None))

	def test_modprobe_option_is_what_we_load(self):
		other = sleepd.EINT_WAKE ^ (1 << 20)
		with tempfile.TemporaryDirectory() as d, \
				patch.object(sleepd, 'SLEEP', self.params(d, eint_wake=str(other))), \
				patch.object(sleepd, 'module_options', return_value=[f'eint_wake={other:#x}']):
			self.assertTrue(sleepd.backend_loaded(SYMS))

	def setup_with(self, d, insmod_out, loaded_after):
		"""setup() from nothing loaded; insmod says `insmod_out` and leaves
		the module (ours) there when `loaded_after`."""
		params = Path(d) / 'parameters'
		hotplug = Path(d) / 'hotplug'
		hotplug.write_text('0')
		writes, logs = [], []

		def run(*cmd, timeout=20, check=False):
			if cmd[0] == 'insmod' and 'zrn-sleep-init' in str(cmd[1]):
				if loaded_after:
					params.mkdir()
					self.params(params)
				return SimpleNamespace(returncode=1 if insmod_out else 0, stdout=insmod_out)
			return SimpleNamespace(returncode=0, stdout='')
		kallsyms = '\n'.join(f'{a} T {n}' for n, a in SYMS.items())
		real_read = sleepd.read
		with patch.object(sleepd, 'SLEEP', params), patch.object(sleepd, 'HOTPLUG', hotplug), \
				patch.object(sleepd, 'run', side_effect=run), \
				patch.object(sleepd, 'module_options', return_value=[]), \
				patch.object(sleepd, 'read', side_effect=lambda path, default='':
					     kallsyms if str(path) == '/proc/kallsyms' else real_read(path, default)), \
				patch.object(sleepd, 'write', side_effect=lambda p, v: writes.append((str(p), v))), \
				patch.object(sleepd, 'log', side_effect=lambda *w: logs.append(' '.join(map(str, w)))), \
				patch.object(sleepd.Path, 'exists', autospec=True,
					     side_effect=lambda self: True if str(self) == '/sys/module/zrn_exynos_rtc'
					     else os.path.exists(self)):
			return sleepd.setup(), writes, logs

	def test_insmod_file_exists_with_our_parameters_is_up(self):
		with tempfile.TemporaryDirectory() as d:
			ok, writes, logs = self.setup_with(d, 'insmod: ERROR: could not insert module: File exists', True)
		self.assertTrue(ok)
		self.assertIn(('/sys/power/mem_sleep', 's2idle'), writes)
		self.assertTrue(any('already loaded, same parameters' in l for l in logs))

	def test_insmod_failure_is_still_a_failure(self):
		with tempfile.TemporaryDirectory() as d:
			ok, _, logs = self.setup_with(d, 'insmod: ERROR: could not insert module: Invalid parameters', False)
		self.assertFalse(ok)
		self.assertTrue(any('backend did not load' in l for l in logs))

	def test_already_loaded_at_start(self):
		with tempfile.TemporaryDirectory() as d:
			params = self.params(d)
			real_read = sleepd.read
			with patch.object(sleepd, 'SLEEP', params), \
					patch.object(sleepd, 'module_options', return_value=[]), \
					patch.object(sleepd, 'read', side_effect=lambda path, default='':
						     's2idle [deep]' if str(path) == '/sys/power/mem_sleep' else real_read(path, default)), \
					patch.object(sleepd, 'write'), patch.object(sleepd, 'log'), \
					patch.object(sleepd, 'run', side_effect=AssertionError('nothing to load')):
				self.assertTrue(sleepd.setup())


if __name__ == '__main__':
	unittest.main()
