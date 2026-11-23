#!/usr/bin/env python3
"""zrn-thermald against a fake sysfs: thresholds, hysteresis, unknown and stale
readings, the power-off's confirmation, the ceilings beside zrnperfd's, the
GPU's cooling state and the shell's notifications.

	python3 devices/samsung-starlte/test-zrn-thermald.py
"""
import importlib.machinery, importlib.util, os, shutil, sys, tempfile, unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'overlay/usr/local/sbin/zrn-thermald')
loader = importlib.machinery.SourceFileLoader('zrn_thermald', SRC)
spec = importlib.util.spec_from_loader('zrn_thermald', loader)
T = importlib.util.module_from_spec(spec)
loader.exec_module(T)
T.log = lambda msg, prio=6: LOG.append(msg)
LOG = []
M = lambda k: T.THROTTLE[k][0]
H = lambda k: T.THROTTLE[k][1]

BIG = '2704000 2327000 1794000 1469000 1066000 400000'
LITTLE = '1794000 1499333 1150500 650000 349916'


class Fake:
	"""A phone's worth of sysfs under a temporary root."""

	def __init__(self):
		self.root = tempfile.mkdtemp(prefix='thermald-')
		T.ROOT = self.root
		T.DRY_RUN = False
		self.samples = 0
		self.therm(battery=300, ap=350)
		for n, steps, cpus in (('policy0', LITTLE, '0 1 2 3'), ('policy4', BIG, '4 5 6 7')):
			top = max(int(x) for x in steps.split())
			self.put(T.CPUFREQ + '/%s/scaling_available_frequencies' % n, steps)
			self.put(T.CPUFREQ + '/%s/cpuinfo_max_freq' % n, top)
			self.put(T.CPUFREQ + '/%s/scaling_max_freq' % n, top)
			self.put(T.CPUFREQ + '/%s/related_cpus' % n, cpus)
		for k, v in (('big', 45), ('little', 44), ('gpu', 43)):
			self.put(T.TMU + '/temp_' + k, v)
		self.put(T.TZ + '/thermal_zone0/type', 'max170xx_battery')
		self.put(T.TZ + '/thermal_zone0/temp', 99000)	# skipped: the gauge's copy
		self.put(T.TZ + '/cooling_device0/type', 'devfreq-17500000.gpu')
		self.put(T.TZ + '/cooling_device0/max_state', 2)
		self.put(T.TZ + '/cooling_device0/cur_state', 0)
		self.put(T.GPU_DEVFREQ + '/device/power/control', 'auto')
		os.makedirs(self.root + T.STATE)
		os.makedirs(self.root + '/run/user/1000')
		self.pipe = None

	def put(self, path, value):
		os.makedirs(os.path.dirname(self.root + path), exist_ok=True)
		with open(self.root + path, 'w') as f:
			f.write('%s\n' % value)

	def get(self, path):
		with open(self.root + path) as f:
			return f.read().strip()

	def therm(self, advance=True, **vals):
		"""zrn-therm's files; None removes one (a read that fails: unknown)."""
		for k, v in vals.items():
			path = self.root + T.THERM + '/' + k
			if v is None:
				if os.path.exists(path):
					os.remove(path)
			else:
				self.put(T.THERM + '/' + k, v)
		if advance:
			self.samples += 1
		self.put(T.THERM + '/status', 'samples %d errors 0 last_err 0 period_ms 5000 cost_us_last 3400 '
			 'cost_us_avg 3400' % self.samples)

	def shell(self):
		"""The shell's control pipe, with a reader on it."""
		path = self.root + '/run/user/1000/ztmetro.ctl'
		if os.path.exists(path):
			os.remove(path)
		os.mkfifo(path)
		self.pipe = os.open(path, os.O_RDONLY | os.O_NONBLOCK)

	def lines(self):
		try:
			return os.read(self.pipe, 65536).decode().splitlines()
		except BlockingIOError:
			return []

	def done(self):
		if self.pipe is not None:
			os.close(self.pipe)
		shutil.rmtree(self.root)


