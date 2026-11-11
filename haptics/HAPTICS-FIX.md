# Haptics fix

## v3 (fix/vibration): pulses shaped from a model of the motor

The user's report: the settings did not change how long a buzz felt; everything rang on ("reverb").
Measured with the phone's own microphone (`s9-microphone`, raw, 48 kHz; the motor's 160 Hz buzz
band-passed, enveloped, and lock-in demodulated at 160 Hz; scripts below):

* **The v2 "brake" drove the motor instead of stopping it.** The chip is signed (a duty as far
  below a half as another is above gives the same buzz in the opposite phase, measured), but a fixed
  brake (up to 10 ms) is mostly longer than the mass needs to stop (the model below says 2 to 7 ms
  at full reverse for the shell's pulses), so the rest drove it the other way. 40 ms pulses at 63 %:
  above -10 dB until 55-61 ms with no brake, 68 ms with 10 ms of full brake, 84 ms with 20 ms; the
  default (10 ms at 70 %) was no better than none (-20 dB at 74 ms against 69 ms).
* **The chip hummed for idle_ms (300 ms) after every pulse.** Enabled and at half duty it is not
  quite still: a steady 160 Hz hum at about -30 dB of a pulse until it was switched off, 300 ms on.
  Starting it again only costs an I2C write (0.2 ms measured), so it is now switched off at once.
* **A fixed 8 ms overdrive at 150 %** made every pulse under 8 ms 1.5 times as strong as asked and
  added a fixed shape that had nothing to do with the settings.
* The motor's time constant is about 14 ms (13-15 ms fitted to the ring-down of unbraked pulses),
  so every pulse was felt about 30 ms (-20 dB) past its end, whatever its length: asked 7 ms -> 40 ms,
  14 -> 49, 24 -> 66, 38 -> 76, 100 -> 137 ms (v2 defaults, onset to -20 dB). Short settings could not
  feel short.

What v3 does (zrn_haptic_main.c): it keeps a first-order model of the motor's amplitude (tau_us)
and plans every pulse from where the motor is: full drive only until the amplitude reaches the
strength asked for (so short ticks start at once and nothing is ever stronger than asked), the
strength asked for until the length is up, then reversed drive for exactly as long as the model says
the mass needs to stop (at most brake_max_ms), then the chip off. Stopping an effect brakes from
wherever it is; restarting plans from the current amplitude. Duty changes no longer restart the
timer's count (only TCMPB1 is written while it runs), so the carrier's phase is never disturbed.

Parameters (`/sys/module/zrn_haptic/parameters`, live): `tau_us` 14000, `od_pct` 100, `brake_pct` 100,
`brake_max_ms` 15, `idle_ms` 0, `zero_trim` 0 (PWM counts added to the half duty). `stats` shows,
per pulse, the overdrive, drive and brake times and the model's amplitude at the start and at the
end of the drive. Old parameters `od_ms`, `od_boost`, `brake_ms` are gone.

Status (2026-11-11): v3's timing is verified on the phone through `stats` (it plays exactly what the
model plans: 7 ms at 28 %: 4.6 ms overdrive, 2.4 ms drive, 3.5 ms brake; 38 ms at 70 %: 16.9 + 21.1
+ 7.4 ms). Its effect on the ring-down is **not yet measured**: the room was too loud for the
microphone while v3 was loaded, and then the phone went offline. To do: record before and after in
a quiet room, tune `tau_us`, then install it as `zrn-haptic.ko`. So far it was only tried as
`zrn-haptic-dev.ko`, bound by hand; the installed module is still v2.

Measuring (`haptics/measure/`): `vt.c` plays pulses (build it on the phone with gcc), `rec.sh`
records the raw microphone (`pw-record --target s9-microphone`) while it plays, `sweep3.sh` repeats
that for several settings of the driver; on the laptop `ana.py` (envelope, -10/-20/-30 dB times),
`tau.py` (ring-down time constant) and `lockin.py` (160 Hz amplitude and phase, to see a brake
oppose the motion) analyse the recordings. The microphone also hears the room: measure only when it
is quiet (the band-passed floor between pulses about 10-20).

---

## v2 (kept for history)

## Why it felt bad

Our driver ran the motor through the kernel's `ff-memless`. Reading it against the stock S9 driver
(`drivers/motor/sec_haptic.c`, `max77705_haptic.c`, `exynos9810-starlte_motor.dtsi` in
xxmustafacooTR/exynos-linux-stable; same numbers as `stock/twrp-live.dts` `haptic {}`):

* ff-memless times a pulse in jiffies (HZ=250, 4 ms steps) from a timer and starts the motor from a
  work item. A 7 ms key tick came out 4 to 16 ms long, never the same twice.
* The motor is an LRA (159.5 Hz, period 48828 ns, duty 70 % normal, 95 % overdrive) with a mass on
  a spring. Driven flat it needs about 15 to 25 ms to reach amplitude and rings for 30 ms or more
  after it stops. Short ticks barely started, then smeared: weak and buzzy. Stock avoids that with
  overdrive at the start and a negative intensity (duty mirrored about one half, which drives the
  motor the other way) to brake. Ours had neither.
