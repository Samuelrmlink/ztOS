---
name: ztos-power-measure
description: Method for measuring power, CPU wakeups and sleep behaviour on the ztOS phone with the fuel gauge, soc.csv and zrn-sleepd, so results are comparable and honest. Use it when a change claims to save power or not block sleep.
---

# Measuring power on the phone

## Rules for a valid number
- **Off the charger.** Check `max77705-charger/online` = 0. On a charger the gauge measures the charge flow, not the phone, so only CPU and wakeup counts are valid. Say so, and leave the drain test for the owner.
- **One change at a time**, with an A/B toggle in the same session. Settle 60 s after each change, take at least 20 one-second readings of `current_now`, and also report `current_avg`. Note screen, AOD, Wi-Fi, Bluetooth and dock state.
- Take the phone lock (`ztlock run BRANCH ...`) for the whole measurement window, so another agent's install doesn't add noise.
- Report mA and mW (the gauge's voltage times current), with the number of samples and the spread. A single run is "one run", not a result.
- Put back everything you toggled.

## Tools on the phone and in the repo
| Need | Use |
|---|---|
| Battery current | `/sys/class/power_supply/max170xx_battery/current_now`, `current_avg` (mA, negative = discharge) |
| Per-wake log | `/var/lib/zrn-sleep/soc.csv` (time, soc, charge, current, asleep_s, reason, wifi, display, charger) |
| Sleep events | `sudo zrn-sleepd log N`, `sudo zrn-sleepd status` (`why=` says what keeps it awake) |
| Overnight report | `python3 power/suspend/soc-report.py soc.csv --since 'YYYY-MM-DD HH:MM' --png out.png` |
| A/B scripted cost | `scripts/dev/power-ab.sh` (runs detached on battery, logs `/var/tmp/power-ab.log`) |
| CPU per process | `scripts/dev/cpusample.sh`; `/proc/*/stat` deltas |
| Wakeups | `/proc/interrupts` deltas, `/sys/kernel/debug/wakeup_sources`, `/proc/timer_list`, `strace -c -f -p PID` for N s |
| Idle states | `/sys/devices/system/cpu/cpu*/cpuidle/state*/{usage,time}` |
| Model vs gauge | `ztacct` (ztacctd energy model; "Unaccounted" = gauge minus model) |

## Known reference points (2026-11-11)
- Awake, screen off: about 600 mA. Awake, everything possible off: about 350 mA. AOD on, so no sleep: about 440 mA.
- Deep sleep with a 30 s Wi-Fi window every 5 min: about 30 mA plus a floor that hasn't been measured.
- Uncore (bus and memory) dominates. Leaving one core awake doesn't save power.
