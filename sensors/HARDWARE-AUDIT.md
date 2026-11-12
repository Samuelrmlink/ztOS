# Galaxy S9 hardware in ztOS: what is used (ROADMAP 8.25)

Audit of 12 November 2026, on the SM-G960F, branch `feature/sensors`.

**Evidence.** Each status comes from one or more of these:

- **The phone's `/sys`**, read-only that night:
  - input devices: `gpio-keys`, `s6sy761`, `s6sy761-force` and `zrn-haptic` (FF);
  - LEDs: `red`, `green` and `blue:status`, plus `white:torch`;
  - power supplies: `max170xx_battery` and the `max77705-*` supplies, plus `p9320-wireless`;
  - I2C clients: only buses 0, 1, 2, 96, 97 and 98. No `/sys/class/rfkill` and no IIO.
- **Samsung's starlte kernel**, Android 10 (exynos-linux-stable):
  - `exynos9810-starlte_eur_open_26.dts` and `_common.dtsi`, with the motor, sound and MST dtsi;
  - `exynos9810_defconfig`;
  - `drivers/sensorhub/brcm/` (`sensor_list.h`, `ssp.h`, `ssp_data.c`, `ssp_sysfs.c`, `factory/`).
- **This repository's own READMEs.**

**Status words:**

- **working**: used by ztOS today.
- **partial**: some of it works.
- **found, unused**: the hardware answers or is reachable, and nothing uses it.
- **missing**: no driver or path at all.

**Sizes:**

- **S**: under a day.
- **M**: a few days.
- **L**: a week or more.
- **?**: unknown.

## The table

### Sensor hub (BCM4773)

Every sensor in this section sits behind the hub: `zrn-bbd`, then lhd, then `ztoscsf-sensord`.

| Feature | Status | Evidence | What it would take | Size |
|---|---|---|---|---|
| Light / colour (TMD4906, ALS) | working | Auto brightness and OpenTone; `/run/ztoscsf-sensors/light` (sensors/README.md) | - | - |
| Proximity (TMD4906) | **working, new** (8.24): host-tested, not yet on the phone | Before 8.24 it was never on, and its 3-byte record was parsed as 2. Now it runs in calls: `prox_blank` (README, "Proximity in calls") | A test on the phone: a simulated call and a hand over the top bezel. Then the owner's real call | S |
| Accelerometer (LSM6DSL) | found, unused | Delivers readings (README); only `watch accel` and ZrnDevTest read it | Feeds auto rotation and step counting, below | - |
| Auto rotation | **built, host-tested** (8.33, `feature/rotation-cover`): sensord's `orient on`, the shell's turn behind Settings' Auto-rotate (off by default). Was: missing | The shell has no output transform. `shade_rotation` exists in `struct zt_server`, but nothing rotates. The hub has a virtual `AUTO_ROTATION_SENSOR` (48, 1 byte, on-change; `MSG2SSP_AUTO_ROTATION_ORIENTATION` 0x9A) | **Sensor side (S):** add sensor 48 (or work it out from accel) and publish `orientation 0..3`. **Shell side (L):** a wlroots output transform, a re-layout of Start, the status bar, the lock and the shade, app sizes and touch mapping, a lock tile, and per-app opt-out (Waydroid) | S + L |
| Gyroscope (LSM6DSL) | found, unused | Delivers readings (README) | Nothing in ztOS needs it alone. Useful for game rotation vectors and Waydroid sensors | - |
| Barometer (LPS22H) | found, unused. **Readings now correct** | Sensor 5 is an s32 count (4096/hPa) plus an s16 in centi-°C (`ssp.h`, `pressure_lps22h.c`). The daemon used to print three 16-bit numbers. With `watch pressure`, `/run/ztoscsf-sensors/pressure` now reads `hPa raw °C`. The factory offset is in EFS, which we don't read | **Quick win (S):** a `pressure on\|off` ctl line that keeps it at 1 s while a reader (weather or altitude app, ZrnDevTest) wants it. Still to check on the phone: that it reports at all; the README's "nothing in 3 s" predates the size fix | S |
| Magnetometer / compass (AK0991x-class, `MAGNETIC_COMMON`) | found, silent | Gave nothing in a 3 s test (README). Samsung's driver sends at hub init the DT's 27-byte `ssp-mag-array` as `MSG2SSP_AP_SET_MAGNETIC_STATIC_MATRIX` (0xFD), plus the HW offset from EFS (0xF5). We send neither. Records: geomagnetic 8 bytes, uncalibrated 13 | Send the static matrix (SSP through the daemon, from the stock DT values), add sensor 4 or 2, parse it, then hard- and soft-iron calibration in user space and a compass in Maps or Waydroid | M |
| Step counter / detector, significant motion, tilt, pickup, wake-up motion, move detector, game and rotation vectors | found, unused | Hub virtual sensors 13, 14, 16, 17, 18, 26, 27, 36, 37 (`sensor_list.h`; sizes 1, 1, 17, 17, 4, 1, 1, 1, 1). Which ones this firmware actually has is in its `0xF4` sensor bitmap (`zrn-ssp.py info`), not yet recorded | Read the bitmap. Then a `steps` file (sensor 18, 4-byte count) and pickup-to-wake for the lock screen (sensor 27), with a per-sensor size table in the daemon | S each |
| Hub thermistors, light flicker, light CCT, pocket mode | found, unused | Sensors 34, 31, 32, 43 in `sensor_list.h` | Not needed | - |

