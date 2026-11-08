# Always-on display (ROADMAP 7.4)

"White on black at low brightness, with elements shifted every couple of
hours to avoid burn-in." With the screen off, the panel shows the time, the
date, the battery and the apps with notifications waiting.

## Stage 1: drawn by the shell, the SoC awake (feature/aod, feature/aod-2)

### Choices, and where each one lives

| What | Now | Where |
|---|---|---|
| Off until switched on | Settings > Display > Always-on display | the shell: `load_config` (`aod_mode = AOD_OFF`); Settings: `config_load` |
| Mode when switched on | "Show it": Always / **Only while charging** | `AOD_DEFAULT_CHARGING` in `ztmetro/settings/ztsettings.c` |
| Dark on battery below | 15 % | `AOD_MIN_BATTERY` in `ztmetro/ztmetro.c` |
| Moves every | 10 minutes, round 9 places | `AOD_SHIFT_S`, `AOD_SPOTS` |
| Colours | time 0.92, the rest grey 0.62 | `AOD_WHITE`, `AOD_GREY` |
| Waiting apps' glyphs | 16 points, grey, outline only | `AOD_ICON`, `aod_icon` |
| Double tap wakes | on; 300 ms tap, 450 ms gap, 48 points | `AOD_DOUBLE_TAP`, `AOD_TAP_MS`, `AOD_TAP_GAP_MS`, `AOD_TAP_SLOP` |
| 12/24-hour clock | 24-hour unless `clock_24h = off` | `core/zt-clock.h` (`ZT_CLOCK_24H_DEFAULT`); Settings > date + region |

No kernel or boot image change. In `ztmetro/ztmetro.c` (search `aod_`):

- With Settings > Display > **Always-on display** on, `screen_set(false)`
  does everything it did (lock sound, Waydroid freeze, perfd's "screen
  off", Wi-Fi suspend, apps told they are hidden) except disabling the
  output. `screen_on` is false, so touches, apps and timers behave as for a
  dark screen; the power key, home and the force press wake it as before.
- A double tap on the glass wakes it too: the touchscreen keeps sensing
  while the panel is lit (s6sy761 stays runtime-active with the screen off;
  it always did, the shell just ignored the touches). A single tap, a touch
  held longer than 300 ms or moved, and two fingers do nothing. With the
  always-on display off, the glass does nothing, as before.
- The panel goes to the brightness table's level 0 (L* 18, the floor of the
  shell's own ramp; about 2.5 % of the peak's luminance). The ramp is held:
  a goal that comes meanwhile (light sensor, Settings) is kept for the wake,
  and nothing is published to Settings' slider.
- One frame a minute (`clock_tick`), plus one on a charger event. A frame
  is the AOD layer alone on black (`aod_render_frame`); nothing else is
  drawn while it shows.
- **Only while charging** (`aod=charging`). On battery it goes dark below
  15 %, and comes back on a charger.
- The ctl takes `aod off|on|charging` for the session (tests, scripts),
  and `aod test battery N|real` and `aod test charger on|off|real`: the
  rules (15 %, only while charging) tried without touching charging.
- `ztmetro --render DIR` writes `aod.png`, `aod-place-0..8.png`, and the
  12-hour clock's longest time (12:59 PM) at each place,
  `aod-12h-place-0..8.png`, with `lock-clock-12h.png` and `-24h.png`.

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
- The block starts 18 % down the screen: clear of the status bar and the
  panel's top self-mask band (rows 0-149, 1.1b) at every place, and far
  from the nav bar and the bottom band.
- A hung shell leaves a static picture lit until the battery ends. Stage 3
  (the panel's own self-move) is the only real fix.

### What it costs (inferred, not measured)

The output stays enabled, so `zrn-sleepd` will not put the SoC to sleep
(it needs every `/sys/class/drm/card*-*/enabled` to read `disabled`), and
DECON keeps fetching the frame from memory at each TE edge (command mode,
hardware trigger). power/README.md measured the awake, display-off panel at
**157 mA** (2026-10-13); AOD should cost about that over a dark screen,
plus little for the pixels at the floor (the report: 0.5-3 mA). With sleep
switched off (as on this phone now), that is roughly 350-450 mA instead of
200-300 mA: 6-7 h instead of 10-ish on a full battery. With working sleep
the difference is far bigger. Hence "only while charging". Measure with
`aod-power.sh`.

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
experiment for stage 1: it should cut the panel's own current. Not
attempted here.

## Testing stage 1 on the phone

1. Build: `git archive feature/aod | ssh phone 'mkdir ~/ztdev-aod && tar x -C ~/ztdev-aod'`,
   then on the phone `cd ~/ztdev-aod/ztmetro && make -j8 ztmetro settings/ztsettings`.
2. Look without installing: `ZTMETRO_SCALE=4 ./ztmetro --render /tmp/aodr` and view
   (without ZTMETRO_SCALE every shell PNG comes out empty: the scale is 0)
   `/tmp/aodr/aod*.png` (offscreen; touches nothing).
3. Install and restart the session: `sudo bash ~/ztdev-aod/power/aod/aod-install.sh`
   (`--undo` puts the old binaries back).
4. Settings > Display > Always-on display. Press power: the time should
   stay, dim. Check `$XDG_RUNTIME_DIR/ztmetro.state` (`screen off`, `aod
   on`), `/sys/class/backlight/s9p-panel/brightness` (0), and journal lines
   `screen off, always-on display` and one frame a minute.
5. Wake with power or home: the lock screen, at the brightness from before.
6. Pull the charger with "Only while charging": dark within a few seconds.
7. Watch for the self-mask bands (1.1b) and for any tint at level 0.
8. Battery: cable out, `sudo setsid -f bash ~/ztdev-aod/power/aod/aod-power.sh`,
   then `/var/tmp/aod-power.log`.
