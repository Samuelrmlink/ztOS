#!/usr/bin/env python3
"""ztclock-alarm-test.py: does a ztclock alarm ring on time from SoC sleep?

Run as the phone's user (not root), detached, under ztlock:
	setsid -f ztlock run sleep 'python3 ztclock-alarm-test.py' > /dev/null 2>&1
Log: /var/tmp/ztclock-alarm-test.log (and the soc.csv rows).

1. adds a one-time alarm (ID 99, label sleeptest) at the start of the minute
   2-3 min out, under the app's lock, with volume 20 and autostop 1 min in
   ~/.config/ztclock/settings (the old file is kept and put back);
2. `sudo zrn-sleepd once 600`: the SoC sleeps until something wakes it;
3. waits for $XDG_RUNTIME_DIR/ztclock.ring, logs the wake and the lateness
   of the ring against the alarm's minute, checks timed-ztclock holds the
   phone up, dismisses it through ztclock.sock after 8 s, then deletes the
   alarm and restores the settings, whatever happened.
"""
import fcntl, os, socket, subprocess, sys, time
from pathlib import Path

HOME = Path.home() / '.config/ztclock'
RUN = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
LOG = Path('/var/tmp/ztclock-alarm-test.log')
ID = 99


def say(*words):
	line = time.strftime('%Y-%m-%d %H:%M:%S ') + ' '.join(str(w) for w in words)
	with open(LOG, 'a') as f:
		f.write(line + '\n')


def edit_alarms(change):
	with open(HOME / '.lock', 'a+') as lock:
		fcntl.flock(lock, fcntl.LOCK_EX)
		path = HOME / 'alarms'
		lines = path.read_text().splitlines() if path.exists() else []
		lines = change(lines)
		tmp = HOME / '.alarms.sleeptest'
		tmp.write_text(''.join(l + '\n' for l in lines))
		os.replace(tmp, path)


def drop_test(lines):
	return [l for l in lines if not l.startswith(f'{ID} ')]


def main():
	now = time.time()
	due = (int(now) // 60 + 3) * 60	# the start of a minute 2-3 min out
	hhmm = time.strftime('%H:%M', time.localtime(due))
	settings = HOME / 'settings'
	old_settings = settings.read_text() if settings.exists() else None
	try:
		settings.write_text('volume 20\nautostop 1\n')
		edit_alarms(lambda lines: drop_test(lines) + [f'{ID} 1 {hhmm} 0000000 0 0 Ring02-WP8.wav sleeptest'])
		say(f'test alarm {ID} at {hhmm} ({due - now:.0f}s out)')
		time.sleep(3)
		say('ztclockd:', subprocess.run(['ztclockd', '--status'], capture_output=True, text=True).stdout.replace('\n', '; '))
		r = subprocess.run(['sudo', 'zrn-sleepd', 'once', '600'], capture_output=True, text=True)
		say('once:', (r.stdout + r.stderr).strip().replace('\n', ' | ') or f'exit {r.returncode}')
		ring = RUN / 'ztclock.ring'
		end = time.time() + 300
		while time.time() < end and not ring.exists():
			time.sleep(0.2)
		if not ring.exists():
			say('FAIL: no ring within 300 s of the return')
			return
		rang = ring.stat().st_mtime
		say(f'ring at {time.strftime("%H:%M:%S", time.localtime(rang))}, '
		    f'late {rang - due:+.1f}s against {hhmm}:00')
		time.sleep(8)
		inhibit = Path('/run/zrn-sleepd/inhibit/timed-ztclock')
		say('inhibit while ringing:', 'held until ' + time.strftime('%H:%M:%S', time.localtime(inhibit.stat().st_mtime))
		    if inhibit.exists() else 'MISSING')
		status = Path('/run/zrn-sleepd/status').read_text().replace('\n', ' ')
		say('zrn-sleepd while ringing:', status)
		s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
		s.sendto(b'dismiss', str(RUN / 'ztclock.sock'))
		time.sleep(2)
		say('after dismiss: ring', 'still there' if ring.exists() else 'gone',
		    '; inhibit', 'still there' if inhibit.exists() else 'gone')
	finally:
		edit_alarms(drop_test)
		if old_settings is None:
			settings.unlink(missing_ok=True)
		else:
			settings.write_text(old_settings)
		say('cleaned up: test alarm deleted, settings restored')
		say('ztclockd:', subprocess.run(['ztclockd', '--status'], capture_output=True, text=True).stdout.replace('\n', '; '))


if __name__ == '__main__':
	main()
