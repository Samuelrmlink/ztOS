# The "Always-on" lump: what the 480 mW is (2026-11-11)

ztacctd's `base_extra_mw` (480 mW, "Always-on (unseparated)") is what the
gauge shows, window after window, above the measured base (1190 mW: 325 mA,
screen off, Wi-Fi off, panel asleep) and the modelled Wi-Fi idle. This
directory holds the A/B tools for taking it apart, and what is known so far.

## What the gauge says about it

From `/var/lib/ztacct/calib.csv` (124 windows, 11-08 to 11-11), the
residual of each window over the model without the floor, per awake second:

| windows | floor, mean | median |
| --- | --- | --- |
| screen off (58) | 516 mW | 514 mW |
| screen on (32) | 498 mW | 464 mW |

- **It does not follow the battery voltage**: screen off, 542 / 511 / 507 /
  530 mW at 3.5-3.75 / 3.75-3.9 / 3.9-4.0 / 4.0-4.15 V (slope 63 mW/V, under
  the noise). A constant-current load seen as constant power is not it.
- **It does not follow load or accessories**: as large overnight with nobody
  at the phone (11-10 04:23-06:39, ~520 mW with the dock; 11-11 07:26-07:47,
  550-586 mW without), as on a busy evening.
- **The bench did not have it.** The same evening as the 11-08 Wi-Fi bench
  (342 mA, screen off, tuned Wi-Fi, quiet phone under ztbench), the gauge's
  own windows show the floor (11-08 20:38-23:31, 500-600 mW). So it is a
  state of the phone in daily use that the 150 s bench did not reach, not a
  block that is always powered (those were in the bench too: see below).
- A spot check on 11-11 07:28, screen off and awake, other agents busy:
  546 mA by `current_now` (60 x 1 s), 527 mA by the coulomb counter over the
  same minute: ~2.1 W against the model's ~1.26 W.

## What is on that should not be (static inspection, 2026-11-11)

