# The AOD on the panel: stock's design, ours, and every command (ROADMAP 8.45)

The owner asked for this on 2026-11-12: "offload the AOD functionality to the
panel when possible... I just need to be able to sleep the SoC to have decent
battery life." Today (`README.md`, stage 1) the shell draws the AOD, so the
SoC never sleeps. On the night of 2026-11-12 that cost about 2 W, and the
battery reached 55 °C.

Sources. They are Samsung's G960F kernel (`github.com/exynos-linux-stable/starlte`,
branch `tw10-android-4.9-q`) and the stock firmware's AOD service and framework,
decompiled. Paths below are relative to `drivers/video/fbdev/exynos/`:

- `panel/s6e3ha8/s6e3ha8_star_a3_s0_panel.h` is **S0**. S1 (`..._a3_s1_...`) has the
  same LPM bytes and sequences, apart from the "metal" exit variant, which is
  identical in content.
- `panel/s6e3ha8/s6e3ha8_aod_panel.h` is **AODP**.
- `panel/s6e3ha8/s6e3ha8_aod.c` is **AODC**.
- `panel/aod/aod_drv.c` is **DRV**.
- `panel/panel_drv.c` is **PD**.
- `dpu_9810/dsim_drv.c` is **DSIM**.
- `exynos9810-starlte_defconfig` (stock) has `SUPPORT_DOZE`, `EXTEND_LIVE_CLOCK`,
  `SUPPORT_AOD_BL` and `DISP_PMIC_SSD` set. `ACTIVE_CLOCK`, `SELF_MOVE_3PX` and
  `SUPPORT_TDMB_TUNE` are not set (defconfig lines 3252-3290).
- The AOD app (`com.samsung.android.app.aodservice`), SystemUI and services.jar
  were decompiled with jadx. Their file:line references are in the
  "Stock, the Android side" section.

## 1. How stock does it

### 1.1 HLPM entry and exit (the panel's low-power mode)

The panel has two low-power modes. ALPM is the older one. HLPM is the one the
AOD uses: `BrightnessManager` only ever asks for `HLPM_LOW_BR` (2) or
`HLPM_HIGH_BR` (4), and `isAlpmBrightness()` is always false. PowerManagerService
writes that number to `/sys/class/lcd/panel/alpm`. The kernel then runs:

**Entry**, `star_a3_s0_alpm_enter_cmdtbl` (S0 2557-2573), after `__delay_normal_alpm`
(PD 390), which keeps at least 124 ms from the panel's power-on (S0 2364, 2575):

| # | Bytes | Stock name | Source | Nature |
|---|---|---|---|---|
| 1 | `F0 5A 5A` | level-2 key on | S0 1877 | key |
| 2 | `B1 0B 74` | `lpm_aor` | S0 2046 | volatile (AOR, also written by every brightness change) |
| 3 | `F7 03` | `gamma_update_enable` | S0 2033 | volatile (latch) |
| 4 | `F0 A5 A5` | key off | S0 1880 | key |
| - | wait 16.7 ms | `wait_1_frame_in_60hz` | S0 2361 | |
| 5 | `F0 5A 5A` | key on | | key |
| 6 | `BB` + 6 bytes of `lpm_nit_table[HLPM][row]` | `lpm_nit` | S0 1726-1739, 2047 | volatile |
| 7 | `FC 5A 5A` | level-3 key on | S0 1878 | key |
| 8 | `B0 2B` then `FD 03` | `avs_on` at offset 0x2B | S0 2050, 2180 | volatile (AVS) |
| 9 | `FC A5 A5` | level-3 key off | S0 1881 | key |
| 10 | `53 03` (2 nit) or `53 02` | `lpm_mode` | S0 1741-1754, 2048 | volatile (DCS write-control-display) |
| 11 | `F4 EB 23 0B 17 8A` | `avc2_off` | S0 2052 | volatile (VGH/VINT/AVC) |
| 12 | `F7 03` | latch | | volatile |
| 13 | `F0 A5 A5` | key off | | key |

The HLPM rows (`BB` parameters) are:
- 2 nit: `05 0C BB 4F 4F 84`;
- 10 nit: `05 0C 9B EE 4F 84`;
- 30 nit: `05 0C 6B 3F 4F 84`;
- 60 nit: `05 0C 0B 10 4F 04`.

With `SUPPORT_AOD_BL` the row comes from the AOD backlight subdevice, through the
table `{2, 10, 30, 60}` nit
(`s6e3ha8_star_a3_s0_panel_aod_dimming.h`). A change of level while in LPM
sends the whole entry again (`panel_update_doze`, PD 1363).