class Policy(unittest.TestCase):
	def test_throttle_marks_and_hysteresis(self):
		lv = T.throttle_level
		self.assertEqual(lv(0, {'ap': 449}), 0)
		self.assertEqual(lv(0, {'ap': M('ap')[0]}), 1)
		self.assertEqual(lv(0, {'ap': M('ap')[2]}), 3)		# straight up
		top = M('ap')[2]
		self.assertEqual(lv(3, {'ap': top - 1}), 3)			# held within HYST
		self.assertEqual(lv(3, {'ap': top - H('ap') - 1}), 2)		# one step down
		self.assertEqual(lv(3, {'ap': 200}), 0)				# all the way when cool
		# the hottest input wins, another one still hot holds the level
		self.assertEqual(lv(0, {'ap': 300, 'battery': M('battery')[1]}), 2)
		self.assertEqual(lv(2, {'ap': 300, 'battery': M('battery')[1] - 10}), 2)

	def test_throttle_unknown(self):
		lv = T.throttle_level
		self.assertEqual(lv(0, {'ap': None, 'battery': None}), 0)	# unknown does not raise
		self.assertEqual(lv(2, {'ap': None, 'battery': None}), 0)	# nor hold
		self.assertEqual(lv(0, {'big': M('big')[3]}), 4)

	def test_warn_hysteresis_and_unknown(self):
		w = T.warn_state
		b = T.WARN['battery']
		self.assertEqual(w(set(), {'battery': b - 1}), set())
		self.assertEqual(w(set(), {'battery': b}), {'battery'})
		self.assertEqual(w({'battery'}, {'battery': b - T.WARN_HYST}), {'battery'})
		self.assertEqual(w({'battery'}, {'battery': b - T.WARN_HYST - 1}), set())
		self.assertEqual(w({'battery'}, {'battery': None}), {'battery'})	# unknown keeps it
		self.assertEqual(w(set(), {'ap': T.WARN['ap']}), {'ap'})

	def test_confirm(self):
		c = T.Confirm()
		hot = T.SHUTDOWN_BATTERY + 1
		self.assertFalse(c.see(hot, 1))
		self.assertFalse(c.see(hot, 1))		# the same sample: not counted
		self.assertFalse(c.see(hot, 2))
		self.assertFalse(c.see(None, 3))	# unknown resets
		self.assertFalse(c.see(hot, 4))
		self.assertFalse(c.see(hot, 5))
		self.assertTrue(c.see(hot, 6))
		self.assertFalse(c.see(hot, 7))		# once per hot spell (the dry run's repeats)
		self.assertFalse(c.see(hot, 8))
		c = T.Confirm()
		for s in range(10):
			self.assertFalse(c.see(hot, None))	# no sample count: never confirmed
		c = T.Confirm()
		self.assertFalse(c.see(hot, 1))
		self.assertFalse(c.see(hot, 2))
		self.assertFalse(c.see(hot - 1, 3))	# at the mark (not above) resets
		self.assertFalse(c.see(hot, 4))

	def test_unknown_needs_time_and_looks(self):
		u = T.Unknown()
		self.assertFalse(u.see(None, 0))
		self.assertFalse(u.see(None, 5))
		self.assertFalse(u.see(None, 10))			# 3 looks, 10 s
		self.assertTrue(u.see(None, T.UNKNOWN_WARN_S))
		self.assertFalse(u.see(300, 40))
		self.assertFalse(u.see(None, 1000))		# starts over


