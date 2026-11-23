#!/usr/bin/env python3
"""zrn-sleepd's ShellWatch on the phone, against a dummy: never the session's
shell. Real /proc, the real DSI panel state, real signals, but to a copy of
sleep(1) named ztmetro that this script starts; a stale heartbeat for it in
a temporary runtime directory; fb0's blank and the session stop replaced.

  sudo python3 shell-watch-device-test.py [/usr/local/sbin/zrn-sleepd]

The panel must be lit (screen on, or the AOD) for the "hung" case."""
import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

source = sys.argv[1] if len(sys.argv) > 1 else '/usr/local/sbin/zrn-sleepd'
loader = importlib.machinery.SourceFileLoader('sleepd', source)
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)

fails = 0


def check(what, cond):
	global fails
	print(('PASS ' if cond else 'FAIL ') + what)
	fails += not cond


with tempfile.TemporaryDirectory() as d:
	d = Path(d)
	dummy = d / 'ztmetro'
	shutil.copy('/bin/sleep', dummy)
	proc = subprocess.Popen([str(dummy), '600'])
	time.sleep(0.3)
	real_shells = subprocess.run(['pgrep', '-x', 'ztmetro'], capture_output=True, text=True).stdout.split()
	real_shells = [int(p) for p in real_shells if int(p) != proc.pid]
	(d / 'run' / '1000').mkdir(parents=True)
	fb = d / 'fb0-blank'
	fb.write_text('0')
	lines, told, kills, stops = [], [], [], []

	def kill(pid, sig):
		assert pid == proc.pid, f'refusing to signal {pid}: not the dummy'
		kills.append(sig)
		os.kill(pid, sig)

	def beat(age):
		(d / 'run' / '1000' / 'ztmetro.alive').write_text(
			f'pid {proc.pid}\nmono {time.monotonic() - age:.0f}\nlit 1\nwaiting 0\nstep 180\n')

	w = sleepd.ShellWatch(runtime=d / 'run', fb_blank=fb, kill=kill, stop_session=lambda: stops.append(1),
			      tell=told.append, say=lines.append)
	lit = sleepd.panel_lit()
	print('the panel lit:', lit, '; the session shell(s):', real_shells, '; the dummy:', proc.pid)
	beat(30)
	check('a fresh heartbeat: nothing', w.check() is None and not kills)
	w.next_at = 0
	beat(sleepd.SHELL_HUNG_S + 10)
	t0 = time.monotonic()
	result = w.check()
	took = time.monotonic() - t0
	if lit:
		check(f'a stale heartbeat, the panel lit: ended ({result}, {took:.1f} s)', result == 'hung')
		check('the dummy is gone', proc.poll() is not None)
		check('fb0 blank asked', fb.read_text() == '4')
	else:
		check('a stale heartbeat, the panel dark: nothing', result is None and not kills)
		proc.kill()
	proc.wait()
	for shell in real_shells:
		check(f'the session shell {shell} is untouched', Path(f'/proc/{shell}').exists())
	for l in lines:
		print('  log:', l)
	print('signals sent to the dummy:', [s.name for s in kills])
sys.exit(1 if fails else 0)
