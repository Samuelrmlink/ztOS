#!/usr/bin/env python3
"""soc-report.py SOC.CSV [--since 'YYYY-mm-dd HH:MM'] [--png OUT.png]

A night from zrn-sleepd's /var/lib/zrn-sleep/soc.csv: the drain (mAh from
charge_now, % from capacity) and the mean current over the whole span, the
time asleep and awake, wakes by purpose, Wi-Fi reassociation times, and an
hourly table. With --png (and matplotlib) a plot of SOC and charge.

Pull it from the phone first:
	scripts/.../p 'cat /var/lib/zrn-sleep/soc.csv' > soc.csv
"""
import argparse, collections, csv, re, sys, time


def num(value):
	try:
		return float(value)
	except (TypeError, ValueError):
		return None


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('csv')
	ap.add_argument('--since')
	ap.add_argument('--png')
	args = ap.parse_args()
	rows = []
	with open(args.csv, newline='') as f:
		for row in csv.DictReader(f):
			if row.get('time') == 'time':	# a header after a rotation
				continue
			if args.since and row['time'] < args.since:
				continue
			row['t'] = num(row['epoch'])
			row['mah'] = num(row['charge_now_mah'])
			row['pct'] = num(row['soc'])
			if row['t'] is not None:
				rows.append(row)
	if len(rows) < 2:
		sys.exit('fewer than two rows')
	# a reboot restarts boot_s: report each run of rows on its own boot too
	first, last = rows[0], rows[-1]
	span_h = (last['t'] - first['t']) / 3600
	print(f'{first["time"]} -> {last["time"]}: {span_h:.2f} h, {len(rows)} rows')
	if first['mah'] is not None and last['mah'] is not None and span_h > 0:
		used = first['mah'] - last['mah']
		print(f'charge {first["mah"]:.0f} -> {last["mah"]:.0f} mAh: {used:.0f} mAh, mean {used / span_h:.1f} mA')
	print(f'soc {first["soc"]}% -> {last["soc"]}%')
	asleep = sum(num(r['asleep_s']) or 0 for r in rows if r['event'] == 'wake')
	print(f'asleep {asleep / 3600:.2f} h ({100 * asleep / 3600 / max(span_h, 1e-9):.0f}%)')
	purposes = collections.Counter(r['reason'] for r in rows if r['event'] == 'wake')
	print('wakes:', ', '.join(f'{k} {v}' for k, v in purposes.most_common()) or 'none')
	reassoc = [num(m.group(1)) for r in rows if r['event'] == 'wifi'
		   for m in [re.search(r'reassociated ([\d.]+)s', r['note'])] if m]
	timeouts = sum(1 for r in rows if r['event'] == 'wifi' and r['reason'] == 'timeout')
	if reassoc or timeouts:
		reassoc.sort()
		mid = reassoc[len(reassoc) // 2] if reassoc else float('nan')
		print(f'wifi back {len(reassoc)} times, median {mid:.1f}s, max {max(reassoc or [0]):.1f}s; '
		      f'{timeouts} not back in the window')
	# asleep rows: the charge across each sleep alone
	sleep_mah = sleep_s = 0.0
	prev = None
	for r in rows:
		if r['event'] == 'sleep':
			prev = r
		elif r['event'] == 'wake' and prev and prev['mah'] is not None and r['mah'] is not None:
			sleep_mah += prev['mah'] - r['mah']
			sleep_s += r['t'] - prev['t']
			prev = None
	if sleep_s > 600:
		print(f'across sleeps alone: {sleep_mah:.0f} mAh in {sleep_s / 3600:.2f} h, '
		      f'{sleep_mah / (sleep_s / 3600):.1f} mA (the gauge counts in ~0.5 mAh steps)')
	print('\nhour              soc   mAh  mean mA  wakes')
	by_hour = collections.OrderedDict()
	for r in rows:
		by_hour.setdefault(r['time'][:13], []).append(r)
	hours = list(by_hour.items())
	for i, (hour, hr) in enumerate(hours):
		end = hours[i + 1][1][0] if i + 1 < len(hours) else hr[-1]
		dt = (end['t'] - hr[0]['t']) / 3600
		mah = (hr[0]['mah'] - end['mah']) if hr[0]['mah'] is not None and end['mah'] is not None else None
		mean = f'{mah / dt:7.1f}' if mah is not None and dt > 0.05 else '      -'
		wakes = sum(1 for r in hr if r['event'] == 'wake')
		print(f'{hour}:00  {hr[0]["soc"]:>3}  {mah if mah is not None else 0:5.0f}  {mean}  {wakes:5d}')
	if args.png:
		try:
			import matplotlib
			matplotlib.use('Agg')
			import matplotlib.pyplot as plt
		except ImportError:
			sys.exit('no matplotlib: the table above is all')
		ts = [time.localtime(r['t']) for r in rows]
		x = [(r['t'] - first['t']) / 3600 for r in rows]
		fig, ax = plt.subplots(figsize=(10, 4))
		ax.plot(x, [r['pct'] for r in rows], label='SOC %')
		ax.set_xlabel(f'hours since {time.strftime("%H:%M", ts[0])}')
		ax.set_ylabel('%')
		ax2 = ax.twinx()
		ax2.plot(x, [r['mah'] for r in rows], color='tab:orange', label='charge_now mAh')
		ax2.set_ylabel('mAh')
		fig.legend(loc='upper right')
		fig.tight_layout()
		fig.savefig(args.png, dpi=120)
		print('plot:', args.png)


if __name__ == '__main__':
	main()
