#!/usr/bin/env python3
"""The S6E3HA8's normal-mode self-move, tried by hand (ROADMAP 8.37; the
owner approved this trial on 2026-11-12, exactly these sequences).

  sudo python3 selfmove-trial.py OUTDIR [MINUTES]	(inside `ztlock run burn-in`)

The sequences are Samsung's, from the G960F kernel's s6e3ha8_aod_panel.h
(tw10-android-4.9-q): s6e3ha8_enable_selfmove (lines 796-810, pattern row 0
of s6e3ha8_aod_self_move_patt_tbl, lines 277-291) and
s6e3ha8_disable_selfmove (812-817). They go through the kernel's panel_cmd
(kernel/patch-decon-panel-io.py): one write, parsed whole before anything is
sent, sent between two frames under the panel lock; nothing is sent while
the pipeline is off, so the screen is kept on throughout.

Steps: (1) disable alone; (2) enable, then MINUTES (20) with the screen on,
a grim a minute and dmesg watched; (3) disable, screen off and on again.
Both keys are closed after every write, and on every way out once anything
was sent (an error, an interrupt): disable, then F0 A5 A5 and FC A5 A5.
F1, C0 and C1 are never sent: every packet's first byte is checked against
the approved list before anything is written."""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PANEL_CMD = Path('/sys/devices/platform/soc@0/16030000.decon/panel_cmd')

# 2026-11-12: this exact trial blanked the panel ~2 min after the enable (DECON's
# flips stopped completing; a forced power-off brought it back). Self-move is
# proposal-only again (ztmetro/BURN-IN.md, post-mortem). Kept as the record;
# it refuses to run.
sys.exit('selfmove-trial: retired after the 2026-11-12 blank panel; see ztmetro/BURN-IN.md')
RUNTIME = '/run/user/1000'
ENV = dict(os.environ, XDG_RUNTIME_DIR=RUNTIME, WAYLAND_DISPLAY='wayland-0')
APPROVED_FIRST = {0xFC, 0xF0, 0xB0, 0xFE, 0x76, 0x77, 0x7C, 0x7D}
FORBIDDEN_FIRST = {0xF1, 0xC0, 0xC1}

PATTERN_ROW0 = ([0x00] * 6 + [0x55, 0x10] + [0x01, 0x50, 0x05, 0x10] * 4 + [0x01] +
		[0x00] * 7 + [0x00] * 32 + [0x00, 0x00] + [0x41, 0xC3] * 4 + [0x41])
assert len(PATTERN_ROW0) == 75, len(PATTERN_ROW0)

ENABLE = [
	[0xFC, 0x5A, 0x5A],
	[0xF0, 0x5A, 0x5A],
	[0xB0, 0x15], [0xFE, 0x00],				# move setting (FE at 0x15)
	[0x76, 0x09, 0x0F, 0x85, 0xA0, 0x0B, 0x90],		# AOD porch setting
	[0x77, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00],	# self timer on
	[0x7C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],	# self-move set
	[0x7D] + PATTERN_ROW0,					# the pattern, row 0
	[0xF0, 0xA5, 0xA5],
	[0xFC, 0xA5, 0xA5],
]
DISABLE = [
	[0xF0, 0x5A, 0x5A],
	[0x7D, 0x01],						# self-move sync off
	[0x76, 0x09, 0x0F, 0x00],				# normal porch setting
	[0xF0, 0xA5, 0xA5],
]
CLOSE = [[0xF0, 0xA5, 0xA5], [0xFC, 0xA5, 0xA5]]
# the display's lines (s9p-decon, DSIM, TE, flips) that say something went wrong;
# its ordinary ones ("brightness level N sent in X us") are not a reason to stop
DISPLAY = re.compile(r'decon|dsim|s9p|\bte\b|flip|panel|drm', re.I)
BAD = re.compile(r'error|fail|timeout|timed out|underrun|not latch|esd|warn|stuck|hang|bug|oops', re.I)


class _Errors:
	def search(self, line):
		return DISPLAY.search(line) and BAD.search(line)


ERRORS = _Errors()

out = Path(sys.argv[1])
minutes = int(sys.argv[2]) if len(sys.argv) > 2 else 20
out.mkdir(parents=True, exist_ok=True)
sent_any = False


