#!/usr/bin/env python3
"""zrn-sleep-why (every sleep blocker, ROADMAP 8.30) and zrn-sleepd's by= on a
fake root: parsing, labels, the lock screen's line and its privacy."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

TREE = Path(__file__).resolve().parents[2]
SBIN = TREE / 'devices/samsung-starlte/overlay/usr/local/sbin'
sys.path.insert(0, str(TREE / 'os/overlay/usr/lib/ztos/python'))
# the phone user's directories, under the fake root
RUNTIME, HOME = '/run/user/1000', '/home/samuel'


def load(name, source):
	loader = importlib.machinery.SourceFileLoader(name, str(source))
	spec = importlib.util.spec_from_loader(loader.name, loader)
	module = importlib.util.module_from_spec(spec)
	loader.exec_module(module)
	return module


why = load('zrn_sleep_why', SBIN / 'zrn-sleep-why')
sleepd = load('sleepd_for_why', SBIN / 'zrn-sleepd')

SINKS = '''Sink Input #41
	Driver: protocol-native.c
	Corked: no
	Properties:
		media.name = "Some Song Title - Private"
		application.name = "Brave"
		application.process.id = "2345"
		application.process.binary = "brave"
Sink Input #42
	Corked: yes
	Properties:
		application.name = "ztmusic"
		application.process.id = "2400"
Sink Input #43
	Corked: no
	Properties:
		application.process.binary = "mpv"
'''


class FakeRoot:
	"""A root with the files the tool reads, under a temporary directory."""
	def __init__(self, base):
		self.base = Path(base)
		self.runtime = self.base / 'run/user/1000'
		self.home = self.base / 'home/samuel'
		self.runtime.mkdir(parents=True)
		self.home.mkdir(parents=True)
		self.file('var/lib/zrn-sleep/enabled', '')
		self.file('run/zrn-sleepd/status', 'state=awake\nwhy=screen on\nsleeps=0\nwakes=0\nwoke_at=0\n')
		(self.base / 'run/zrn-sleepd/inhibit').mkdir(parents=True)

	def file(self, rel, text, mtime=None):
		p = self.base / rel
		p.parent.mkdir(parents=True, exist_ok=True)
		p.write_text(text)
		if mtime is not None:
			os.utime(p, (mtime, mtime))
		return p

	def proc(self, pid, comm, cgroup='0::/system.slice/foo.service', fds=()):
		self.file(f'proc/{pid}/comm', comm + '\n')
		self.file(f'proc/{pid}/cgroup', cgroup + '\n')
		d = self.base / f'proc/{pid}/fd'
		d.mkdir(parents=True, exist_ok=True)
		for i, target in enumerate(fds):
			os.symlink(target, d / str(i + 3))

	def connector(self, name, enabled):
		self.file(f'sys/class/drm/{name}/enabled', 'enabled\n' if enabled else 'disabled\n')

	def source(self, n, name, active_ms=0, wakeups=0):
		base = f'sys/class/wakeup/wakeup{n}'
		self.file(f'{base}/name', name + '\n')
		self.file(f'{base}/active_time_ms', f'{active_ms}\n')
		self.file(f'{base}/active_count', '1\n')
		self.file(f'{base}/wakeup_count', f'{wakeups}\n')
		self.file(f'{base}/prevent_suspend_time_ms', '0\n')


class Base(unittest.TestCase):
	def setUp(self):
		self.tmp = tempfile.TemporaryDirectory()
		self.root = FakeRoot(self.tmp.name)
		self.patches = [patch.object(why, 'ROOT', self.root.base),
				patch.object(why, 'user_dirs', return_value=(RUNTIME,
									      HOME, 'samuel')),
				patch.object(why, 'sink_inputs', return_value=why.parse_sink_inputs(SINKS))]
		for p in self.patches:
			p.start()

	def tearDown(self):
		for p in self.patches:
			p.stop()
		self.tmp.cleanup()

	def kinds(self, result):
		return [b['kind'] for b in result['blockers']]


class Parsing(Base):
	def test_sink_inputs(self):
		streams = why.parse_sink_inputs(SINKS)
		self.assertEqual(len(streams), 3)
		self.assertEqual(streams[0], {'app': 'Brave', 'pid': 2345, 'binary': 'brave', 'corked': False})
		self.assertTrue(streams[1]['corked'])
		self.assertEqual(streams[2]['binary'], 'mpv')

	def test_audio_names_apps_not_titles(self):
		self.root.file('run/zrnperfd.status', 'tickle_audio=2\n')
		self.root.proc(2345, 'brave', '0::/user.slice/user-1000.slice/session-1.scope')
		items = why.audio_blockers(why.Procs(), '', 'samuel')
		self.assertEqual([b['label'] for b in items], ['Brave playing', 'mpv playing'])
		self.assertEqual(items[0]['pid'], 2345)
		self.assertEqual(items[0]['unit'], 'session-1.scope')
		self.assertNotIn('Private', repr(items))

	def test_audio_only_when_perfd_counts_it(self):
		self.root.file('run/zrnperfd.status', 'tickle_audio=0\n')
		self.assertEqual(why.audio_blockers(why.Procs(), '', 'samuel'), [])

	def test_audio_without_pactl(self):
		self.root.file('run/zrnperfd.status', 'tickle_audio=1\n')
		items = why.audio_blockers(why.Procs(), '', 'samuel', streams=[])
		self.assertEqual([b['label'] for b in items], ['Sound playing'])

	def test_inhibits(self):
		now = time.time()
		inh = 'run/zrn-sleepd/inhibit'
		self.root.proc(812, 'ztos-gnss-gate', '0::/system.slice/ztos-gnss-gate.service')
		self.root.proc(900, 'ztos-iald', '0::/system.slice/ztos-iald.service')
		self.root.proc(77, 'mytool')
		self.root.file(f'{inh}/timed-gnss', '', mtime=now + 60)
		self.root.file(f'{inh}/timed-old', '', mtime=now - 5)			# expired: not held
		self.root.file(f'{inh}/.ztclock.new', 'ztclock ringing\n')		# on its way in
		self.root.file(f'{inh}/timed-ial-1', 'Podcasts: playing episode 12 (:1.42)\n', mtime=now + 300)
		self.root.file(f'{inh}/bench', 'pid=77\nlabel=Bench run\n', mtime=now - 120)
		items = why.inhibit_blockers(why.Procs(), now)
		by = {b['label']: b for b in items}
		self.assertEqual(sorted(by), ['Bench run', 'Location (GPS)', 'Podcasts'])
		gnss = by['Location (GPS)']
		self.assertEqual((gnss['kind'], gnss['pid'], gnss['unit']), ('gps', 812, 'ztos-gnss-gate.service'))
		self.assertIn('for another 1 min', gnss['detail'])
		ial = by['Podcasts']
		self.assertEqual((ial['pid'], ial['proc']), (900, 'ztos-iald'))
		self.assertNotIn('episode', repr(ial))		# the app's own words stay off
		bench = by['Bench run']
		self.assertEqual((bench['pid'], bench['proc']), (77, 'mytool'))
		self.assertIn('held for 2 min', bench['detail'])

	def test_gps_readers_merge_with_the_gate(self):
		now = time.time()
		self.root.proc(812, 'ztos-gnss-gate')
		self.root.proc(813, 'zrn-gnss-nmea')
		self.root.proc(1500, 'gpsd-client', fds=['socket:[4242]'])
		self.root.file('run/zrn-sleepd/inhibit/timed-gnss', '', mtime=now + 60)
		self.root.file('proc/net/tcp',
			       '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n'
			       '   0: 0100007F:C350 0100007F:277E 01 00000000:00000000 00:00000000 00000000  1000        0 4242 1\n')
		procs = why.Procs()
		items = why.gps_blockers(procs, why.inhibit_blockers(procs, now))
		self.assertEqual(len(items), 1)
		self.assertEqual(items[0]['short'], 'gpsd-client using location')
		self.assertIn('read by gpsd-client', items[0]['detail'])

	def test_camera(self):
		self.root.file('dev/zrn-cam0', '')
		self.root.proc(3000, 'ztcamera', fds=['/dev/zrn-cam0', '/dev/null'])
		items = why.camera_blockers(why.Procs())
		self.assertEqual([(b['kind'], b['pid'], b['proc']) for b in items], [('camera', 3000, 'ztcamera')])

	def test_no_camera_device_no_scan(self):
		with patch.object(why.Procs, 'fd_targets', side_effect=AssertionError('scanned')):
			self.assertEqual(why.camera_blockers(why.Procs()), [])

	def test_wakeup_sources(self):
		self.root.source(0, 'p9320-wireless', active_ms=2150355)
		self.root.source(1, 'umts_ipc0', active_ms=0)
		self.root.source(2, '[timerfd]', active_ms=40)
		items = why.wakeup_blockers()
		self.assertEqual([b['label'] for b in items], ['Wireless charger (kernel)', "A program's timer (kernel)"])
		self.assertIn('held for 36 min', items[0]['detail'])

	def test_display_lock_screen_and_aod_setting(self):
		self.root.connector('card0-DSI-1', True)
		self.root.file('run/user/1000/ztmetro.state', 'screen on\naod off\n')
		self.root.file('home/samuel/.config/ztmetro/config', 'accent = 0x0050ef\naod = on\n')
		items = why.display_blockers(RUNTIME, HOME)
		self.assertEqual([b['kind'] for b in items], ['display', 'aod'])

	def test_aod_showing(self):
		self.root.connector('card0-DSI-1', True)
		self.root.file('run/user/1000/ztmetro.state', 'screen off\naod on\n')
		items = why.display_blockers(RUNTIME, HOME)
		self.assertEqual([(b['kind'], b['label']) for b in items], [('aod', 'Always-on display showing')])

	def test_dock(self):
		self.root.connector('card0-DSI-1', True)
		self.root.connector('card0-DP-1', True)
		self.root.file('run/user/1000/ztmetro.state', 'screen on\naod off\n')
		items = why.display_blockers(RUNTIME, HOME)
		self.assertEqual([b['kind'] for b in items], ['dock', 'display'])

	def test_screen_off_no_aod(self):
		self.root.connector('card0-DSI-1', False)
		self.root.file('run/user/1000/ztmetro.state', 'screen off\naod off\n')
		self.assertEqual(why.display_blockers(RUNTIME, HOME), [])

	def test_charger(self):
		self.root.file('sys/class/power_supply/max77705-charger/type', 'USB\n')
		self.root.file('sys/class/power_supply/max77705-charger/online', '1\n')
		self.root.file('sys/class/power_supply/p9320-wireless/type', 'Wireless\n')
		self.root.file('sys/class/power_supply/p9320-wireless/online', '1\n')
		self.assertEqual([b['short'] for b in why.charger_blockers()], ['Charging'])

	def test_sleepd_states(self):
		(self.root.base / 'var/lib/zrn-sleep/enabled').unlink()
		items = why.sleepd_blockers({'state': 'awake'}, 'doze window')
		self.assertEqual([b['label'] for b in items], ['Sleep switched off', 'Letting background apps fetch messages'])
		self.root.file('var/lib/zrn-sleep/enabled', '')
		items = why.sleepd_blockers({'state': 'awake', 'last': 'modem'}, '')
		self.assertEqual(items[0]['label'], 'Just woke')
		self.assertIn('the modem', items[0]['detail'])


class Wakes(unittest.TestCase):
	def line(self, t, rest):
		return time.strftime('%Y-%m-%d %H:%M:%S ', time.localtime(t)) + rest

	def test_last_hour(self):
		now = time.time()
		lines = [
			self.line(now - 4000, 'woke: modem (umts_ipc0 +2) wake_stat=0x01000000 after 300s'),	# too old
			self.line(now - 3000, 'woke: modem (umts_ipc0 +2) wake_stat=0x01000000 after 300s by=umts_ipc0+2'),
			self.line(now - 2000, 'woke: modem (umts_router +1) wake_stat=0x01000000 after 300s'),
			self.line(now - 1500, 'woke: rtc wake_stat=0x80000002 after 300s (asleep 299.0s) by=zrn-exynos9810-rtc+1'),
			self.line(now - 1000, 'woke: cancelled wake_stat=0x00000000 after 2s'),			# no sleep
			self.line(now - 900, 'sleep cancelled: screen on'),
			self.line(now - 500, 'once: woke: modem (umts_ipc0 +1) wake_stat=0x01000000'),
		]
		w = why.wakes(lines, now)
		self.assertEqual(w['count'], 4)
		self.assertEqual(w['reasons'], {'modem': 3, 'rtc': 1})
		self.assertEqual(w['modem'], {'umts_ipc0': 2, 'umts_router': 1})
		self.assertEqual(w['sources'], {'umts_ipc0': 2, 'zrn-exynos9810-rtc': 1})
		self.assertEqual(why.wakes_text(w), 'Woke 4 times in the last hour, mostly by the modem (umts_ipc0)')

	def test_texts(self):
		self.assertEqual(why.wakes_text({'count': 0, 'reasons': {}, 'sources': {}, 'modem': {}}),
				 'No wakes from sleep in the last hour')
		w = {'count': 1, 'reasons': {'alarm': 1}, 'sources': {'alarmtimer.0.auto': 1}, 'modem': {}}
		self.assertEqual(why.wakes_text(w), "Woke once in the last hour, by an app's alarm (alarmtimer.0.auto)")
		w = {'count': 2, 'reasons': {'rtc': 2}, 'sources': {'zrn-exynos9810-rtc': 2}, 'modem': {}}
		self.assertEqual(why.wakes_text(w), 'Woke twice in the last hour, by its own timer')

	def test_rank(self):
		before = {'1': ['brave', 100, 5], '2': ['foo', 50, 5], '3': ['old', 10, 5]}
		after = {'1': ['brave', 700, 5], '2': ['foo', 80, 5], '3': ['reused', 900, 5], '4': ['new', 120, 500]}
		self.assertEqual(why.rank(before, after, 600, born_after=400),
				 [('brave', 60.0), ('new', 12.0), ('foo', 3.0)])

	def test_lifetime_rank(self):
		snap = {'1': ['a', 600, 0], '2': ['a', 600, 0], '3': ['b', 6000, 990]}
		self.assertEqual(why.lifetime_rank(snap, 1000), [('a', 72.0)])	# b: too young to say


class Line(unittest.TestCase):
	def b(self, kind, short):
		return why.blocker(kind, short)

	def test_headline(self):
		self.assertEqual(why.headline([self.b('display', 'Screen on')]), '')
		self.assertEqual(why.headline([self.b('display', 'Screen on')], always=True), 'Awake: the lock screen only')
		items = [self.b('audio', 'Brave playing'), self.b('gps', 'Location in use'), self.b('aod', 'Always-on display'),
			 self.b('display', 'Screen on')]
		self.assertEqual(why.headline(items), 'Awake: Brave playing · 2 more')
		self.assertEqual(why.headline([self.b('wakeup', 'Kernel: Wireless charger')]), 'Awake: Kernel: Wireless charger')


class EndToEnd(Base):
	def test_collect_order_and_forms(self):
		now = time.time()
		self.root.connector('card0-DSI-1', True)
		self.root.file('run/user/1000/ztmetro.state', 'screen on\naod off\n')
		self.root.file('run/zrnperfd.status', 'tickle_audio=1\n')
		self.root.proc(2345, 'brave')
		self.root.proc(50, 'ztclockd', '0::/user.slice/user-1000.slice/user@1000.service/app.slice/ztclockd.service')
		self.root.file('run/zrn-sleepd/inhibit/timed-ztclock', 'ztclock ringing\n', mtime=now + 600)
		self.root.source(0, 'p9320-wireless', active_ms=60000)
		self.root.file('var/lib/zrn-sleep/log',
			       time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now - 600)) +
			       ' woke: alarm wake_stat=0x80000002 after 120s by=alarmtimer.0.auto+1\n')
		result = why.collect(now=now)
		self.assertEqual(self.kinds(result), ['audio', 'audio', 'inhibit', 'wakeup', 'display'])
		lines = why.status_lines(result)
		self.assertEqual(lines[0], 'blocker=audio: Brave playing (pid 2345, brave, foo.service) - sound keeps the phone awake')
		self.assertIn('blocker=inhibit: Alarm ringing (pid 50, ztclockd, ztclockd.service) - timed-ztclock, for another 10 min',
			      lines)
		self.assertEqual(lines[-1], "woke_last_hour=1: Woke once in the last hour, by an app's alarm (alarmtimer.0.auto)")
		shell = why.shell_lines(result)
		self.assertEqual(shell[0], 'line\tAwake: Brave playing · 3 more')
		self.assertTrue(all(len(l.split('\t')) == 4 for l in shell if l.startswith('item\t')))
		self.assertIn('woke\tWoke once in the last hour, by an app\'s alarm (alarmtimer.0.auto)', shell)
		self.assertNotIn('Private', '\n'.join(shell))

	def test_nothing_but_the_lock_screen(self):
		self.root.connector('card0-DSI-1', True)
		self.root.file('run/user/1000/ztmetro.state', 'screen on\naod off\n')
		result = why.collect()
		self.assertEqual(why.shell_lines(result)[0], 'line\t')
		self.assertEqual(why.shell_lines(result, always=True)[0], 'line\tAwake: the lock screen only')

	def test_wakers_snapshots(self):
		self.root.proc(10, 'busy')
		self.root.file('proc/10/task/10/status', 'voluntary_ctxt_switches:\t100\nnonvoluntary_ctxt_switches:\t20\n')
		self.root.file('proc/10/stat', '10 (busy) S ' + ' '.join(['0'] * 18) + ' 100 0\n')
		self.root.file('proc/interrupts', '   CPU0  CPU1\n 12:  10  20  GIC-0  27 Level  arch_timer\n')
		self.root.file('proc/uptime', '1000.00 50.00\n')
		with patch.object(why.os, 'geteuid', return_value=1000):
			first = why.wakers(why.Procs(), RUNTIME)
			self.assertEqual(first['window'], 'averaged since each started')
			self.assertEqual(first['irqs'], [('arch_timer', 1.8)])
			self.root.file('proc/10/task/10/status', 'voluntary_ctxt_switches:\t700\nnonvoluntary_ctxt_switches:\t20\n')
			self.root.file('proc/uptime', '1600.00 50.00\n')
			second = why.wakers(why.Procs(), RUNTIME)
		self.assertEqual(second['window'], 'last 10 min')
		self.assertEqual(second['procs'], [('busy', 60.0)])
		self.assertEqual(len(list((self.root.runtime / 'zrn-sleep-why').glob('*.json'))), 2)


class SleepdBy(unittest.TestCase):
	def test_woke_by(self):
		before = {'wakeup1': ('umts_ipc0', 5), 'wakeup2': ('[timerfd]', 1), 'wakeup3': ('[timerfd]', 0),
			  'wakeup4': ('alarmtimer.0.auto', 3)}
		after = {'wakeup1': ('umts_ipc0', 7), 'wakeup2': ('[timerfd]', 2), 'wakeup3': ('[timerfd]', 1),
			 'wakeup4': ('alarmtimer.0.auto', 3), 'wakeup5': ('new', 9)}
		self.assertEqual(sleepd.woke_by(before, after), '[timerfd]+2,umts_ipc0+2')
		self.assertEqual(sleepd.by_text({'by': 'umts_ipc0+2'}), ' by=umts_ipc0+2')
		self.assertEqual(sleepd.by_text({}), '')

	def test_wake_counts_never_fails(self):
		with tempfile.TemporaryDirectory() as d:
			(Path(d) / 'wakeup0').mkdir()
			(Path(d) / 'wakeup0/name').write_text('gpio-keys\n')
			(Path(d) / 'wakeup0/wakeup_count').write_text('4\n')
			with patch.object(sleepd, 'WAKEUP', Path(d)):
				self.assertEqual(sleepd.wake_counts(), {'wakeup0': ('gpio-keys', 4)})
			with patch.object(sleepd, 'WAKEUP', Path(d) / 'missing'):
				self.assertEqual(sleepd.wake_counts(), {})

	def test_parsed_back(self):
		"""What zrn-sleepd logs, zrn-sleep-why reads."""
		now = time.time()
		info = {'modem_rx': 'umts_ipc0 +1', 'by': 'umts_ipc0+1'}
		line = (time.strftime('%Y-%m-%d %H:%M:%S ', time.localtime(now - 10)) +
			f'woke: {sleepd.woke_text("modem", info)} wake_stat=0x01000000 after 300s{sleepd.by_text(info)}')
		w = why.wakes([line], now)
		self.assertEqual((w['count'], w['modem'], w['sources']), (1, {'umts_ipc0': 1}, {'umts_ipc0': 1}))


if __name__ == '__main__':
	unittest.main()