* The ring vibration was one 38 ms "heavy" tap every 1.8 s. That is why it was bad.
* Stock also has 7 frequency tables (120 to 200 Hz) for its haptic engine; we only use the main one
  (160 Hz, the motor's resonance), which is right for taps.

## Driver (haptics/zrn_haptic_main.c, no more ff-memless)

* One hard hrtimer plays a pulse in three parts: overdrive (od_ms at od_boost % strength), sustain
  (to the requested length), brake (brake_ms of reversed drive). Only timer registers are touched in
  the timer, so timing is microseconds, not jiffies. An RT kthread does the I2C enable; the chip
  stays enabled idle_ms after a pulse, so the next pulse starts with no I2C.
* Measured on the phone (sysfs `stats`): start latency 0 to 0.5 ms; drive time exact (7 ms asked:
  7.0 ms od, 3.5 ms brake; 300 ms asked: 300.0 ms drive); stop from userspace brakes at once.
* Tunable live, no reload: `/sys/module/zrn_haptic*/parameters/{od_ms,od_boost,brake_ms,brake_pct,idle_ms}`
  (defaults 8, 150, 10, 70, 300). `cat /sys/bus/platform/devices/max77705-haptic/stats` shows the last 8 pulses.
  If taps feel too hard raise nothing: lower `od_boost` (100 = no overdrive) or `brake_pct`.
* `Makefile` now also builds `zrn-haptic-dev.ko` (same source, driver name `zrn-haptic-dev`) to
  try a build next to the loaded one. `ff-memless.c` is no longer used (left in the tree).
* Build: `make -C build/linux-7.1/out M=$PWD/haptics ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
  modules` with `build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin` on PATH.

## Ring vibration patterns

* `ztmetro/core/zrnvibe.h` (header only, shared). Patterns: short, long, double, burst, heartbeat,
  rising, match ringtone, off. A child process plays them as one rumble effect per pulse; SIGTERM
  brakes and exits. Even if killed hard, the motor stops within one pulse.
* "match" reads the ringtone wav (16-bit PCM) at runtime: 10 ms loudness, one pulse per note (a dip
  below half the local loudness starts a new one), strength follows loudness, pulses no closer than
  130 ms start to start and no longer than 600 ms, loops with the wav's length. No ffmpeg needed.
  Unreadable wav falls back to "long".
* Config: `~/.config/ztmetro/ring-vibration` is now `ring silent pattern` (old two-number files still read; pattern
  defaults to long).
* `ztphone.c`: call_sound() starts the pattern when a call is incoming and restarts it if the pattern or ringtone changes;
  `sound_stop()` also stops it, and every answer, ignore, hangup and call-end path already calls sound_stop().
  The orchestrator's `modem_tell("speaker on/off")` line is kept.

## Settings page (ztsettings.c: only the audio page and its helpers)

"ringtones + sounds" is now four accent-coloured sections: volume (two sliders), ringtone (one picker
box that opens into the list, replacing 30 inline rows), vibration (vibrate when ringing / when silent,
pattern picker, "feel it" preview), system (sound engine). Picking a ringtone previews it (and its
matching vibration, if the pattern is match). Picking a pattern saves it and plays it once; for match
the ringtone plays with it. Leaving the page stops both.

## Installed on the phone

* `/usr/lib/modules/7.1.0-zboot/extra/zrn-haptic.ko` = new driver (old: `zrn-haptic.ko.before-haptics`). It loads at next boot.
* `/usr/local/bin/ztphone`, `/usr/local/bin/ztsettings` (backups `*.before-haptics`). Built in `~/agent-haptics` from the
  laptop's current `ztmetro/` tree (the repo is newer than `~/ztmetro-phase2`), so ztsettings also carries the other
  person's uncommitted wallpaper-worker changes.
* Live state: to test without a reboot I loaded `zrn-haptic-dev.ko` with insmod, unbound the old driver from
  `max77705-haptic` and bound the dev one (no rmmod anywhere). The old module is still loaded but unbound. At next boot
  the new `zrn-haptic.ko` replaces both. Rollback: copy `.before-haptics` back.

## Not done / to test

* No accelerometer on the phone (sensor hub failed), so strength and crispness were not measured, only timing.
  Feel needs the user's hands; tune with the module parameters above.
* The HAPS table in `ztmetro.c` (tick/tap/click/heavy/key lengths and levels) is unchanged; the shell was not restarted.
  The shell finds the new input device on its own (rescans within 3 s of a failed write).
* Settings page was checked with `ztsettings --render` screenshots only (not driven on the live screen); taps on the
  pickers and "feel it" are untested by hand. Pattern playback was tested through a harness using the same header
  (all 8 patterns, start/stop timing, stats).
