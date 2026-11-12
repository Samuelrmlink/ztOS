# Burn-in protection (ROADMAP 8.37)

The S9's S6E3HA8 is an AMOLED: every pixel ages by how long and how bright it
is lit. A picture that stands still for hundreds of hours leaves its ghost.
The always-on display has its own protection (`power/aod/README.md`,
"Burn-in"), and it is the owner's design, left as it is. This covers
everything else.

## What stands still, and for how long

Hours are estimates for a heavy day (about 4 h of screen-on, 80 wakes),
not measured; the lock screen's 10 s timeout and the screen's 1 min timeout
are the defaults.

| What | Lit per day | What is static | Risk | Now |
|---|---|---|---|---|
| Status bar | ~3.5 h (all of screen-on but full-screen apps) | signal, Wi-Fi, BT, battery outline, the clock's colon; white on black | **highest**: the same white glyphs at the same pixels every hour the screen is on | shifted (3 x 2 px) |
| Nav bar | ~3.5 h (hidden in full screen) | three white glyphs (back, Start, search) | **high** | shifted (3 x 2 px) |
| Start's tiles | ~0.5-1 h | the tile grid's 3-point gaps, accent fills; live tiles change their insides, the edges stay; scrolling moves them | medium: filled areas age evenly, the gaps' edges do not | not shifted (see proposals) |
| Lock screen clock | ~15 min (80 x 10 s) | time, day, date; the photo | low | shifted (4 x 4 px) |
| Call screen | ~10-30 min | the in-call buttons; no proximity sensor read, so it stays lit unless the screen times out | low-medium | not shifted |
| Keyboard | ~20-40 min | the key grid: dark grey keys, white letters | medium (always the same place, bright letters) | not shifted (touch geometry) |
| ztdesk (the desktop, seen on the phone) | 0 to several hours when docked | the scaled XFCE panel and ztdesk's own bar; a touchpad is an unchanging picture under the finger | **high when used for hours**: the screen timeout is reset by every touch | not shifted (see proposals) |
| XFCE on the phone panel | only if chosen | XFCE's panel bar, full white text | high if used | not shifted |
| DP-1 (external monitor) | as docked | XFCE's panel | only if that monitor is an OLED: then its own pixel shift (most OLED monitors have one) | n/a |
| AOD | up to 24 h | moves itself, 9 places / 10 min | the owner's design | unchanged |

## What Samsung does

From the last stock firmware (G960FXXUHFVG4, One UI 2 / Android 10: SystemUI,
services.jar, framework-res and its vendor overlay, SettingsProvider) and
Samsung's G960F kernel (`exynos-linux-stable/starlte`, branch
`tw10-android-4.9-q`, `drivers/video/fbdev/exynos/panel/`). Bytecode
readings are not checked on a device.

| What | Stock | Where |
|---|---|---|
| Status bar shift | a step at every screen-off (and rotation), sideways up to 3 dp (9 px) and back, up/down -1/0/+1/0 px | SystemUI `IndicatorMarqueeGardener`; `qs_indicator_marquee_max_shift_size` = 3dp |
| Nav bar shift | a step at every screen-off (and rotation), up to ~12 px sideways and ~8 px up/down, about 1 px a step | SystemUI `SecNavigationBarView.marqueeNavigationBar`, `MarqueeLogic`; `NAVBAR_SUPPORT_ICON_MOVEMENT` |
| AOSP's own burn-in shift | off (`config_enableBurnInProtection` false, offsets 0); AOSP's only moves the doze picture, 2 px every 2 min | framework-res `config.xml`; `BurnInProtectionHelper` |
| AOD | small zig-zag moves each minute, a big jump every 10, and the panel's own AOD self-move (re-armed about every 10 min) | AODService_v50 `ZigZagViewPositionControllerImpl`; kernel `aod/aod_drv.c` |
| **Panel self-move, screen on** | **on**: 3 s after screen-on, then every 19 min, the service writes pattern 1-4 to `/sys/class/lcd/panel/self_move`; 0 at screen-off. Off with negative colours or for VR apps | services.jar `MdnieScenarioControlService.pixel_self_move`; vendor overlay `config_pixel_self_move_enabled` = true |
| Brightness stepping | with auto-brightness above level 143: down 2 % a minute to 85 % (not maps/navigation) | `MdnieScenarioControlService.burn_in_prevention_mode`; `config_Display_Solution_Burn_In_Scale_Factor_Value` |
| Screen timeout | 30 s, dim before it (up to 7 s, at level 59) | SettingsProvider `def_screen_off_timeout` = 30000; `config_maximumScreenDimDuration`; overlay `config_screenBrightnessDim` = 59 |
| ACL (current limit) | on: `55 02`, `B4 00 44 40 FF 26 49` (15 % OPR, 8 % in HBM, off in AOD), sent with every brightness | `s6e3ha8_star_a3_s0_panel.h` 511-532, `panel_bl.c` 447-478 |
| ELVSS temperature compensation | `B5 <TSET> DC <ELVSS>`, by temperature band and brightness | `s6e3ha8.c` `copy_tset_maptbl` |