**Exit**, `star_a3_s0_alpm_exit_cmdtbl` (S0 2579-2592), without `ACTIVE_CLOCK`
and TDMB: `F0 5A 5A; BB 01` (`lpm_off_nit`, HLPM: `lpm_off_table` S0 1756) `;
F4 EB 23 0B 17 CA` (`avc2_on`) `; 53 00` (`exit_alpm`) `; F0 A5 A5`. Then
`backlight_update_status`, the whole normal brightness set (PD 338-385).

**Around it** (PD 425-487, 338-385):
- Stock lowers the panel's 1.6 V buck to 1.5 V in LPM (`BUCK_ALPM_VOLT`,
  `regulator_set_voltage`).
- It raises the ELVSS short-detection current (`DISP_PMIC_SSD`).
- Both are PMIC writes. **We do not do them** (not approved; they are small).

### 1.2 The live clock, self icon and self grid (the panel draws over its frame)

`aod_enter_to_lpm` (DRV 168) runs after the HLPM entry:

1. The self mask off: `F0 5A 5A; 7A 00; F0 A5 A5` (AODP 385-395). Ours has
   been off since boot (1.1b).
2. **`ENTER_AOD_SEQ`** (AODP 604-623): `FC 5A 5A; B0 15; FE 00` ("move setting");
   `76 09 0F 85 A0 0B 90` ("setting for aod porch"); `FC A5 A5`.
3. After a panel power-off (`reset_flag`), the images into side RAM (below).
4. `ICON_GRID_ON` if the self icon or grid is on, and the analog or digital
   clock control if that clock is on and its image is loaded.

**Exit** `aod_exit_from_lpm` (DRV 239):
- `ICON_GRID_OFF` (`7C 00 00`).
- If self-move or the icon was on, `SELF_MOVE_OFF`: `7D 00 00 00 04`, 34 ms,
  `7D 01`.
- `EXIT_AOD_SEQ` (AODP 625-638): `F0 5A 5A; 76 09 0F 00; 77 00 00; 79 00 00;
  F0 A5 A5`.
- Then the self mask back on, which we leave off.

**Images in side RAM.** The AOD app writes them to `/dev/self_display` with a 2-byte
header: `IC` icon, `AC` analog, `DC` digital (DRV 758-800). The kernel sends
them at the next LPM entry after a power-off. Each is `F0 5A 5A`, then `77 00 00`
("init side ram", analog and digital only), then `75 <path>`, then the image as
`4C` (first) and `5C` (continue) DCS long writes of at most 496 bytes (511
rounded down to 16, `panel_dsi_write_mem`, `panel/panel.c` 596-635), then
`F0 A5 A5`. The paths are `75 01` analog, `75 02` digital, `75 08` icon and
`75 10` self mask. The stock compiled-in images (fetched; sizes in bytes) are:

| Image | Size | Format (from the bytes) |
|---|---|---|
| analog | 230400 | 480 x 120 RGBA. Three 40-row bands: hour (x 102-324), minute (102-412), second (94-413), all horizontal, transparent `FF FF FF 00` around them |
| digital | 115200 | Compressed (an RLE-like word stream); not decoded |
| self icon | 1552 | Compressed |
| self mask | 3552 | Compressed |

**Clock registers.** Register `77` is one long parameter block. `B0 n` sets where
the next write's parameters start:
- **Time** (`SET_TIME_SEQ`, AODP 574-595, filled by `copy_set_time_ctrl`, AODC
  96): `77 00 EN DIG HH MM SS MS RATE STEP 00 00 00 00 00` then `78 01`.
  - EN bits: 0x01 analog, 0x02 display on, 0x04 time, 0x10 digital, 0x20 24 h.
  - RATE/STEP by interval: 1 s gives `1E 13`, 500 ms `0F 12`, 200 ms `06 11`,
    100 ms `03 10`.
  - The first update after an entry clears STEP bit 4.
- **Analog place** (`ANALOG_CTRL_SEQ`, AODP 473-495): `B0 09; 77 XH XL YH YL ROT 00
  78 13 78 13 78 13`, then `77 00 EN 00`.
- **Digital place**: `B0 15; 77` + 20 bytes, plus a colon "blink" in `79`.

The time write covers parameters 0-14. The analog place sits at 9-20, so the time
must go **before** the place. Stock's own order (entry, then the app's set-time)
risks clobbering it. Ours always sends time, then place.

**Self icon and grid** (`ICON_GRID_ON`, AODP 417-431): `7C` + 20 bytes, with the
icon position and size and a grid rectangle. One icon image only.

### 1.3 Self-move in AOD

