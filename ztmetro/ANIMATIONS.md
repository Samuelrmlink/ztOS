# Animations

Every animation in the shell (`ztmetro.c`), the Metro app core
(`core/zrnmetrocore.c`, so every native app), Settings and the gallery, the
kind it belongs to, and where it is coded (ROADMAP 8.66).

## The model

`core/zt-anim.h` holds one speed per **kind** and a **master** speed over them.
Each is a step: Off, 0.25x, 0.5x, 0.75x, 1x, 1.25x, 1.5x, 2x or 3x. A kind
runs at its own speed times the master's (the screen's turn ignores the
master's speed, only its Off applies). Slow motion (a developer switch) makes
everything five times as long on top.

Every duration goes through one helper:

| Where | Helper | Off gives |
|---|---|---|
| shell, `animate*()` | `animate(server, KIND, ...)` → `anim_ms()` | the value set at once, no frames, running animation on it cancelled |
| shell timers waiting on a motion | `anim_wait()` | 1 ms (0 would disarm the timer) |
| shell springs and glides (stepped by the clock) | `anim_dt()` | one 10 s step: it lands where it was going this frame |
| shell motion timed from a start stamp | `anim_elapsed()` | 10⁹ ms: past its end |
| app core, page turns | `turn_rate` / `turn_elapsed()` | ends at its next frame (`arrived()` runs then) |
| app core, press, picker, board, tabs, fling | `PRESS_*_MS`, `PICKER_*_MS`, `board_ms()`, `zanim_dt()` | 1 ms / at once |
| apps' own motion | `zmc_anim_ms()`, `zmc_anim_wait()` | 0 / 1 ms |
| Python SDK apps | `ANIM_SPEED` (`sdk/runtime/ztos/__init__.py`) | as fast as the toolkit goes |

Nothing waits for an animation-end callback that Off would skip: the shell's
ends are polled each frame (`animating()`), the core's page turn ends on its
next frame, and timers that wait on a motion get 1 ms.

### Config (`~/.config/ztmetro/config`)

```
anim_global = 75          # the master, per cent; off = no animation at all
anim_settings_link = off  # one per kind: anim_<key>, per cent or off
slow_motion = off
```

