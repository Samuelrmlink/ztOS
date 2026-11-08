# Always-on display (ROADMAP 7.4)

"White on black at low brightness, with elements shifted every couple of
hours to avoid burn-in." With the screen off, the panel shows the time, the
date, the battery, the apps with notifications waiting and where to press for
Start.

## Stage 1: drawn by the shell, the SoC awake (feature/aod, feature/aod-2)

### The rule

With Settings > Display > **Always-on display** on, the screen is never
simply dark when it goes off (power key, timeout, lock-screen timeout): it is
the AOD, unless one of the deliberate cases below holds. Each of those is
logged in `~/.local/state/ztmetro.log` with its reason ("screen off, dark, AOD
not shown: ..." or "screen dark, AOD not shown: ..."); any other dark is a
bug.

### When the screen goes dark instead (deliberate, each logged)

| Reason in the log | Why |
|---|---|
| switched off in Settings | the user's choice |
| set to only while charging, and no charger | the user's choice ("Show it: Only while charging") |
| battery N % < 15 % | only with "Turn off AOD below 15 % battery" switched on (off by default) |
| a call is under way | no proximity sensor is read by the shell: a lit screen that a double tap wakes, against a cheek, is worse than the dark. With a proximity path this becomes "only while the sensor is covered" |
| no display yet / not on the phone's own panel / the demo loop runs | start-up, `--render` and nested runs, the demo loop's own screen handling |
| the display would not switch on | the commit enabling the output failed (logged as an error) |

Not darks: a frame that fails to draw is retried (the last picture stays, the
error is logged; a missing texture is logged as "no picture to show"); a
shell restart starts with the screen on; a woken lock screen left alone goes
back to the AOD. The SoC never sleeps while the AOD shows (see the cost).

### Choices, and where each one lives

| What | Now | Where |
|---|---|---|
| Off until switched on | Settings > Display > Always-on display | the shell: `load_config` (`aod_mode = AOD_OFF`); Settings: `config_load` |
| Mode when switched on | "Show it": **Always** / Only while charging | `AOD_DEFAULT_CHARGING` (false) in `ztmetro/settings/ztsettings.c` |
| Dark below 15 % on battery | a switch, **off** | `aod_low_battery_off`; `AOD_MIN_BATTERY` in `ztmetro/ztmetro.c` |
| Brightness from the room | a switch, **on**; 6 levels, ~2 to ~55 nits (off: level 2, ~7 nits) | `aod_auto_brightness`; `AOD_LEVELS`, `AOD_FIXED_LEVEL`, `AOD_LIGHT_*` |
| Moves every | 10 minutes, round 9 places | `AOD_SHIFT_S`, `AOD_SPOTS` |
| Colours | time 0.92, the rest grey 0.62, the force mark 0.34 tinted with the accent; all times the level's pixel factor | `AOD_WHITE`, `AOD_GREY`, `AOD_FORCE_LEVEL`, `AOD_FORCE_TINT` |
| Waiting apps' glyphs | 16 points, grey, outline only | `AOD_ICON`, `aod_icon` |
| The force press's mark | a squircle outline like stock's: 19 x 18 points, 0.55-point line, exponent 3.2, own 7-place cycle | `AOD_FORCE_*`, `aod_force_mark` |
| Double tap wakes | Settings > Lock Screen, **on**; taps of 200 ms at most, the second within 300 ms and 120 panel pixels (~5.5 mm), 40 px of movement | `double_tap_wake`; `AOD_DOUBLE_TAP`, `AOD_TAP_*` |
| Lock-screen timeout | Settings > Lock Screen, **10 s** (5, 10, 15, 30 s, 1 min, or as the screen) | `lock_timeout`; `LOCK_TIMEOUT_S`; Settings `LOCK_TIMEOUT_DEFAULT` |
| 12/24-hour clock | 24-hour unless `clock_24h = off` | `core/zt-clock.h` (`ZT_CLOCK_24H_DEFAULT`); Settings > date + region |

### How it works

No kernel or boot image change. In `ztmetro/ztmetro.c` (search `aod_`):

- `screen_set(false)` does everything it did (lock sound, Waydroid freeze,
  perfd's "screen off", Wi-Fi suspend, apps told they are hidden) except
  disabling the output. `screen_on` is false, so apps and timers behave as
  for a dark screen.
- Only these wake it: the power key; the force press, if switched on in
  Settings; a double tap, if switched on. A single touch, a touch held
  longer than 300 ms or moved, and two fingers do nothing (no wake, no
  haptic, nothing reaches an app). The double tap must be rapid and local:
  each tap 200 ms at most, the second down within 300 ms and 120 panel
  pixels (~5.5 mm) of the first; a slow, spread or swiping one does nothing. Calls wake it as before (the modem daemon
  sends "screen on"). Confirmed by hand on 2026-11-08: the touchscreen keeps
  sensing with the AOD lit (s6sy761 stays runtime-active with the screen
  off), and the force press wakes it.
- Woken onto the lock screen and left alone, it goes back to the AOD after
  the lock-screen timeout (10 s); any touch restarts it; unlocking switches
  to the screen's own timeout (`idle_timeout_s`, `idle_fired`).
- The panel holds its brightness ramp while the AOD shows: a goal that comes
  meanwhile (light sensor, Settings) is kept for the wake, nothing is
  published to Settings' slider. The AOD's own level comes from the room:
  ztoscsf-sensord takes `aod MS` from the shell and keeps only the light
  sensor on with the screen off, at that rate (zrnperfd's "screen off" is
  unchanged); the shell reads `/run/ztoscsf-sensors/light` every 5 s for a
  minute after the AOD appears or the room changes level, then every 30 s.
  Levels (after a side-by-side photo with Samsung's AOD, 2026-11-08: ours
  was clearly brighter): the panel's own dimming (AOR) goes no lower than
  the table's level 0 (L* 18, ~2.5 % of its 400-nit peak, ~10 nits for
  white), so below that the pixels are dimmed instead, which on OLED cuts
  light and current alike. For the time's white: dark room (< 5 lux) floor
  x0.52, ~2 nits; dim (5-40) x0.70, ~4; a lit room (40-250) x0.88, ~7;
  bright (250-2000) L* 25, ~15; daylight indoors (2000-10000) L* 34, ~30;
  sun L* 45, ~55, Samsung's AOD ceiling. Nits are estimates from the
  shell's table, not measured. 1.6x lux hysteresis; the panel's part of one
  step glides (3 L* a second), more snap; the pixels' part goes with the
  next frame, drawn at once.
- One frame a minute (`clock_tick`), plus one on a charger event and one
  when a notification comes or goes (`led_notes_sync`). A frame is the AOD
  layer alone on black (`aod_render_frame`).
- The ctl takes `aod off|on|charging` for the session (tests, scripts), and
  `aod test battery N|real`, `aod test charger on|off|real`, `aod test lux
  N|real`: the rules tried without touching charging or the room. The state
  file has `aod on|off` and `aod_frames N`; each move logs `aod: place N`,
  each level `aod: N lux, level L`.
- `ztmetro --render DIR` writes `aod.png`, `aod-place-0..8.png`, the
  12-hour clock's longest time (12:59 PM) at each place
  (`aod-12h-place-0..8.png`), the force mark at each of its places
  (`aod-force-0..6.png`) and `lock-clock-12h.png` / `-24h.png`.

### Burn-in

- Everything moves together to the next of 9 places (up to 14 points, 56
  pixels, sideways and 16 up or down) every 10 minutes (`AOD_SHIFT_S`), as
  the research report advised: the date and the battery stand still between
  moves, so they should not stand long. Each place gets the same share.
- Thin strokes (Segoe UI Light for the time), grey (0.62) for all but the
  time, no filled area but the battery's level. A waiting app's glyph is
  drawn as an outline whatever it is (`aod_icon`: the shape less itself
  shrunk by 1.1 points, so a solid glyph keeps only its edge). The
  night-light colour temperature applies (less blue).
- The block starts 18 % down the screen: lit rows 565-1440 of 2960 over all
  nine places, clear of the status bar and the panel's top self-mask band
  (rows 0-149, 1.1b).
- The force press is reported at (753, 2899), inside the bottom self-mask
  band (rows 2810-2959). The mark sits just above the band, over the press's
  column (lowest lit row ~2780), at stock's size (19 x 18 points, measured
  from a side-by-side photo), its brightest channel the grey text's (a grim
  of the live AOD at the darkest level: mark 82,58,63, text 82), a
  0.75-point line, tinted with the accent; it moves by up to 6 pixels on its
  own 7-place cycle (not in step with the nine). Not drawn with the force
  press or "Show where to press for Start" switched off.
