#!/usr/bin/env python3
"""wakeup-holders.py: which processes poll with EPOLLWAKEUP (each such epoll
item is a wakeup source, '[timerfd]', 'eventpoll', ...), what the polled fd
is, its clock if a timerfd, and whether the process is frozen. A source left
active by a process that does not read its event (frozen in the Waydroid
cgroup, say) blocks /sys/power/wakeup_count, and zrn-sleepd stalls. Root.
Also lists the wakeup sources active now."""
import os, re
from pathlib import Path

EPOLLWAKEUP = 1 << 29


def cgroup(pid):
	try:
		return Path(f'/proc/{pid}/cgroup').read_text().strip().split('::')[-1]
	except OSError:
		return '?'


def main():
	for w in sorted(Path('/sys/class/wakeup').iterdir()):
		try:
			if int((w / 'active_time_ms').read_text()) > 0:
				print(f'ACTIVE {(w / "name").read_text().strip()} for {(w / "active_time_ms").read_text().strip()} ms')
		except (OSError, ValueError):
			pass
	for proc in Path('/proc').iterdir():
		if not proc.name.isdigit():
			continue
		pid = proc.name
		try:
			fds = list((proc / 'fdinfo').iterdir())
			comm = (proc / 'comm').read_text().strip()
		except OSError:
			continue
		for info in fds:
			try:
				text = info.read_text()
			except OSError:
				continue
			for m in re.finditer(r'tfd:\s+(\d+)\s+events:\s+([0-9a-f]+)', text):
				if int(m.group(2), 16) & EPOLLWAKEUP:
					tfd = m.group(1)
					try:
						target = os.readlink(proc / 'fd' / tfd)
						clock = re.search(r'clockid:\s+(\d+)', (proc / 'fdinfo' / tfd).read_text())
					except OSError:
						target, clock = '?', None
					print(f'{pid} {comm} epoll {info.name} -> fd {tfd} {target}'
					      f'{" clock " + clock.group(1) if clock else ""} cgroup {cgroup(pid)}')


if __name__ == '__main__':
	main()