def say(*w):
	line = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in w)
	print(line, flush=True)
	with open(out / 'trial.log', 'a') as f:
		f.write(line + '\n')


def text(packets):
	for p in packets:
		assert p[0] in APPROVED_FIRST and p[0] not in FORBIDDEN_FIRST, f'packet {p[0]:02X} not approved'
		assert 1 < len(p) < 256
	return '; '.join(' '.join(f'{b:02X}' for b in p) for p in packets) + '\n'


def send(name, packets):
	global sent_any
	t = text(packets)
	if not lit():
		raise RuntimeError(f'{name}: the panel is off, panel_cmd would send nothing')
	sent_any = True
	PANEL_CMD.write_text(t)
	say(f'sent {name}: {t.strip()}')


def close_keys():
	PANEL_CMD.write_text(text(CLOSE))
	say('keys closed: F0 A5 A5; FC A5 A5')


def lit():
	return all(Path(p).read_text().strip() == 'enabled'
		   for p in map(str, Path('/sys/class/drm').glob('card*-DSI-*/enabled')))


def ctl(line):
	with open(f'{RUNTIME}/ztmetro.ctl', 'w') as f:
		f.write(line + '\n')


def shot(name):
	r = subprocess.run(['sudo', '-u', '#1000', 'env', f'XDG_RUNTIME_DIR={RUNTIME}', 'WAYLAND_DISPLAY=wayland-0',
			    'grim', f'/tmp/selfmove-{name}.png'], capture_output=True, text=True)
	if r.returncode == 0:
		subprocess.run(['mv', f'/tmp/selfmove-{name}.png', str(out / f'{name}.png')])
	else:
		say(f'grim {name} failed: {r.stderr.strip()}')


def dmesg():
	return subprocess.run(['dmesg'], capture_output=True, text=True).stdout.splitlines()


def new_errors(before):
	lines = dmesg()[len(before):]
	return [l for l in lines if ERRORS.search(l)], lines


def keep_on():
	ctl('screen on')


base = dmesg()
try:
	keep_on()
	time.sleep(2)
	say(f'start: panel {"lit" if lit() else "off"}; battery '
	    f'{Path("/sys/class/power_supply/max170xx_battery/capacity").read_text().strip()} %')
	# (1) disable alone
	shot('0-before')
	send('disable (step 1)', DISABLE)
	close_keys()
	time.sleep(3)
	shot('1-after-disable')
	errs, _ = new_errors(base)
	if errs:
		raise RuntimeError('dmesg after step 1: ' + ' | '.join(errs))
	# (2) enable, row 0, then MINUTES lit
	send('enable row 0 (step 2)', ENABLE)
	close_keys()
	t0 = time.monotonic()
	for m in range(minutes):
		for _ in range(6):
			keep_on()
			time.sleep(10)
		shot(f'2-enabled-{m + 1:02d}min')
		errs, _ = new_errors(base)
		say(f'{m + 1} min enabled, panel {"lit" if lit() else "OFF"}, dmesg matches {len(errs)}')
		if errs:
			raise RuntimeError('dmesg during step 2: ' + ' | '.join(errs))
		if not lit():
			raise RuntimeError('the panel went off during step 2')
	# (3) disable, then off and on
	send('disable (step 3)', DISABLE)
	close_keys()
	time.sleep(2)
	shot('3-after-disable')
	ctl('screen off')
	time.sleep(5)
	ctl('screen on')
	time.sleep(3)
	shot('3-after-off-on')
	errs, lines = new_errors(base)
	say(f'done: dmesg matches {len(errs)}; new dmesg lines {len(lines)}')
	for l in lines:
		say('  dmesg:', l)
except BaseException as e:
	say(f'STOPPED: {e!r}')
	if sent_any:
		try:
			if not lit():
				ctl('screen on')
				time.sleep(3)
			send('disable (on the way out)', DISABLE)
		except BaseException as e2:
			say(f'disable on the way out failed: {e2!r}')
		try:
			close_keys()
		except BaseException as e3:
			say(f'closing the keys failed: {e3!r}')
	errs, lines = new_errors(base)
	for l in lines:
		say('  dmesg:', l)
	sys.exit(1)
