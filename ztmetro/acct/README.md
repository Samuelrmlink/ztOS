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

The phone's own costs are groups too: **Idle (awake)**, **Always-on
(unseparated)**, **Deep sleep**, **Wi-Fi idle**, **Mobile radio idle**,
**Mobile radio (airplane)**, **Bluetooth**, **USB-C accessories**, **Desktop
(external display)**, **Unaccounted** (what the gauge measured beyond the
model), **Short-lived processes** (born and gone between scans with no known
parent), **Network: not attributed**.

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
That is how Android shows VPNs too. With Tailscale in userspace mode (no
tunnel interface, as here) an ssh over Tailscale is `tailscaled`'s Wi-Fi
traffic only: sshd sees it on loopback, which is not counted.

Sockets the kernel makes itself have no maker on record: a TCP connection
accepted on a listening socket (an ssh or rsync over the LAN or USB) is a
clone made in the kernel, and the `cgroup/sock` hook only sees `socket()`.
When such a socket moves bytes, the kernel's socket table (`sock_diag`, one
netlink dump in that scan, no `/proc` walk) gives its local port, and the
listening socket on that port has a maker: the connection goes to it (sshd's
are `System: sshd`). A socket still not placed keeps its bytes waiting for
the next rescan (up to 12 minutes) instead of giving them to nobody.
**Network: not attributed** is what is left: `ztacct status` says why
(`unattributed`: never placed, or a maker that is no longer known, and on
which network), what `sock_diag` placed, what is waiting, and the largest
sockets not placed with what `sock_diag` knew of them when they first moved
bytes (`unplaced_top`: TCP or UDP, ports, state; `gone` if they had already
left its table). Waydroid's
apps are not in it: their sockets are made in the container by its
processes and go to their packages; Waydroid's NAT (forwarded, no socket) is
not counted at all, as the interfaces' own counters show.

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
  CLOCK_MONOTONIC`), plus each resume. Bluetooth up and the modem's CP
  online in airplane mode were on when the base was measured: their share
  of it (estimates, 10 and 25 mW) is shown as **Bluetooth** and **Mobile
  radio (airplane)**, moved out of Idle, not added; each Bluetooth link
  (`/sys/class/bluetooth/hci0:N`) adds 20 mW (an estimate).
- **Always-on (unseparated)**: what the gauge shows, window after window,
  beyond the measured base when nothing we read changes: 480 mW, fitted
  (below). The gauge is not the cause: its coulomb counter and `current_now`
  agree to 1 % minute by minute (2026-11-11). It is real draw of the phone as
  used day to day that the 150 s bench (Wi-Fi off, nothing running but the
  test) did not have, and that no state we read tells apart. What it may be,
  and what would settle it: "What is left", below.
- **USB-C accessories** (host / OTG power): while the MAX77705's boost gives
  VBUS to a sink partner (its regulator `max77705-otg` reads `enabled`;
  without it, `typec/port0/power_role` `[source]`), the boost (90 mW) and,
  with devices on the bus (`/sys/bus/usb/devices`, `1-1`, `1-1.4`, …), the
  bus (470 mW: xHCI, the PHY, the dock's hub) and each device (140 mW). The
  MAX77705 has no OTG current readout (its power supplies show only the
  input side), so these are fitted. The Belkin 6-in-1 dock (two hubs, an
  RTL8153 Ethernet, a card reader, a billboard device, an HDMI converter, a
  keyboard/mouse dongle) is **1.5–1.6 W** on the battery: the gauge stepped
  from 3.55 W to 1.95 W, screen off, when it was pulled (2026-11-10 23:48
  CST). A dock that powers the phone (PD in) costs the battery nothing: the
  boost is off. *This is what was Unaccounted on the desktop, not XFCE*:
  XFCE's own processes are counted by their CPU under their `Login:` groups
  (xfdesktop 0.11 s of CPU a minute, screen off).
- **Desktop (external display)**: the DisplayPort link up, the combo PHY's
  DP part, the PHY and the link controller (220 mW, fitted), and video out
  (DECON2 scanout and the link's video at 1080p60: 300 mW, an estimate;
  never on long enough on battery to fit). The link being up is only in the
  driver's debugfs `state` (root): `id N pma N phy N ctrl N`, up if any of
  pma, phy, ctrl is 1 (its "link powered and clocked" is the power domain,
  on at every boot). It is read at a settle (every 5 minutes) and only
  while a USB-C partner is there or the last read found it up: a read with
  the link's power domain off logs a warning. Video out is the DRM
  connector `card*-DP-*/enabled`. The driver leaves the link up after the
  dock is pulled (`x9dp_off` is not called on detach), until a reboot: 220 mW
  a day long with no display (seen 2026-11-11).

Every 5 minutes the modelled joules are checked against the gauge's coulomb
counter (`max170xx_battery/charge_counter`, µAh × `voltage_now`). On battery,
a model above the measurement is scaled down to it; below, the rest is
**Unaccounted**. On the charger the gauge cannot see the phone's own use and
the model stands. `ztacct top` and the app say what share of the measured
energy the model explains (all but Unaccounted).

Every checked window is appended to `/var/lib/ztacct/calib.csv` with the
model's inputs, to fit the coefficients (`/etc/ztacct/power_profile`, `KEY
VALUE` lines: `base_awake_mw`, `base_extra_mw`, `screen_mw`,
`cluster1.khz`/`cluster1.mw`, `cpu_scale`, `gpu_scale`, `otg_mw`,
`usb_host_mw`, `usb_dev_mw`, `dp_link_mw`, `dp_video_mw`, `bt_on_mw`,
`bt_conn_mw`, `cp_air_mw`, …): `fit-calib.py calib.csv` fits them by least
squares. CPU and GPU joules are written at scale 1. Since 2026-11-11 each
row also has the accessories' and radios' states, in seconds:
`otg_s` (VBUS out), `usb_host_s`, `usb_dev_s` and `usb_active_s` (device
seconds: devices × seconds, all and not runtime-suspended), `dp_link_s`,
`dp_video_s`, `bt_on_s`, `bt_conn_s` (link seconds), `cp_s` (modem online),
`airplane_s`, `waydroid_s` (the container running). The first save of the
new daemon rewrites an older calib.csv once with the new header, its rows
given empty fields: unknown, not 0.

Older rows get their accessory states from the kernel's log:
`fit-calib.py calib.csv --journal KLOG`, with `journalctl _TRANSPORT=kernel
-o short-unix` (all boots; the journal is persistent) as KLOG. It rebuilds
`otg_s` from max77705's `OTG: boost on` / `boost off`, the USB devices from
`usb 1-1…: new … USB device` / `USB disconnect` (and `usb1-port1: unable to
enumerate`: a dock that never came up), the bus going with usbdrd's `device
mode`, the DP link from exynos9810-dp's `pma: on` (then on until the next
boot) and video from its `drm: enable` / `disable`; a boot (`Linux
version`) ends everything. Bluetooth was never logged usably: rows without
it do not fit it. `--report` says how much each model leaves Unaccounted
and where the energy goes by group; `--holdout-after EPOCH` fits on the
rows before and reports on the rows after.

The profile of 2026-11-11 (`fit-calib.py calib.csv --journal KLOG --fix
base_suspend_mw=100 --fix dp_video_mw=300 --fix bt_on_mw=0 --fix
bt_conn_mw=0 --report`, 109 windows of 11-08 to 11-11, deep sleep off
throughout): Unaccounted was 33.0 % of the measured energy with the old
model and is 1.9 % with the new; fitted on the 90 windows before 11-11 and
tested on the 19 after (the dock on, then pulled), the old model left
48.9 % Unaccounted and the new one predicts the gauge to +2.8 %. Where the
measured energy went over all 109 windows: Idle (awake) 42 %, USB-C
accessories 19 %, Always-on (unseparated) 17 %, CPU 9 %, Wi-Fi 5 %, Desktop
(external display) 4 %, screen 2.5 %, Unaccounted 2 %.

`bench-cpu.sh` (overnight, `ztbench`) measures the CPU tables with busy
loops, screen off.

### What is left

The **Always-on (unseparated)** 480 mW: draw in daily use beyond the 325 mA
bench, with nothing we read changing. Candidates, and what would settle each
(all with the charger out, the screen off, five-minute windows):

- The sensor hub and its daemons (28 mA in October's A/B: 100 mW), the
  audio codec and PipeWire held open, Waydroid's container (its CPU is
  counted, but not what its wakeups keep awake), BLE scanning by
  ztos-podsd: stop one at a time (the container: `waydroid container
  stop`) and read the window's gauge.
- The modem's CP: online in airplane mode, `mif` traffic every ~20 s; the
  base had it too, but not necessarily as busy. A window with the modem
  driver unloaded would say.
- The panel not asleep while "off" (157 mA in October) on some screen-offs:
  `s9p_decon panel_sleep` should read `Y`; ztacctd could record it.
- The DP link (above) is separated since it is read; were the driver to
  power it down on unplug, its 220 mW would show as gone from the gauge, and
  confirm the fit.

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