### Other sensors

| Feature | Status | Evidence | What it would take | Size |
|---|---|---|---|---|
| Hall sensor (flip cover) | **built, not loaded** (8.34): `sensors/hall/zrn-hall.ko`, the shell's cover handling; loading waits for the owner's approval of the pin change. Stock's pull is down, not up (`COVERS.md`). Was: missing | Stock: `hall` on `gpa3-6` (pull-up, `CONFIG_HALL_EVENT_REVERSE`) and `certify_hall` on `gpa0-2`. Samsung also tells the hub (`MSG2SSP_HALL_IC_ON_OFF` 0x96) to compensate the magnetometer. Mainline `gpio-keys` has no switch (EV=3) | A `gpio-keys` child with `linux,input-type = <EV_SW>`, `SW_LID`, `gpa3-6` active-low **in the DT** (board-details-in-DT rule). Either `kernel/make-dts.py` and a BOOT flash, or a runtime overlay applied by a module as `zrn-modem` does, with no BOOT change. Then the shell reads SW_LID: screen off on close, on at open | S (DT or overlay) + S (shell) |
| Grip / SAR sensor (SX9320) | found in stock DT, unused | `sx9320` at 0x28 on `hsi2c_11` (104C0000). That bus isn't in our DT | It's for the modem's SAR back-off, not a user feature. Leave it | - |
| Heart rate / SpO2 (MAX86915) | other agent | `hrmsensor@57` on `hsi2c_20` | **ROADMAP 8.26** (`feature/hrm`) | - |
| Iris IR camera and LED, fingerprint | other agent | - | **ROADMAP 8.22 / 8.23** | - |

### Radios

| Feature | Status | Evidence | What it would take | Size |
|---|---|---|---|---|
| NFC (`sec-nfc` 0x27, eSE `ese_p3`) | other agent | `hsi2c_20`, `spi_7` | **ROADMAP 8.28** (`research/nfc`) | - |
| MST (Samsung Pay) | other agent | `sec-mst`: gpg3-4, gph0-7, gph0-6 | **ROADMAP 8.28** | - |
| GNSS (BCM4773) | other agent | `ztos-gnss-gate` running | **ROADMAP 8.29** (`feature/gnss`) | - |
| FM radio (Richwave RTC6213N) | missing (present on this G960F) | `rtcfmradio@64` on `hsi2c_11`, irq gpa2-1, LNA gph1-3, `VDD_FM_2P8` (LDO45, always-on); `CONFIG_RADIO_RTC6213N=y` in stock. The headphone cable is the antenna | `hsi2c_11` in the DT, a port of Samsung's GPL V4L2 driver (no mainline driver), FM audio into the CS47L93 (an analogue input route), and the headphone jack first | L |

### Audio

| Feature | Status | Evidence | What it would take | Size |
|---|---|---|---|---|
| Stereo speakers (2 × MAX98512) | working | 0x39 left, 0x3a right; the second speaker is the earpiece in speaker mode (audio/README.md) | Speaker protection (DSM) for full volume: audio/DSM.md | M |
| Earpiece | working in calls | MAX98512 in receiver mode; heard on real calls 2026-10-30 (audio/CALL-AUDIO.md) | - | - |
| Microphones | partial | `capture=Y`, `pcmC0D2c` on the phone. The call microphone was fixed after 2026-10-30 (codec pins after sleep) and still needs confirming on a real call | Owner's call test | S |
| Headphone jack | missing | CS47L93 (madera) HPOUT1 and jack detect. snd-zrn-abox powers the codec only for the mics; no output or jack detection (audio/README.md "Not done") | The jack detect IRQ and accessory detection on the codec, HPOUT1 routing, and a PipeWire port switch | M-L |

### Lights, haptics and power

| Feature | Status | Evidence | What it would take | Size |
|---|---|---|---|---|
| Rear flash as a torch (S2MPB02) | working | `white:torch` from `leds/zrn-flash.ko`, and the action centre torch tile (`TORCH_PATH`, level 9 of 15) | - | - |
| Notification LED | working | `red`, `green`, `blue:status`, `zrn-ledd` | - | - |
| Vibration motor | working | `zrn-haptic` (MAX77705 haptic, FF input) | - | - |
| Wireless charging (P9320) | working | `p9320-wireless` power supply, driver bound on bus 2 | - | - |

## Quick wins after proximity

1. **Barometer (S).** The record is parsed right now, and `watch pressure` fills the file. A daemon command to keep it on is about 20 lines. First check on the phone that it reports at all.
2. **Hub sensor bitmap (S).** Run `zrn-ssp.py info` once with sensord stopped, inside `ztlock`, at a time the GNSS agent isn't using the hub. Record which virtual sensors this firmware has. Note that the tool still sends `0xD1`, which the README says made the firmware assert, so drop that line first.
3. **Orientation file (S).** Hub sensor 48, published as `/run/ztoscsf-sensors/orientation`. That is all the sensor side of auto rotation needs. The shell's rotation is the large part.
4. **Hall sensor (S).** A `gpio-keys` switch from a runtime DT overlay (no BOOT flash), then a few lines in the shell.

Not built now: everything except proximity for calls and the pressure and proximity record fixes.