Our shift is time-based (3 min), not screen-off-based: our status bar
stays lit across long sessions without a screen-off, and the minute redraw
makes it free. Our amplitude (3 px) is about a third of stock's; the
screen timeout default (60 s) is stock's 30 s doubled.

## What ztOS does now

### The shift (`ztmetro/burn-in.h`)

- The status row (in the bar, on the lock screen, in the held bar and the
  action centre's top), the nav bar's three buttons and the lock screen's
  clock block are drawn a few panel pixels off their place: status and nav
  up to 3 px sideways and 2 px up or down, the lock clock 4 and 4.
- Nine places on a 3 x 3 walk, each next to the one before; a new place
  every 3 minutes (`BURNIN_STEP_S`), round all nine in 27 minutes, each
  place the same share. The place comes from the wall clock, so it goes on
  through a shell restart. The nav bar runs 4 places ahead of the status
  bar: the two never move the same way at once.
- Whole panel pixels at any display scaling (the step is pixels over the
  scale), and in either orientation (it moves the drawing, before the
  output's transform).
- **No frame of its own.** The status bar and the lock clock are drawn on
  the minute anyway (`clock_tick`) and take the new place then. The nav bar
  is not: when its place has changed it is drawn again in that same minute's
  frame (20 small uploads an hour, no extra frame, no extra wakeup).
- Settings > display > **Shift the bars against burn-in** (`burn_in_shift`,
  on by default).
- Not in the DotMatrix style (its dot grid would tear) and not in the AOD.
- `ztmetro --render DIR` writes `burnin-status-N`, `burnin-nav-N`,
  `burnin-lock-N` for each place. `make check-burn-in` tests the schedule.

### A hung shell (`zrn-sleepd`'s ShellWatch)

- The shell's minute tick writes `$XDG_RUNTIME_DIR/ztmetro.alive`: pid,
  `CLOCK_MONOTONIC` seconds (which stop while the SoC sleeps, so a phone
  woken after hours is not a silent shell), whether the panel should be lit,
  and how long a frame it asked for has waited.
- zrn-sleepd looks at it every 30 s inside its own 2 s loop. With the
  phone's DSI panel lit (screen or AOD; DP does not count) and the heartbeat
  5 minutes old, or a frame waiting 5 minutes, it sends the shell SIGTERM
  and SIGCONT, and SIGKILL after 5 s.
- The shell's display file closing is DRM's own way to dark: its
  framebuffers are removed, which switches the CRTC off, and the s9p driver
  puts the panel to sleep exactly as for a screen-off. **No panel command is
  sent by the watch.** It then asks fb0's blank too, in case the console
  took the panel (that works only with no compositor holding the display).
- greetd starts a new shell, which shows a notification. Three hung shells
  within 30 minutes stop the session (greetd), dark until a reboot.
- Left alone: a shell under a debugger, a pid that is not `ztmetro`, a shell
  that writes no heartbeat (an older build).
- `power/suspend/test-shell-watch.py` (16 cases, in `make check-burn-in`).

## Proposals, not done

### The panel's own self-move (needs the owner's approval: panel commands)

Stock runs it all the time the screen is on. The whole picture moves inside
the DDI, so it also covers what the shell cannot shift (Start, the
keyboard, a hung shell's last frame while the watch waits). Sequences from
`s6e3ha8_aod_panel.h` (`tw10-android-4.9-q`), sent the way the kernel's
`panel_cmd` (`kernel/patch-decon-panel-io.py`) sends them. **None writes F1,
C0 or C1.** Enable **opens the level-3 key (FC 5A 5A)**, the key that also
guards the panel's flash and MCD registers, to write FE at offset 0x15:
that is the risky part.

Enable (`s6e3ha8_enable_selfmove`, lines 796-810; stock writes it for
pattern 1-4):

    FC 5A 5A                          level-3 key on
    F0 5A 5A                          level-2 key on
    B0 15 ; FE 00                     "move setting" (FE at offset 0x15; meaning unknown)
    76 09 0F 85 A0 0B 90              the AOD porch setting
    77 00 04 00 00 00 00 00 03 00 00  self timer on (FAST_TIMER off)
    7C 00 01 00 00 00 00 00 00 00 00  self-move set
    7D + 75 bytes                     the pattern (lines 277-305): row 0 for patterns 1/3,
                                      row 1 for 2/4 (they differ only in byte 6: 00 / 01):
        00 00 00 00 00 0r 55 10 01 50 05 10 01 50 05 10 01 50 05 10 01 50 05 10 01
        (45 x 00) 41 C3 41 C3 41 C3 41 C3 41
    F0 A5 A5 ; FC A5 A5               keys off

Disable (`s6e3ha8_disable_selfmove`, lines 812-817; stock: pattern 0, and
at every screen-off):

    F0 5A 5A ; 7D 01 ; 76 09 0F 00 ; F0 A5 A5

Proposed trial, one step at a time, watching the phone's own panel
(photographed; the HDMI rig does not see it): (1) disable alone (F0 only,
harmless if self-move is already off); (2) enable with pattern row 0,
watch for 20 min that the picture moves and nothing else changes
(colours, brightness, the self-mask bands); (3) disable, and a screen
off/on, to see the panel come back normal. Then, if good: zrn-sleepd or the
shell sends enable 3 s after each screen-on and every 19 min with the next
pattern, disable at screen-off, as stock does. Unknowns: how far it moves
(the position bits are undocumented) and whether our DECON's frame trigger
copes with a panel that moves the picture itself.

### Others

- **Screen timeout:** stock's default is 30 s; ours is 60 s. "Never" is
  offered; a warning under it ("the screen can burn in") would fit Settings.
- **Dim before the timeout**, as stock (a few seconds at a low level): one
  ramp of the brightness, no redraw; also tells the user it is about to go.
- **Stock's brightness stepping** (2 % a minute to 85 % at high brightness
  with auto-brightness on): cheap, the shell already owns the brightness
  ramp.
- **ztdesk on the phone:** a touchpad needs no picture: dim the phone's
  panel to the lowest level (or dark, with touches still read, as the AOD
  does) after 30 s without a touch while a monitor shows the desktop.
- **The keyboard and Start** could take the same shift at their own redraws
  (the keyboard is redrawn every time it comes up): the touch geometry would
  have to follow it. Not done: a 3 px offset in hit-testing is invisible but
  it is a change to every key's rectangle.
- **The AOD** (the owner's design): nothing to change found. Stock moves its
  AOD further (a big jump every 10 min across its range) and also lets the
  panel self-move it; ours already moves 9 places every 10 min.

## Tested

- Host: `make check-burn-in` (the schedule; the heartbeat file; ShellWatch,
  16 cases); `power/suspend/test-sleepd-recovery.py` still passes; the shell
  and Settings build clean (desktop/dev container).
- Phone, 2026-11-12: the integrated shell (with this branch) writes the
  heartbeat (pid = the running shell, monotonic age under a minute at two
  readings 6 minutes apart); zrn-sleepd with ShellWatch installed and running,
  no action taken against the live shell; the lock screen drawn at place 8,
  looking right. Not yet done (battery reached 15 %): the screenshot pair
  across a place change, the 7-minute frame/upload count, and
  `power/suspend/shell-watch-device-test.py` (ShellWatch against a dummy
  process named ztmetro, never the session's shell).