class Daemon(unittest.TestCase):
	def setUp(self):
		LOG.clear()
		self.f = Fake()
		self.offs = []
		self.d = T.Thermald(poweroff=self.offs.append)

	def tearDown(self):
		self.f.done()

	def test_reads_inputs(self):
		self.f.put(T.TZ + '/thermal_zone1/type', 'cpu-thermal')
		self.f.put(T.TZ + '/thermal_zone1/temp', 51500)
		r = T.readings()
		self.assertEqual(r['battery'], 300)
		self.assertEqual(r['big'], 450)
		self.assertEqual(r['tz'], 515)			# not the gauge's 99 C
		self.assertIsNone(r['usb'])			# no file: unknown
		self.f.put(T.THERM + '/charger', 'garbage')
		self.assertIsNone(T.readings()['charger'])
		self.f.put(T.TMU + '/temp_big', 0)		# 0 C from a sensor not read yet
		self.assertIsNone(T.readings()['big'])

	def test_power_off_confirmed(self):
		hot = T.SHUTDOWN_BATTERY + 1
		for i in range(T.CONFIRM - 1):
			self.f.therm(battery=hot)
			self.d.tick(i * 5)
		self.assertEqual(self.offs, [])
		self.f.therm(battery=hot)
		self.d.tick(20)
		self.assertEqual(len(self.offs), 1)
		self.assertIn('%.1f C' % (hot / 10), self.offs[0])
		why = self.f.get(T.STATE + '/shutdown')
		self.assertIn('the battery was at', why)
		self.assertIn('the battery was at', self.f.get(T.STATE + '/events.log'))
		self.f.therm(battery=hot)
		self.d.tick(25)
		self.assertEqual(len(self.offs), 1)		# once

	def test_no_power_off_on_stale(self):
		hot = T.SHUTDOWN_BATTERY + 1
		for i in range(10):
			self.f.therm(advance=False, battery=hot)	# the same reading over and over
			self.d.tick(i * 5)
		self.assertEqual(self.offs, [])			# only the first one counted

	def test_no_power_off_on_unknown(self):
		hot = T.SHUTDOWN_BATTERY + 1
		for i in range(12):
			self.f.therm(battery=None if i % 2 else hot)	# hot, unknown, hot, ...
			self.d.tick(i * 5)
		self.assertEqual(self.offs, [])
		self.f.put(T.THERM + '/battery', 2000)		# off the scale: not a reading
		for i in range(5):
			self.f.therm()
			self.d.tick(100 + i)
		self.assertEqual(self.offs, [])

	def test_no_power_off_under_threshold(self):
		for i in range(10):
			self.f.therm(battery=T.SHUTDOWN_BATTERY)	# at it is not above it
			self.d.tick(i)
		self.assertEqual(self.offs, [])

	def test_dry_run_records_aside(self):
		T.DRY_RUN = True
		d = T.Thermald()
		for i in range(T.CONFIRM):
			self.f.therm(battery=T.SHUTDOWN_BATTERY + 50)
			d.tick(i)
		self.assertTrue(any('DRY RUN: would power off' in m for m in LOG))
		self.assertFalse(os.path.exists(self.f.root + T.STATE + '/shutdown'))
		self.assertIn('the battery was at', self.f.get(T.STATE + '/shutdown.dry-run'))

	def test_reason_shown_at_next_start(self):
		self.f.shell()
		self.f.put(T.STATE + '/shutdown', '2026-11-12 05:20:00: the battery was at 68.5 C')
		T.Thermald()
		lines = self.f.lines()
		self.assertTrue(any(l.startswith('notekey zrn-thermald-shutdown|-|-1||System|The phone was switched off')
				    and '68.5 C' in l for l in lines), lines)
		self.assertFalse(os.path.exists(self.f.root + T.STATE + '/shutdown'))
		self.assertIn('68.5 C', self.f.get(T.STATE + '/shutdown.last'))

	def test_reason_waits_for_the_shell(self):
		self.f.put(T.STATE + '/shutdown', 'x: the battery was at 68.5 C')
		d = T.Thermald()			# no shell yet
		self.f.shell()
		d.tick(0)
		self.assertTrue(any('zrn-thermald-shutdown' in l for l in self.f.lines()))

	def test_caps_beside_zrnperfd(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		little = T.CPUFREQ + '/policy0/scaling_max_freq'
		self.f.put(big, 1794000)			# zrnperfd's rung
		self.f.therm(ap=M('ap')[1])		# level 2
		self.d.tick(0)
		self.assertEqual(self.d.level, 2)
		self.assertEqual(self.f.get(big), '1469000')
		self.assertEqual(self.f.get(little), '1499333')
		self.assertEqual(self.f.get(T.TZ + '/cooling_device0/cur_state'), '1')
		self.assertEqual(self.f.get(T.GPU_DEVFREQ + '/device/power/control'), 'auto')	# put back
		# zrnperfd climbs a rung: clamped again, its wish kept
		self.f.put(big, 2704000)
		self.d.caps()
		self.assertEqual(self.f.get(big), '1469000')
		self.assertEqual(self.d.cpus[1].wish, 2704000)
		# zrnperfd goes under the ceiling: left alone
		self.f.put(big, 1066000)
		self.d.caps()
		self.assertEqual(self.f.get(big), '1066000')
		# zrnperfd back up, then the phone cools: its wish comes back
		self.f.put(big, 2327000)
		self.d.caps()
		self.assertEqual(self.f.get(big), '1469000')
		self.f.therm(ap=300)
		self.d.tick(5)
		self.assertEqual(self.d.level, 0)
		self.assertEqual(self.f.get(big), '2327000')
		self.assertEqual(self.f.get(little), '1794000')
		self.assertEqual(self.f.get(T.TZ + '/cooling_device0/cur_state'), '0')

	def test_caps_step_down_with_hysteresis(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.f.therm(ap=M('ap')[3])
		self.d.tick(0)
		self.assertEqual((self.d.level, self.f.get(big)), (4, '400000'))
		self.assertEqual(self.f.get(T.TZ + '/cooling_device0/cur_state'), '2')
		self.f.therm(ap=M('ap')[3] - H('ap') + 1)
		self.d.tick(5)
		self.assertEqual(self.d.level, 4)
		self.f.therm(ap=M('ap')[3] - H('ap') - 1)
		self.d.tick(10)
		self.assertEqual((self.d.level, self.f.get(big)), (3, '1066000'))

	def perfd(self, cap='0,0', reader=True):
		"""A zrnperfd that knows thermal-cap: its status, and its pipe with a reader."""
		self.f.put('/run/zrnperfd.status', 'profile=balanced\nthermal_cap=%s' % cap)
		path = self.f.root + T.PERFD_CTL
		if not os.path.exists(path):
			os.mkfifo(path)
		if reader:
			self.ctl = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
			self.addCleanup(os.close, self.ctl)

	def sent(self):
		try:
			return os.read(self.ctl, 4096).decode().splitlines()
		except BlockingIOError:
			return []

	def test_caps_through_zrnperfd(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.perfd()
		self.f.therm(ap=M('ap')[1])			# level 2
		self.d.tick(0)
		self.assertEqual(self.sent(), ['thermal-cap 1499333 1469000'])
		self.assertTrue(self.d.via_perfd)
		self.assertEqual(self.f.get(big), '2704000')	# not written here
		self.assertEqual(self.f.get(T.TZ + '/cooling_device0/cur_state'), '1')	# the GPU still is
		self.d.tick(5)
		self.assertEqual(self.sent(), ['thermal-cap 1499333 1469000'])	# not taken yet: again
		self.f.put('/run/zrnperfd.status', 'thermal_cap=1499333,1469000')
		self.d.tick(10)
		self.assertEqual(self.sent(), [])			# taken: quiet
		self.f.therm(ap=300)
		self.d.tick(15)
		self.assertEqual(self.sent(), ['thermal-cap off'])
		self.assertIn('cpu_caps=zrnperfd', self.f.get(T.STATUS))

	def test_zrnperfd_without_the_command(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.f.put('/run/zrnperfd.status', 'profile=balanced')	# an older zrnperfd
		self.f.therm(ap=M('ap')[1])
		self.d.tick(0)
		self.assertFalse(self.d.via_perfd)
		self.assertEqual(self.f.get(big), '1469000')	# clamped here

	def test_zrnperfd_gone(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.perfd(reader=False)			# its status left behind, nobody reading
		self.f.therm(ap=M('ap')[1])
		self.d.tick(0)
		self.assertFalse(self.d.via_perfd)
		self.assertEqual(self.f.get(big), '1469000')

	def test_zrnperfd_comes_back(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.f.therm(ap=M('ap')[1])
		self.d.tick(0)
		self.assertEqual(self.f.get(big), '1469000')	# clamped here meanwhile
		self.perfd()
		self.d.tick(5)
		self.assertTrue(self.d.via_perfd)
		self.assertEqual(self.sent(), ['thermal-cap 1499333 1469000'])
		self.assertEqual(self.f.get(big), '1469000')	# ours stays until zrnperfd has it
		self.f.put('/run/zrnperfd.status', 'thermal_cap=1499333,1469000')
		self.f.put(big, '1469000')			# zrnperfd's own write, capped
		self.d.tick(10)
		self.assertIsNone(self.d.cpus[1].ours)		# forgotten, not written back
		self.assertEqual(self.f.get(big), '1469000')

	def test_release_on_stop(self):
		big = T.CPUFREQ + '/policy4/scaling_max_freq'
		self.f.therm(ap=M('ap')[3])
		self.d.tick(0)
		self.d.release()
		self.assertEqual(self.f.get(big), '2704000')
		self.assertEqual(self.f.get(T.TZ + '/cooling_device0/cur_state'), '0')

	def test_warning_notification(self):
		self.f.shell()
		self.f.therm(battery=T.WARN['battery'])
		self.d.tick(0)
		lines = self.f.lines()
		self.assertEqual(len(lines), 1, lines)
		self.assertTrue(lines[0].startswith('notekey zrn-thermald|-|-2||System|Phone is hot|The battery at 50.0 C'),
				lines[0])
		self.f.therm(battery=T.WARN['battery'])
		self.d.tick(5)
		self.assertEqual(self.f.lines(), [])		# said once
		self.f.therm(battery=T.WARN['battery'] + 10)
		self.d.tick(10)
		self.assertTrue(self.f.lines()[0].startswith('notekey zrn-thermald|q|'))	# an update, quiet
		self.f.therm(battery=T.WARN['battery'] - T.WARN_HYST - 1)
		self.d.tick(15)
		self.assertEqual(self.f.lines(), ['noteclose zrn-thermald'])

	def test_shell_restart_gets_it_again(self):
		self.f.shell()
		self.f.therm(ap=T.WARN['ap'])
		self.d.tick(0)
		self.assertEqual(len(self.f.lines()), 1)
		os.close(self.f.pipe)
		self.f.pipe = None
		self.f.shell()					# a new pipe: the shell restarted
		self.d.tick(5)
		lines = self.f.lines()
		self.assertTrue(any(l.startswith('notekey zrn-thermald|q|') for l in lines), lines)

	def test_unknown_warns(self):
		self.f.shell()
		for i, t in enumerate((0, 5, 10, 15, 20, 25, 30)):
			self.f.therm(battery=None)
			self.d.tick(t)
		lines = self.f.lines()
		self.assertEqual(sum('zrn-thermald-unknown' in l for l in lines), 1, lines)
		self.assertEqual(self.offs, [])
		self.f.therm(battery=300)
		self.d.tick(35)
		self.assertEqual(self.f.lines(), ['noteclose zrn-thermald-unknown'])

	def test_one_unknown_after_sleep_is_quiet(self):
		self.f.shell()
		self.f.therm(battery=None)
		self.d.tick(0)
		self.f.therm(battery=300)
		self.d.tick(1)
		self.assertEqual(self.f.lines(), [])

	def test_no_module(self):
		shutil.rmtree(self.f.root + T.THERM)
		for t in range(0, 60, 5):
			self.d.tick(t)
		self.assertEqual(self.offs, [])
		self.assertTrue(self.d.unknown_shown)
		self.assertEqual(self.d.period_ms, T.DEFAULT_PERIOD_MS)

	def test_fake_mode_is_dry(self):
		fake = self.f.root + '/fake'
		with open(fake, 'w') as f:
			f.write('battery %d\nap unknown\n' % (T.SHUTDOWN_BATTERY + 10))
		d = T.Thermald(fake=fake)
		for i in range(T.CONFIRM):
			d.tick(i)
		self.assertTrue(any('DRY RUN' in m for m in LOG))
		self.assertTrue(os.path.exists(self.f.root + T.STATE + '/shutdown.dry-run'))
		self.assertFalse(os.path.exists(self.f.root + T.STATE + '/shutdown'))
		self.assertIsNone(d.r['ap'])

	def test_status_file(self):
		self.f.therm(ap=M('ap')[0])
		self.d.tick(0)
		s = self.f.get(T.STATUS)
		self.assertIn('level=1', s)
		self.assertIn('ap=%d' % M('ap')[0], s)


if __name__ == '__main__':
	unittest.main(verbosity=1)