- A hung shell leaves a static picture lit until the battery ends. Stage 3
  (the panel's own self-move) is the only real fix.

### Themes

`ztmetro/clock-themes.h`: a theme is a draw function for the clock block (the
time, AM/PM in 12-hour time, the date), the alignment of the shared rows the
AOD adds under it (battery, waiting apps; the force mark is the AOD's), and a
light budget: the frame's mean linear light at most (1 = the whole screen
white). The AOD measures every frame (`aod_measure`) and dims a frame over
its budget (`aod_budget_k`, in `aod_tint`), on top of the level's factor.
Every theme goes through the nine places like the rest; the image's picture
also shifts within its frame. To add one: a function and a line in
`CLOCK_THEMES`, and its id in `clock-theme-ids.h` (Settings' list).

| Theme | Where | Light (12 h, longest time, 5 apps) | Budget | Lit |
|---|---|---|---|---|
| Metro | AOD, lock | 0.0075 | 0.0080 | 1.5 % |
| Compact (left, like stock's) | AOD | 0.0046 | 0.0060 | 1.1 % |
| Digital (large) | AOD | 0.0142 | 0.0090: x0.63 | 2.3 % |
| Analogue (thin hands, no seconds) | AOD | 0.0032 | 0.0060 | 1.1 % |
| Stacked (hours over minutes) | AOD | 0.0115 | 0.0090: x0.78 | 2.0 % |
| Calendar (the month) | AOD | 0.0059 | 0.0080 | 1.5 % |
| Flip clock (static cards, outlines) | AOD, lock (turning) | 0.0064 | 0.0080 | 1.6 % |
| Image (the user's picture) | AOD | by the picture: muted at most 0.34 of full light, faint 0.18, a quarter of its colour, faded to black at its edges | 0.0080 | |

From `ztmetro --render` (`aod-themes.txt`); the current of each is the
overnight item `ztbench-aod-themes.sh`.

Settings > Display > **always-on display**: the mode (Off, Always, Only
while charging), the themes with a preview of each (the shell writes them,
`previews_write`, to `$XDG_RUNTIME_DIR/ztmetro-previews/`, at the fixed
level and budget, the time 10:08), the picture (`ztgallery --pick`) and its
strength, auto brightness, the 15 % switch and the force mark. Settings >
Lock Screen: the clock (Metro or the flip clock) and, with the flip clock,
"use the flip clock on the always-on display too".

The lock screen's flip clock is an original cairo drawing after HTC's
(lovelace-htc-flipclock-weather is MIT; nothing of it is used): two cards
with a gradient, a hinge and pins, a shadow, the date under them, AM/PM
small on the hour card in 12-hour time. On the minute the changed card
turns: the old top half falls, the new bottom half lands (`FLIP_MS` 560 ms
at `FLIP_FRAME_MS` 33, then nothing until the next minute).

### The screenshot chord

Power and volume down together: `grim` into `~/Pictures/Screenshots/
Screenshot_YYYYmmdd_HHMMSS.png`, a white flash (after the capture), a heavy
tick and a "Screenshot saved" notification. Volume down's own step waits
`SHOT_CHORD_MS` (150 ms) for the power key; a chord cancels the power key's
own action. With the screen off the power key wakes it, as before.

### What it costs

The output stays enabled, so `zrn-sleepd` will not put the SoC to sleep
(it needs every `/sys/class/drm/card*-*/enabled` to read `disabled`), and
DECON keeps fetching the frame from memory at each TE edge (command mode,
hardware trigger). Measured with `aod-power.sh` (fuel gauge, cable out,
Wi-Fi associated in power save, sleep switched off as on this phone):
MEASURED
## Stage 2: the picture kept while the SoC sleeps (prepared, untested)

Whether the panel keeps its picture through SoC sleep is **not known**. It
could not be tried: sleep is switched off on the phone, and the phone was
shared. What is known:

- Today's blank cannot keep it: `s9p_decon_blank` always sends a black
  frame first, then display-off and sleep-in (`panel_dcs=N` would leave the
  panel awake but black).
- `kernel/patch-decon-aod.py` (not in build-boot.sh) adds `aod_keep`: blank
  without the black frame, display-off or sleep-in, the trigger left masked
  so no transfer starts on TE while "off", and no sleep-out on enable. With
  it the shell could disable the output (so zrn-sleepd may sleep) with the
  picture still on the panel.
- Unknowns for the bench: the panel's supplies and the DSIM lanes through
  SYS_SLEEP; whether DECON's registers (it has no PM callbacks) survive as
  they do with the panel asleep; the hang risk from TE with the panel awake.
- The time can then only change when the SoC is awake: a sleep cycle is
  heavy (Wi-Fi unbound, the audio DSP restarted), too heavy for every
  minute. So stage 2 shows the time at a 5-30 minute grain, or needs stage 3.

## Stage 3: the panel's own clock and self-move

The panel (S6E3HA8) has stock sequences for HLPM/ALPM at 2-60 nit, its own
clock sprites and a self-move (see the research report). That needs side-RAM
uploads and is the long-term answer for battery and burn-in. Its HLPM
sequence through `panel_cmd` (volatile registers only) is the next cheap
experiment for stage 1: it should cut the panel's own current, and would go
below today's floor (2 nits HLPM against level 0) in a dark room.

Not attempted (2026-11-08): neither this repository nor third_party/s9plus
has Samsung's panel driver source or any panel documentation with the
HLPM/ALPM sequences (grep for hlpm, alpm, aod, lpm: only the stock kernel
config's unrelated LPM options). Without the stock sequences there is no way
to tell which registers are volatile and which would be written to the
panel's MTP, so nothing was sent. What is needed: Samsung's
`s6e3ha8` panel driver from the G960F kernel source (the `*_aod*` and
`*hlpm*` command tables, and which of them are behind the MTP-write key
`F0 5A 5A` / `F1 5A 5A` only to unlock volatile pages).

## Testing stage 1 on the phone

All of it under `ztlock run NAME '...'` when another agent shares the phone.

1. Build: copy the tree to `~/ztdev-aod` on the phone, then there
   `cd ~/ztdev-aod/ztmetro && nice -n 19 make -j2 ztmetro settings/ztsettings sensord/ztoscsf-sensord`
   (not -j8: it heats the phone and drains it fast).
2. Look without installing: `ZTMETRO_SCALE=4 ./ztmetro --render /tmp/aodr` and view
   (without ZTMETRO_SCALE every shell PNG comes out empty: the scale is 0)
   `/tmp/aodr/aod*.png` (offscreen; touches nothing).
3. Install and restart the session: `sudo bash ~/ztdev-aod/power/aod/aod-install.sh`
   (shell, Settings and ztoscsf-sensord; `--undo` puts the old binaries back).
4. Settings > Display > Always-on display. Press power: the time should
   stay, dim. Check `$XDG_RUNTIME_DIR/ztmetro.state` (`screen off`, `aod
   on`), `/sys/class/backlight/s9p-panel/brightness` (0), and journal lines
   `screen off, always-on display` and one frame a minute.
5. Wake with power, the force press or a double tap: the lock screen, at
   the brightness from before; left alone it goes back to the AOD in 10 s.
6. Pull the charger with "Only while charging": dark within a few seconds.
   The ctl-driven checks of every rule: `aod-test.sh` (the first spec) and
   `aod-test2.sh` (the second), `aod-soak.sh MIN` (frames, moves, wakes),
   each run on the phone under ztlock, logging to /var/tmp.
7. Watch for the self-mask bands (1.1b) and for any tint at level 0.
8. Battery: cable out, nobody touching it,
   `sudo SAMPLES=60 setsid -f bash ~/ztdev-aod/power/aod/aod-power.sh`, then
   `/var/tmp/aod-power.log`.