Values may also be written `on` (100) or `1.5x`; anything between steps is
taken to the nearest. A key that is absent is at its default: master 75 (the
shell's speed since 2026-11), every kind 100.

**Migration.** Before 8.66 there was one speed, `menu_speed` (25-300, default
75) and `slow_motion`. A file with `menu_speed` and no `anim_global` reads it
as the master (snapped to a step: 60 → 0.5x). Settings writes only what
changes, so an untouched file stays as it was; once the master is changed it
writes `anim_global` and keeps `menu_speed` beside it, equal to the master (300
when Off), for an older shell or the SDK.

### Applying a change

Settings > display > animations writes the file without a SIGHUP when only
speeds changed (`config_save_anim`, as the brightness slider's save, 8.56) and
says `anim-reload` on the shell's control pipe: the shell reads the anim keys
alone (`anim_reload`), not the whole config with its tiles and textures. Apps
read the file again (one `stat`) at each page turn and each touch
(`zanim_refresh`), so the next motion is at the new speed. A full SIGHUP
reload reads them too.

The app's own entrance (its rows arriving as it opens) is timed by the shell,
which passes the speed in `ZTMETRO_MENU_SPEED` (0: off, the page is drawn
settled) with `ZTMETRO_ENTER_MS`.

## Kinds and what is under each

Durations are as written in the code, at 1x; the shell's at the default master
(0.75x) last 4/3 as long.

### App transitions

**Opening an app** (`app_open`)
- Start's tiles turnstile away: `start_turn(away)`, `start_t` 0→1 linear over
  `start_out_ms()` (50 ms a row, 51 ms left after right, 180 ms a tile's
  life, the tapped tile 80 ms later; about 600-800 ms for a screen of tiles).
- Black gap: `TURN_GAP_MS` 170.
- Another app swings in: `show_toplevel`, `app_angle` 80°→0 and `app_alpha`
  over `APP_IN_MS` 520, ease-out.
- A native app's own rows glide in (entrance): core `begin_animation` with
  `zmc.entrance`, rows 190 ms, title 200 ms, 28 ms stagger
  (`ENTER_*_MS`), at the shell's speed; uncovered at `ZTMETRO_ENTER_MS`.
- "Loading..." waits until Start has gone (`loading_frame`'s `gone`).
- Landscape launch flip: `lf_begin`, `lf_t` over `LAUNCH_MS` 380 linear (kept
  exact: only Off and slow motion apply), splash cross-fade `LAUNCH_XFADE_MS`
  150.
- Triggered by: a tile, the app list, search, a toast, the switcher.

**Closing an app** (`app_close`)
- The app swings back the way it came: `show_toplevel(NULL)`,
  `animate_rev` on `app_angle`/`app_alpha`, `APP_IN_MS` 520 ease-in.
- Gap `BACK_GAP_MS` 170, then Start's tiles return: `start_turn(!away)` over
  `return_ms` (the launch's schedule reversed).
- An app's own rows turning away as it closes from its first page
  (core `quit_after_anim`, 128 ms a row, 33 ms stagger).
- Also: Start's tiles coming back after the boot logo or a failed launch.
- The live tile's flip 1 s after Start is back (`home_flip_arm`) waits for
  this kind's length.

**Pages in apps** (`app_pages`)
- Core page turn (`zmc_go`, `zmc_turn`, back): old rows turn away 128 ms each
  (`ROW_OUT_MS`, t² ease), 33 ms stagger, 120 ms black (`GAP_MS`), new rows
  180 ms each (`ROW_IN_MS`, cubic ease-out).
- List pickers opening 250 ms / closing 200 ms (`zmc_picker`).
- Boards (accent colours, letters; `zrnboard.h`): Readerboard swivel 350 ms
  in / 250 ms out a cell, staggered (`board_ms`).
- Pivot tabs letting go: critically damped spring, `PIVOT_SPRING_W` 0.018/ms.
- Gallery viewer: open/close 220, photo slide 240, zoom 200, thumbnail fade 140
  (`gallery/ztgallery.c`).
- SDK apps (Python): `ANIM_SPEED` is the master times this kind.

**Task switcher** (`switcher`)
- Opening/closing/picking: `switch_t`, `switch_x` over `SWITCH_MS` 260.
- A card swiped away: `switcher_forget` 180; a card thrown up (`drop`) 90-240
  by the throw's speed, back 260; the landscape cover-flow card (`cf_drop`)
  160 / 220-260.
- The row let go: a spring (k 0.014/ms) in `physics_step`.

### Settings

**Opening a Settings page** (`settings_link`) — the owner's "jumping into the
settings".
- Path: quick tile held (Wi-Fi, Bluetooth) → `shade_set_open(false)` (the
  shade's own close is the **Action centre** kind) → `launch("ztsettings
  wifi")` → `launch_core` marks `settings_link_ms`.
  - Settings not running: Start's tiles away and the gap (`start_turn`), then
    Settings' entrance, all at this kind's speed (`open_kind()`), and
    `ZTMETRO_MENU_SPEED` carries it to Settings.
  - Settings running: `ztsettings wifi` tells the running one
    (`instance_tell`), which turns to the page as `ZT_ANIM_SETTINGS_LINK`
    (`go_to_outside_page` sets `zmc_next_anim`) and asks the shell to
    `launch ztsettings`; the window swings in (`app_angle`, 520) at this
    kind's speed. The mark is used up when Settings is shown.
- Also search results that are Settings pages (`hyper_launch`), the battery
  tile (`ztsettings battery`) and other apps opening a Settings page.
- Off: no tiles turning, no gap, no entrance, no page turn: the page is there.

**Settings pages** (`settings_pages`)
- Settings' own page turns (as **Pages in apps**, `zmc_page_anim` is
  `ZT_ANIM_SETTINGS_PAGES` in Settings), its pickers, the wallpaper board,
  the Bluetooth list's rows sliding (`bt_anim_tick`, τ 75 ms).

### Start

**Start and the app list** (`start`)
- Start ↔ app list swipe let go: spring k 0.018/ms (`page_moving`).
- Edit mode in/out `EDIT_MS` 180; a tile's menu 200 in / 140 out; pinning
  (`pin_item`, 320 after 220); a dragged tile's neighbours settling (τ 70 ms).
- Search (`hyper_open` 250, `hyper_close` 180).
- The app list's letter board (Readerboard, 350/250 a cell).
- Landscape: cover-flow split 300, sweep 120 a card (≤900), rail 200,
  divider spring, cover-flow spring.

**Press tilt** (`press`)
- Start tiles: in 75-90 ms linear, out `TILT_OUT_MS` 200 on Log(10)
  (`tile_press`, `tile_release_after`); app list rows (`item_press`), quick
  tiles in the shade (`shade_tile_press`, 70/200).
- Apps: core `press_amount`, `ZMC_TILT_IN_MS` 90 / `ZMC_TILT_OUT_MS` 200.

**Scrolling** (`scroll`)
- Start, the app list, the action centre's list and the cover-flow rail:
  glide τ `SCROLL_TAU_MS` 380, past an end a spring `SPRING_W` 0.016/ms that
  does not bounce back over (`scroll_step`).
- Apps: core `fling_step`, glide τ 350, past an end τ 35/110 (the overscroll
  bounce).
- Off: the list lands where the flick would have taken it, in one frame.

### Shade & quick tiles

**Action centre** (`shade`)
- Opening/closing after the finger lets go: `shade_set_open`, `shade_h` over
  `SHADE_MS` 260 ease-out.
- A notification swiped away (`note_swipe_physics`, τ 55/70) and its row
  closing (`note_close_h` 180); "clear all" cascade (`note_clear_x`).

**Quick tile panels** (`quick_panels`)
- Brightness and torch level panels from a quick tile held: 260 down, 170 up
  (`level_modal_show/close`).

### Live tiles

**Live tile flips** (`live_tiles`)
- A tile turning to its back and front: `tile_flip_to`, spring 520 ms with
  8 % overshoot (every 4-10 s; one tile at a time, `LIVE_FLIP_GAP_MS`).
- A signal tile going back to its front (`tile_signal_clear`, 500).

### System

**Lock screen** (`lock`)
- Unlock swipe: `lock_open`, `lock_y` up 200, the app behind settling 450;
  a tap's lift and drop 320; let go short of it 220.
- The passcode pad rising and dropping (220), a wrong code's shake (300).
- The lock lifting for the clock (`lock_lift_sync`, 300 Log(8)).
- The flip clock's cards (`flip_frame`, `FLIP_MS`).
- At start: the boot logo (280/460 in, 450 hold, 85 out) and the nav icons and
  lock screen coming in (`boot_anim_start`, 480-750 ms after 0.7-2.2 s).

**Volume panel** (`volume`): 260 down, 170 up (`audio_modal_show/close`).

**Notification toasts** (`toast`): swivel in 300, out 199 (`note_add_from`,
`toast_hide`, `wd_toast`). How long it stays (`TOAST_MS` 4.5 s) is not an
animation and does not change.

**Dialogs** (`dialog`): the shell's dialogs drop in 260, go 170.

**Keyboard** (`keyboard`): the on-screen keyboard sliding (`osk_slide`,
`OSK_MS` 200) and the app lifting above it (`app_lift`, 200).

**Screen rotation** (`rotation`): the turn from the frozen picture
(`ROT_MS_90` 300, `ROT_MS_180` 400) and the black stage in landscape
(`CF_STAGE_MS` 300). Not slowed or quickened by the master speed (it is
matched to the panel's own); Off and slow motion apply.

**Status bar** (`bars`): the bars over a full-screen app (`fs_bars`, 180), the
status bar held down (`peek`, 180 in / 220 out), the bars' cross-fade
(`BAR_XFADE_MS` 180), the screenshot flash (`SHOT_FLASH_MS` 280).

## Not under a kind

- Snaps (`snap()`, `ANIM_EXACT`): 1 ms jumps that only end a motion.
- Waits that are not motion: long press, toast and volume panel stay times,
  key repeat, peek's stay, the launch wait.
- The backlight's ramp (brightness, not the UI).
- The phone app's call panels flip (`phone/ztphone.c` `FLIP_IN_MS` 600 /
  `FLIP_OUT_MS` 500): timed against the shell's call Readerboard; left at its
  written speed.
- The live tile backs' marquee (`SWEEP_SPEED`) and the action centre's media
  progress: they move with time, not as transitions.
