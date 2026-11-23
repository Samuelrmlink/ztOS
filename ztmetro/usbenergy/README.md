# USB accessory energy (ROADMAP 8.76)

What each USB accessory drew from the phone, in Wh, per session and in total,
kept after it is unplugged and across reboots.

| piece | what |
| --- | --- |
| `ztusbenergyd run` (`ztusbenergyd.service`, the user's) | keeps the record |
| `ztusbenergyd list` | the record as text |
| Settings > battery + performance > battery | a "USB accessories" section |
| USBView, a device's page | "Energy drawn from the phone" |
| `~/.local/state/ztos/usb-energy.json` | the record (format: `usb-energy.h`) |

## What can be measured

**Nothing on this phone meters VBUS out.** Checked:

- Mainline sysfs: `max77705-charger` and `max77705-usbc` report the input
  side only (online, input current limit, the partner's offer);
  `max77705-otg` is a regulator with `state` and nothing else; there is no
  `usb_power_delivery` class. No current or power for the boost's output.
- Samsung's driver (`drivers/battery_v2/max77705_charger.c`): OTG is
  `CHG_CNFG_00` mode bits and a current *limit* (`CHG_CNFG_02_OTG_ILIM`,
  900 or 1500 mA); `CHG_DTLS` gives states, not current. Its fuel gauge part
  (`max77705_fuelgauge.c`) reads `IIN_REG` 0xD0 (0.125 mA a step, unsigned,
  the *input* current from CHGIN) and `VBYP_REG` 0xB3 (the bypass node's
  voltage), and `ISYS_REG` 0x43. Samsung only ever uses IIN as charging input
  (`POWER_SUPPLY_EXT_PROP_MEASURE_INPUT` while charging). Whether the
  converter's input ADC reads the reverse (boost) current is not in any
  source we have; mainline's `max17042` driver does not expose these
  registers at all. Finding out would need an I2C read of 0xD0/0xB3 with the
  boost on (a read, but of a chip that needs the owner's leave), and a
  kernel change to expose it. Not done.
- USB PD: the S9 is the source here, and the PD contract with a sink (if
  any) is a *maximum*, like bMaxPower. Plain USB-C accessories (EarPods,
  docks without PD in) have no contract at all.

So the record says how each number was got:

| method | what | how good |
| --- | --- | --- |
| `upper bound` | bMaxPower (the active configuration's, `bMaxPower` in sysfs) x 5 V while the phone gives VBUS; 2.5 mA (USB 2.0 7.2.3, a suspended device's limit) while the device is runtime-suspended or the phone is asleep | A ceiling, not a measurement. USB-C devices usually draw well under what they declare (a USB DAC declaring 100 mA often draws 10-40 mA); a few draw more than they should. |
| `estimated from battery current` | the gauge's coulomb counter (`charge_counter` x `voltage_now`) over the session, less the phone's own draw: the gauge's `current_avg` x `voltage_avg` read at the plug-in (the typec partner's uevent, before the device enumerates), moved by ztacct's known screen load (`screen_mw` + `screen_full_mw` x backlight, `/etc/ztacct/power_profile`) when the screen changes, and ztacct's `base_suspend_mw` while asleep | ±60 mW (screen off) or ±0.8 W (screen on), 90 % band, plus 2 mAh of counter steps; see below. It is what having the accessory in cost the battery: the boost's loss and the USB host's own power (xHCI, the PHY: ztacct fits 470 mW for the bus with a dock on it) are in it, so it is above what the device itself drew. |
| `not powered by the phone` | the boost was off all session: a dock with power in, a charger | exact: nothing from the battery |
| `measured VBUS` | reserved, for a meter this phone does not have | |

A session's figure (`wh`) is the estimate when it is clear of its noise
(at least twice its band, over at least 90 % of the powered time, the
charger out) **and** no more than the bound; otherwise the bound. Both are
kept in every session (`upper_wh`, `est_wh` ± `est_err_wh`), so nothing is
lost by the choice. An accessory's method is its sessions' if they agree,
else `mixed`.

### How far the phone's own draw wanders (the estimate's error)

The estimate subtracts a baseline taken once, before the plug-in, so its
error is how far the phone's own draw moves on its own. From the phone's own
logs (`/var/lib/ztacct/calib.csv`, 784 five-minute gauge windows, and
zrn-sleepd's `/var/lib/zrn-sleep/soc.csv`, single `current_avg` reads 5
minutes apart; 2026-11-11 to 11-22, charger out, nothing on USB-C):

| | median | 90 % | sd |
| --- | --- | --- | --- |
| 5-min windows, screen off (472 pairs) | 40 mW | 52 mW | 65 mW |
| 5-min windows, screen on (22 pairs) | 272 mW | 646 mW | 409 mW |
| single reads, screen off (101 pairs) | 9 mW | 29 mW | 85 mW |
| single reads, AOD (2183 pairs) | 4 mW | 34 mW | 201 mW |
| single reads, screen on (23 pairs) | 381 mW | 980 mW | 634 mW |

Re-baselining by ztacct's whole model (not just the screen) did not help
with the screen off (sd 89 mW) and only a little with it on (350 mW), so
only the screen's known load is used. Hence the bands: 60 mW screen off,
0.8 W screen on, 50 mW asleep.

What that means: with the screen off, over ten minutes, the band is about
18 mWh (60 mW x 10 min, plus 2 mAh of counter steps), so an accessory
costing 1 W is measured to about ±10 %, and anything under about 0.2 W is
not resolved at all; the USB-C EarPods (tens of mW;
bound 0.5 W for 100 mA declared) are not resolved and get their bound, about
83 mWh for ten minutes. A dock drawing 1.5 W is resolved even with the
screen on. Playing music raises the phone's own draw too (the CPU, the audio
path), and that lands in the estimate: one more reason the estimate is an
upper side figure, and why it must also stay under the bound to be used.

## The daemon's cost

- Nothing attached: blocked in `poll` on the kernel's uevent socket; no
  timer. It wakes for uevents (USB, typec, and the battery's own changes,
  which it drops at once).
- Something attached: one reading every 30 s (`UE_TICK_S`) on a
  `CLOCK_MONOTONIC` timerfd: a few sysfs files, no process scan. The timer
  does not run while the phone sleeps and never wakes it (8.68's lesson:
  sample by time at a rate the measurement needs, not per event). The
  coulomb counter integrates between readings, so 30 s loses nothing of the
  energy; the readings only place the screen, charger, VBUS and device
  suspend changes, to within 30 s.
- Asleep: the next reading's `CLOCK_BOOTTIME` - `CLOCK_MONOTONIC` gap is
  the time asleep, counted as connected and asleep; the bound uses the
  suspend current, the estimate ztacct's suspend draw.
- The record is written at each plug and unplug, every 5 minutes while
  something is attached (open sessions as checkpoints), and at stop.
  Written whole: a temporary file, fsync, rename, fsync of the directory
  (as `atomic_write` in `ztos-android`).

## Sessions and keys

An accessory is `vid:pid:serial`, or `vid:pid@port` (sysfs name, e.g.
`1-1`, `1-1.2`) without a serial. Every USB device on the host bus is one
(hubs too, root hubs not). A session runs from its add uevent to its remove;
the daemon stopping (shutdown, restart) ends every session, and one still
attached when it starts again gets a new session with no baseline (so the
bound). A session left open by a crash or a hard reset ends at its last
checkpoint. The last 100 sessions are kept per accessory; the totals keep
all, and 64 accessories at most (the least recently seen makes room).

Devices behind one hub share the port's VBUS: the estimate for an interval
is split between the devices by their bounds.

## Tests

`make check-usbenergy`: a fake sysfs (the OTG regulator, the gauge with a
coulomb counter stepping 1 mAh, the charger, the USB-C port's own supply,
the backlight, zrnperfd's status), fake netlink uevents, and these cases:
EarPods screen off (the bound), a 1.5 W dock (the estimate, within 10 mWh),
the screen coming on midway (the screen's load taken out), an hour asleep,
charging midway, a powered dock, the daemon starting with a device in, a hub
with a runtime-suspended device behind it, the record surviving save and
load (with quotes in names), a checkpoint then a hard reset, unknown fields,
a truncated file, and 105 sessions.
