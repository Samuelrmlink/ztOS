#!/usr/bin/env python3
"""alarm-timerfd.py SECONDS [LOG]: arm a CLOCK_REALTIME_ALARM timerfd SECONDS
from now (as the Clock app's alarm service does), wait for it, and say how
late it fired. Needs CAP_WAKE_ALARM (run as root). The line goes to stdout
and, if given, is appended to LOG, so a run across a SoC sleep can be read
afterwards:

	sudo setsid -f python3 alarm-timerfd.py 150 /var/tmp/alarm-test.log

An alarmtimer can only wake the SoC through a wake-capable RTC
(/sys/class/rtc/rtc0, zrn-exynos-rtc): without one, timerfd_create still
works but the alarm fires only at the next wake.
"""
import ctypes, os, struct, sys, time

CLOCK_REALTIME_ALARM = 8
TFD_TIMER_ABSTIME = 1


class timespec(ctypes.Structure):
	_fields_ = [('tv_sec', ctypes.c_long), ('tv_nsec', ctypes.c_long)]


class itimerspec(ctypes.Structure):
	_fields_ = [('it_interval', timespec), ('it_value', timespec)]


def main():
	seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 120
	logfile = sys.argv[2] if len(sys.argv) > 2 else None
	libc = ctypes.CDLL(None, use_errno=True)
	fd = libc.timerfd_create(CLOCK_REALTIME_ALARM, os.O_CLOEXEC)
	if fd < 0:
		e = ctypes.get_errno()
		sys.exit(f'timerfd_create(CLOCK_REALTIME_ALARM): {os.strerror(e)}')
	due = time.time() + seconds
	spec = itimerspec()
	spec.it_value.tv_sec = int(due)
	spec.it_value.tv_nsec = int((due % 1) * 1e9)
	if libc.timerfd_settime(fd, TFD_TIMER_ABSTIME, ctypes.byref(spec), None) < 0:
		e = ctypes.get_errno()
		sys.exit(f'timerfd_settime: {os.strerror(e)}')
	boot0 = time.clock_gettime(time.CLOCK_BOOTTIME)
	mono0 = time.monotonic()
	armed = time.strftime('%H:%M:%S', time.localtime(due))
	start = f'{time.strftime("%Y-%m-%d %H:%M:%S")} armed CLOCK_REALTIME_ALARM for {armed} (+{seconds:.0f}s)'
	print(start, flush=True)
	if logfile:
		with open(logfile, 'a') as f:
			f.write(start + '\n')
	expirations = struct.unpack('Q', os.read(fd, 8))[0]
	now = time.time()
	asleep = (time.clock_gettime(time.CLOCK_BOOTTIME) - boot0) - (time.monotonic() - mono0)
	line = (f'{time.strftime("%Y-%m-%d %H:%M:%S")} fired: due {armed}, late {now - due:+.3f}s, '
		f'expirations {expirations}, suspended {asleep:.1f}s while waiting')
	print(line, flush=True)
	if logfile:
		with open(logfile, 'a') as f:
			f.write(line + '\n')


if __name__ == '__main__':
	main()