`SELF_MOVE_ON` (AODP 700-705) is `F0; B0 06; 7D` + a 90-byte position table
chosen by the clock interval (AODP 61-272), then `7D 00 EN 00 00 00 00`,
then `F0` off. The position bytes are undocumented. The AOD app re-arms it every
10 minutes (`LiveClockConfiguration` 9, `AODSelfMoveAlarmPart` 11, 33). With
self-move on, each 10-minute tick is a random "big jump" of the whole layout
(`ViewPositionParams` 7-8, `AODPositionControllerFactory` 33). The range is
775 x 32 px at WQHD (944 on WQHD+), and the small zig-zag steps are 1-3 px
(`ZigZagViewPositionControllerImpl` 288-332).

The screen-on self-move (`s6e3ha8_enable_selfmove`, AODP 796-810) is a different
sequence. It is the one that blanked the phone on 2026-11-12 (section 4).

### 1.4 What the SoC does meanwhile

- AOD shown means `Display.STATE_DOZE_SUSPEND` (SystemUI `DozeMachine` 125-147).
  DSIM goes to `DOZE_SUSPEND` (DSIM 969):
  - the panel's `doze_suspend` (the AOD side does nothing, DRV `aod_doze_suspend`);
  - `dsim_reg_stop`;
  - the IRQ off, the D-PHY powered off;
  - the DSIM and DECON runtime-suspended (their power domain off);
  - **panel power kept**: `dsim_set_panel_power(0)` is only for `DSIM_STATE_OFF`
    (DSIM 902-935).
- So nothing scans out. The panel refreshes from its own frame memory in HLPM,
  and the AP is free to suspend.
- The rails that stay on are the panel's own: S2DOS05 `vdd1` 1.6 V (1.5 V in
  stock LPM), `vdd2` 1.8 V, `vdd5` 3.0 V and ELVSS. ztOS never switches them
  (`power/RAIL-MAP.md` E). So **no rail change is needed for us**.
- To redraw, the AOD takes a draw wake lock (100 ms to 1 s). PowerManagerService
  then turns `DOZE_SUSPEND` into `DOZE` (DSIM 873: the link up again, the panel
  still in HLPM), and a frame goes out.

### 1.5 When stock wakes the AP

- Without the live clock the AP wakes every minute (`AOD_TIME_TICK`, RTC_WAKEUP,
  `AODTimeTickTimerEx` 164).
- The S9 has the live clock. `floating_feature.xml`: `activeclock=2` means version 2
  ("STAR"), and self-move is supported.
- So after the first minutes the per-minute alarm is turned off. The AP wakes:
  - on the hour (`AODEveryHourAlarmPart` 24-36);
  - every 10 minutes for the self-move jump;
  - at notifications, charging and battery changes (their own events).
- No time resync timer runs on the S9 (`LIVECLOCK_TIMESYNC_ISSUE` false).
- The partial-HLPM GRAM scan is version 6 only, so not the S9.

### 1.6 Brightness levels

The panel has four HLPM levels: 2, 10, 30 and 60 nit. With the two-level
framework path, which the S9 uses most likely (the
`config_aodBrightnessValues` overlay was not in the dump), stock picks HLPM 60
nit above 2 lux and 2 nit at 2 lux or below (`AODUiController` 1375-1410). The
four-level path uses cut-offs at 2, 50 and 100 lux.

## 2. Our design

**The panel holds the picture and draws the clock. The SoC sleeps, and wakes
only to change what the picture says.**

1. The shell draws the AOD frame as today: date, battery, waiting apps, the force
   mark, at one of its 9 places. It leaves out the time and draws a thin dial
   instead (12 short marks, grey).
2. The kernel puts the panel in HLPM (`aod_lpm`, 2 or 10 nit) while that frame
   is still being flipped normally (stage a).
3. The shell parks the output with `aod_keep` (stage b). The pipeline reads
   disabled, the trigger stays masked, and the panel keeps its frame in HLPM.
   zrn-sleepd may sleep.
4. With the pipeline parked, the kernel sets up the live clock: the
   live-clock entry, our hand sprite into side RAM, the time, and the analog
   place at the dial's centre (`aod_clock`, stage c). The panel turns the hands
   by itself, once a second.
5. zrn-sleepd sleeps in `deep` mode. It wakes on the power key, a notification
   (modem, push window), the charger, or its RTC. The RTC wake is at the next
   10-minute place change at the latest. At a wake:
   - the shell resyncs the time (`aod_clock time ...`);
   - if the place, battery or waiting apps changed, it unparks, draws, flips
     and parks again. The kernel takes the live clock out (`76 09 0F 00`)
     before any frame goes, and the shell sets it up again after parking.
