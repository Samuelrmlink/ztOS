# battery + data: the power budget accountant

Android's "battery and data usage per app", for ztOS on the Galaxy S9.

| piece | what |
| --- | --- |
| `ztacctd` (root, `ztacctd.service`) | collects: processes into apps, CPU, data, energy; keeps the windows |
| `ztacct` | the command: `ztacct top`, `ztacct app Brave`, `ztacct proc PID`, `--json` |
| `ztusage` ("battery + data") | the app: apps by energy, data, CPU or background CPU; an app's page; a process's page |

## Apps: which processes belong together

First match wins (`classify()` in `ztacctd.c`):

1. kernel threads: **Kernel** (with interrupt time)
2. inside Waydroid's container (`lxc.payload.waydroid`): an Android app (uid
   ≥ 10000) by its package (`cmdline`, `com.foo:service` is `com.foo`), named
   by its `waydroid.PKG.desktop`; the rest **Android system**
3. a cgroup made for one app: zrnperfd's tickle (`zrn-tickle-PID`) or a
   systemd scope `app-ID.scope` (if the shell ever launches apps that way)
4. `system.slice/UNIT`: **System: UNIT**; the user's own services
   (`user@1000.service/…/UNIT`): **Service: UNIT**
5. the shell's login session: the process tree. Every child of `ztmetro` is
   an app's first process, and all its descendants (Brave's zygote, renderers,
   GPU and network processes) belong to it. The app is named by the desktop
   file whose `Exec` (or file id, icon, `StartupWMClass`) matches the
   program. SDK apps are forked by `ztos-zygote`, not the shell: each goes with
   the launcher (the shell's child) that started last before it. Unnamed
   helpers of the shell (`zt*`, `zrn*`, bwrap image loaders) and `ztmetro`
   itself: **ztMETRO**
6. other login sessions: **Login: ssh**, **Login: sudo**

This needs no change to `ztmetro.c`: apps are the shell's children already.
cgroups per app would be better still (cgroup v2 `cpu.stat` counts exited
children exactly, and the tree would survive reparenting), and are picked up
by rule 3 the day the shell launches into `app-ID.scope`.

The phone's own costs are groups too: **Idle (awake)**, **Deep sleep**,
**Wi-Fi idle**, **Mobile radio idle**, **Unaccounted** (what the gauge
measured beyond the model), **Short-lived processes** (born and gone between
scans with no known parent), **Network: not attributed**.

## CPU time, also of processes that have exited

`/proc/PID/stat` utime + stime at each scan (every 15 s with the screen on,
60 s off; a `CLOCK_MONOTONIC` timer, which stops while the SoC sleeps: the
daemon never wakes it). For what ran between the last scan and the exit, and
for processes that live and die between two scans: taskstats exit records
(`CONFIG_TASKSTATS`, on): the kernel sends every exiting thread's own CPU time
(and tgid, parent, name) over generic netlink. A dead process's last interval
is the sum of its threads' records since the scan; a process never seen goes
to its parent's app. The process events connector (`CONFIG_PROC_EVENTS`) is
not in the kernel; taskstats covers what it would have given.

With the screen off the exit records are not polled: they wait in the socket
(4 MB) for the next scan, so an exiting process costs no wake. Overruns are
counted in `ztacct status`.

Apps keep their tallies when their processes are gone; each app also keeps
its exited processes by name (up to 10, count and totals).

## Data per process

Linux has no byte counter per process. `acct-bpf.h` attaches three eBPF
programs to the root cgroup (`CONFIG_BPF_SYSCALL`, `CONFIG_CGROUP_BPF`, on;
no kprobes or BTF needed, written as raw instructions, held by BPF links so
they leave with the daemon):

- `cgroup/sock` (socket created): socket cookie → the process making it
- `cgroup/skb` ingress and egress: (cookie, interface, direction) → bytes, packets

Each scan gives the growth since the last to the socket's process and app;
Wi-Fi (`wl*`), mobile (`rmnet*`, `ccmni*`, `wwan*`) and other (USB, tunnels)
apart; loopback is not counted. Waydroid's sockets see only the container's
interface and go to the network of the default route.

Accuracy: these are IP packets as the sockets see them (headers included,
link layer not), counted at the socket: retransmissions count, packets
dropped before a socket do not, forwarded traffic (no socket) is not
anyone's. Sockets made before ztacctd started are found once by
`pidfd_getfd` + `SO_COOKIE` and go to the process holding them. A socket
handed to another process stays its maker's. `ztacct status` puts the
interfaces' own counters beside the attributed totals; on the phone they
agree to a few percent (link-layer headers, ARP and DHCP frames).

Traffic through a tunnel (Tailscale) is counted twice in the totals: once
for the app, on the tunnel ("other"), and once for `tailscaled`, on Wi-Fi.
That is how Android shows VPNs too.

## Energy

Android's `power_profile` approach on this phone's counters, every scan:

- **CPU**: per cluster (A55 ×4, M3 ×4), busy core seconds (`/proc/stat`) at
  the frequencies `cpufreq/stats/time_in_state` shows, weighted by frequency;
  a process gets its CPU seconds × the joules per busy second of the cluster
  it was last on; interrupts go to Kernel. Hot-plugged cores are skipped
  for the scan they come back.
- **Screen**: while on, `screen_mw + screen_full_mw × backlight`, to the app
  in front (zrnperfd's `front=` line; at Start, or without it, ztMETRO).
- **GPU**: devfreq's time above its lowest clock × power at that clock, to
  the app in front. (Panfrost's per-process engine time needs its
  `profiling` switch, off by default.)
- **Wi-Fi**: per packet + per byte, to the socket's app; associated and idle
  (17 mA screen off with zrnperfd's tuning, 82 mA untuned with the screen
  on, power/README.md): Wi-Fi idle. **Mobile**: active time per packet and a
  tail, by bytes; up and idle: Mobile radio idle.
- **Base**: awake and idle (this kernel has no cpuidle states: 325 mA with the
  screen off, dark, radio off) and deep sleep (`CLOCK_BOOTTIME −
  CLOCK_MONOTONIC`), plus each resume.

Every 5 minutes the modelled joules are checked against the gauge's coulomb
counter (`max170xx_battery/charge_counter`, µAh × `voltage_now`). On battery,
a model above the measurement is scaled down to it; below, the rest is
**Unaccounted**. On the charger the gauge cannot see the phone's own use and
the model stands. Every checked window is appended to
`/var/lib/ztacct/calib.csv` with the model's inputs, to fit the coefficients
(`/etc/ztacct/power_profile`, `KEY VALUE` lines: `base_awake_mw`,
`screen_mw`, `cluster1.khz`/`cluster1.mw`, `cpu_scale`, …): `fit-calib.py
calib.csv` fits them by least squares. `bench-cpu.sh` (overnight, `ztbench`)
measures the CPU tables with busy loops, screen off.

## Windows and the state file

Since the last full charge (unplugged after ≥ 95 % or Full), per day (7 days),
the last 24 h (hourly). `/var/lib/ztacct/state` (binary, ~150 KB) is written
every 20 minutes, when the screen goes off (at most every 5 min), on `ztacct
save` and at stop. Within one boot it also keeps each live process's
baseline, so a restart of the daemon loses nothing. 320 apps at most; the
least used with nothing running makes room.

## Interfaces

`/run/ztacct/sock` (mode 0660, the phone user's group; `SO_PEERCRED`: root or
that user). One line in, JSON out (see `acct-common.h`):

	top [WINDOW [energy|cpu|data|bg]]
	app NAME[@WINDOW]
	proc PID
	status
	save

`/run/ztacct/status`: a few `key=value` lines for anyone.

An app's answer carries its processes, its exited processes, 24 hourly
values, and the union of its processes' open files (`/proc/PID/fd`, read by
the root daemon), de-duplicated, each with how many and which processes hold
it, in sections: files, sockets (with addresses and state from `/proc/net`),
pipes, anonymous inodes, devices.

## Its own cost

`ztacct status` reports the daemon's CPU time and wakes.

## The app

`ztusage` ("battery + data" in the app list, `/usr/local/share/applications/ztusage.desktop`):
four tabs (battery, data, cpu, background) sort the apps; "Showing" cycles
the window (since the charge, today, last 24 h, yesterday, the day before).
Tapping an app opens its processes under it, each with its share; "details"
(or holding the app) opens its page: totals, where the energy went, 24
hourly bars, processes now and exited, open files. Tapping a process opens
its page: memory, CPU, data, I/O, what it is, threads, open files.

`ztusage --render battery|data|cpu|background|open:APP|app:APP|proc:PID FILE.png`
draws a page with no display (`ZTUSAGE_RENDER_H` points tall).