Reads only (sysfs, debugfs, /proc, CMU registers read through /dev/mem, the
fuel gauge's registers read with `i2cget -f`).

| what | evidence | fix |
| --- | --- | --- |
| No cpuidle at all | `cpuidle/current_driver` = none, no `cpu*/cpuidle`; zrn-cpuidle not loaded (ROADMAP 3.5) | kernel: rebuild zrn-cpuidle (1.4/3.5); SICD for the uncore |
| Memory at its top clock awake | PLL_MIF `0x1b800120` = `0xa19e0310`: M 414, P 3, S 0, 3588 MHz = MIF 1794 MHz, the boot level of the ASV table (421-1794); INT at its boot 534 MHz. Mainline has no MIF/INT devfreq: no pm_qos holder exists to release | ACPM DVFS request (`s9p-acpm dvfs mif 421000`, /dev/mem mailbox): **not approved**, not tried. An awake 421/107 MHz round trip was proven safe in October (power/suspend/README.md) |
| Audio DSP never idle | `snd-zrn-abox` ~1000-1170 interrupts/s with every PCM closed and both sinks SUSPENDED; `debug=8` shows DSP interrupt 1 (an SGI) every 1.0 ms. The block is powered from boot; Samsung powers it off 500 ms after the last stream | driver: power the block off when idle (the `recover_power_cycle` path already does the sequence) |
| Fuel-gauge interrupt ~2/s | `max170xx_battery` and its parent `max77705` ~1.9/s each; STATUS (0x00) reads 0 in 60 polls, alert windows bracket the values (V 3.1-5.1 V, T -128..127, SOC 81-83 % at RepSOC 82.7 %, MiscCFG source RepSOC). `14360000.i2c` 150-210 IRQ/s | kernel: find what asserts it (CONFIG 0x0314: Aen, ETHRM, Tex, Ten) |
| UFS never runtime-suspends | the LUs' `power/control` = on (block-layer default), no clock gating, no auto-hibern8 (`auto_hibern8`: not supported): the link stays in HS between commands. `rpm_lvl` 3 (sleep + hibern8) is ready | `power/control=auto` + `autosuspend_delay_ms` on the LUs: untested, a wedge here loses the root fs |
| USB controller up with no cable | `10c00000.usb` `power/control` = on (dwc3 forbids runtime PM at probe), `runtime_status` active in device mode, nothing attached | `power/control=auto`: a cable must still wake it; needs a plug test |
| Wakeups | screen off: arch_timer ~950/s, function-call IPIs 430-640/s, i2c-1 150/s, abox 1000/s; tasks (screen on, other agents busy): dbus-broker 270/s, zrnperfd 230/s, ztacctd 200/s, sugov:0 190/s, tailscaled ~200/s over its threads, ztmetro 70/s | per daemon, below |

Fine as they are: the GPU is runtime-suspended (panfrost, 260 MHz floor);
the panel sleeps (`panel_sleep=Y`); DECON 4.3 interrupts/s screen off; the
regulator framework sees only fixed/dummy rails (the S2MPS18 sits behind the
APM), and `clk_summary` shows stub clocks only (the bootloader's settings
stand; nothing for Linux to gate).

## Tools

On the phone in `/var/tmp/always-on/` (root, under `ztlock`):

- `meas LABEL SETTLE N [STEP]`: mean/median of `current_now` (N samples
  after SETTLE s), the coulomb counter's average over the same time, CPU
  busy %, screen state and the interrupt rates that matter. Refuses on the
  charger.
- `exp.sh CASE...`: screen off, zrn-sleepd inhibited, each case set,
  measured and undone (all undone on exit): `B` baseline, `sensors` (lhd and
  ztoscsf-sensord stopped), `daemons`, `apps`, `pipewire`, `tailscale`
  (SIGSTOP), `dwc3` (runtime PM allowed), `wifi` (radio off), `waydroid`
  (thawed: ztmetro freezes it with the screen off already).
- `launch.sh RUN CASE...`: `exp.sh` detached through systemd-run, log in
  `/var/tmp/always-on/RUN.log` (ssh can drop: `wifi` takes the link).
- `pcpu SECONDS`: per-thread CPU and context switches.
- `mifread.py`: the MIF PLL and dividers, read only.

## The A/B, 2026-11-11 08:43-09:19 (run2, kernel #62, on battery, screen off)

`current_now`, 90 x 1 s after 40 s settling per case; each case against
the mean of the baselines either side. The coulomb counter moves in ~40 mA
steps over 90 s and is too coarse here. The baselines themselves wander
467-536 mA (other agents' work on the phone, 14-31 % CPU busy): a
difference under ~25 mA is not a finding.

| case | B before / after | case | B - case | verdict |
| --- | --- | --- | --- | --- |
| Wi-Fi radio off | 535.6 / 532.5 | 458.0 | **76 mA (~300 mW)** | real: 2.4 GHz at -70 dBm, 3-9 frames/s, each holding the radio up 200 ms; the model had 65 mW (the 11-08 bench, 5 GHz, quiet) |
| tailscaled stopped | 508.9 / 507.3 | 483.6 | 25 mA | at the noise; CPU busy also fell 20 -> 13 % |
| daemons stopped (ztacctd, zrnperfd, audiod, bt-state, notifyd, pushd, ntfyd, ledd, corenetworkingd, iald) | 480.5 / 467.6 | 456.3 | 18 mA | noise; busy 17 -> 5 % (that CPU is modelled) |
| sensor hub (lhd, ztoscsf-sensord) | 496.8 / 480.5 | 473.1 | 16 mA | noise (October: 28 mA); inside the base anyway |
| pipewire, pipewire-pulse, wireplumber | 507.3 / 508.9 | 497.3 | 11 mA | noise; the DSP ticks on without them |
| apps (ztusage, ztpower, ztdesk, ztdeskmode, portal) | 467.6 / 507.3 | 534.9 | -47 mA | nothing: baseline drift |
| dwc3 `power/control=auto` | 507.3 / 535.6 | 492.5 | (29 mA) | nothing: it stayed `active` (device mode keeps it awake), so this is the noise level |

Rechecked: the "decon" rates in `meas` lines before this commit were wrong
(three interrupt lines share the name; the last, always 0, set the start
value, so the rate was the total count). Read directly, DECON gives 0
interrupts/s with the screen off, 15/s in the seconds after it goes off.

So of the 480 mW floor, **~235 mW is Wi-Fi** (300 measured against the 65
modelled). The profile now has `wifi_idle_off_mw 300`; refitted with it
(138 windows, accessories held), the floor is **365 mW**, Unaccounted
2.7 %. The floor does not leave with Wi-Fi: on 11-10 04:23-06:39 Wi-Fi was
not associated (Ethernet on the dock) and the windows still show it; and
with the radio off the phone drew 458 mA against the bench's 325.

## Still to measure (charger out, screen off, nobody else on the phone)

The 365 mW left is below this run's noise case by case. A quiet phone (no
agents, no ssh traffic but the run's own) and longer windows split it:

    SETTLE=60 N=300 sh /var/tmp/always-on/launch.sh run3 B wifi B tailscale B daemons B sensors B

Then the parts only a kernel change or approval can test: the audio DSP
powered off when idle, MIF at 421 MHz awake (ACPM), cpuidle (zrn-cpuidle),
the fuel gauge's interrupt, and UFS runtime PM.