6. On a real wake (power key, double tap, force press), the shell enables the
   output, writes `aod_lpm 0` (stock's exit plus the whole brightness set),
   and the lock screen comes up.

**Burn-in** (the owner's rules: thin, dim, moving):
- Kept:
  - everything moves together to the next of 9 places every 10 minutes;
  - thin grey strokes, outline-only icons.
- Changed:
  - The **time is now analogue hands**, drawn by the panel. A hand never stands
    still on a pixel for more than a minute. Our hour and minute hands are 3 px
    lines (stock's are 14 and 12 px), grey `A0` on transparent, with no second hand.
  - The **digital time is gone from the AOD while the SoC sleeps**. Our
    themes (Metro, Compact, Digital, Stacked, Flip) would need the panel's
    digital sprite, which is compressed in a format we have not decoded.
    Analogue is the one theme the panel can draw from an image we make. The
    other themes remain for "only while charging", where nothing needs to sleep.
  - **Brightness**: the panel's own 2 or 10 nit HLPM replaces the shell's six
    levels. 2 nit in the dark (< 5 lux, as stock: < 2) and 10 nit above. We
    stay at most 10 nit by default, where stock goes to 60.
  - The **panel's AOD self-move is not used** for now. The 10-minute places
    give the movement. Self-move's bytes are undocumented, and the same
    register family blanked the phone (section 4).

What it should cost: section 5 (measurements).

## 3. Implementation (stages)

### Kernel (`kernel/patch-decon-aod.py`, `kernel/patch-decon-lpm.py`)

- `aod_keep` (module parameter) makes the next blank leave the panel lit, with
  the trigger masked (unchanged from stage 2 of `README.md`).
- On the DECON device, next to `panel_cmd`:
  - `aod_lpm` (0-4) runs the HLPM entry or exit of table 1.1 exactly.
    Brightness writes are held off while in LPM.
  - `aod_sprite` takes the 230400-byte hand image.
  - `aod_clock` takes `analog X Y HH MM SS [24]`, `time HH MM SS [24]` or `off`.
- Every sequence goes between two frames with the trigger off, and **closes F0
  and FC on every path**. The bytes are compiled in; these files take numbers,
  never bytes.
- A panel sleep clears all of it, since sleep-out reloads the registers and the
  side RAM is lost.
- An unpark sends the live clock's exit before any frame.

### What is sent: the complete list

Only these, all from the tables above:

- **Keys:** `F0 5A 5A`, `F0 A5 A5`, `FC 5A 5A`, `FC A5 A5`.
- **Offsets:** `B0 2B`, `B0 15`, `B0 09`.
- **HLPM entry:** `B1 0B 74`, `F7 03`, `BB 05 0C ..`, `FD 03`, `53 02/03`,
  `F4 EB 23 0B 17 8A`.
- **HLPM exit:** `BB 01`, `F4 EB 23 0B 17 CA`, `53 00`.
- **Live clock entry and exit:** `7A 00`, `FE 00`, `76 09 0F 85 A0 0B 90`,
  `76 09 0F 00`, `77 00 00`, `79 00 00`.
- **Side RAM:** `75 01`, `4C`/`5C` + data.
- **Clock:** `77` time (16 bytes), `78 01`, `77` place (13 bytes), `77 00 EN 00`.

**Never sent:** `F1`, `C0`, `C1` (the flash/MTP write path), the `9F` level-1 key
(not needed), and `7C`/`7D` (self icon, grid and self-move). The self mask
enable is not sent either.

## 4. Incident: the screen-on self-move blank (2026-11-12, `feature/burn-in`)

At 11:58:35 CST the burn-in trial sent stock's screen-on self-move enable in
normal mode:

    FC 5A 5A; F0 5A 5A; B0 15; FE 00; 76 09 0F 85 A0 0B 90; 77 ..; 7C ..; 7D ..; F0 A5 A5; FC A5 A5

What happened:
- About 1.8 minutes later s9p-decon logged "flip did not latch within 50 ms",
  then "vblank wait timed out".
- DECON's frames stopped completing, and the panel stayed dark while the shell
  ran.
- A screen off/on did not bring it back; only a power-off did. The phone then
  dropped off the network.
- The trial's own dmesg check sat behind a blocking screenshot, so its disable
  never ran.
- The suspect is `76 .. 85 A0 0B 90` (the AOD porch/timing) together with `FE`.
  It changes the panel's timing, which our DECON's TE-driven flips do not survive.

What we do differently:
- **`76`/`FE` are only sent with the pipeline parked** (no frames at all). Any
  unpark first sends stock's `76 09 0F 00` exit.
- A separate watcher process (`panel/lpm-watch.sh`) follows `dmesg -w`. At the
  first "flip did not latch", "vblank wait timed out", "did not take DCS" or
  "shadow update stuck", it sends the exit.
- It takes no screenshots and does no blocking work.
- A deadman (`panel/deadman.sh`) reboots by sysrq when the host's heartbeat is 8
  minutes old.

## 5. Results

See the stage log below (filled in as each stage is measured).
