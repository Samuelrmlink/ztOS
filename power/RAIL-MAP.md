# Power rails: what ztOS switches, what stock does, what to change (ROADMAP 8.31)

2026-11-12. Every claim carries a label:

- **obs**: observed on this phone.
- **src**: from source, meaning Samsung's kernel (LineageOS `android_kernel_samsung_exynos9810`, branch lineage-20, the tree `third_party/vendor-kernel` pins), the stock rev26 device tree (`dtb11`, hw_rev 0x1a, extracted from `stock/BOOT.img`) or our own drivers.
- **inf**: inferred.

No register, GPIO or regulator was written for this work.

## Contents

1. [Sources and tools](#sources-and-tools)
2. [How the rails are controlled, in short](#how-the-rails-are-controlled-in-short)
3. [Rail map](#rail-map) (A: S2MPS18, B: S2MPB02, C: S2MPB03, D: MAX77705, E: S2DOS05, F: GPIO switches)
4. [Samsung's policy, from source](#samsungs-policy-from-source)
5. [Gaps and plan](#gaps-and-plan), ordered by expected gain
6. [Register writes for the top three fixes](#register-writes-for-the-top-three-fixes) (for approval)
7. [Open reads](#open-reads)

## Sources and tools

| What | Where |
|---|---|
| Stock DT, rev26 | `python3 -I camera/iris/stock-dt.py stock/BOOT.img OUT` (on `research/iris`), then `OUT/dtb11_2441800.txt` |
| Every stock regulator: flags and consumers | `power/rails/stock-regulators.py OUT/dtb11_2441800.txt` |
| ztOS's regulators and their consumers (read only) | `ssh … 'sh -s' < power/rails/rail-snapshot.sh` |
| S2MPS18 LDO modes now | `cat /sys/class/sec/sensorhub/ldo`. This is zrn-bbd's existing read of LDO1-45 through ACPM. |
| Compare those modes with stock | `power/rails/ldo-compare.py ldo-now.txt regs.tsv` |
| Stock S2MPS18 registers | `sensors/stock-state/s2mps18-regs.txt` (stock kernel in TWRP, **modem not running**) |
| An early ztOS boot | `sensors/stock-state/s2mps18-regs-mainline.txt` |
| Stock regulator use counts | `sensors/stock-state/regulator_summary.txt` (TWRP) |

The live reads were taken 2026-11-12 00:13-00:20 CST. Kernel `7.1.0-zboot-wd-dp6 #61`, up 40 min, on the charger. The backlight was 0 with AOD showing, so the phone was awake (`zrn-sleepd`: `why=aod showing`, `sleeps=0`). That is today's "idle with the screen off": the phone is awake and the SoC never sleeps. **obs**

## How the rails are controlled, in short

- **ztOS's regulator framework sees five rails** (**obs**: `rail-snapshot.sh`): the fixed `ufs-vcc`, `vbus-stub`, `tsp_io` and `tsp_avdd`, and `max77705-otg`. `tsp_io` and `tsp_avdd` are fixed always-on stand-ins. They do not reach L35 or L43.
- **The other PMIC outputs are handled outside Linux's regulator framework.** They are switched by our own modules writing registers (ACPM for the S2MPS18, bit-banged I2C for the S2MPB02 and S2MPB03), or left as the bootloader, ACPM or the CP set them. Nothing switches a rail when its device goes idle unless a module does it itself.
- **Stock Linux turns unclaimed rails off; ztOS never does.** Stock registers every rail with the regulator core. About 30 s after boot the core switches off every rail that is enabled, unclaimed and not always-on (`regulator_init_complete`, full DT constraints). ztOS has no such step, so whatever the bootloader left on stays on. **src, inf**
- **S2MPS18 enable modes**, bits 7:6 of each `LnCTRL`/`BnCTRL` register (`s2mps18_of_map_mode`, `SEC_OPMODE_*`) **src**:
  - 0: off;
  - 1: on while PWREN is high, so off in SoC sleep;
  - 2: on while PWREN_MIF is high, so off when the memory interface goes down;
  - 3: always on.

  DT `regulator-initial-mode` is that number. Modes 1 and 2 only matter once the SoC really sleeps, and ztOS does not reach SoC sleep at all today (above). They are a later gain, not today's.

## Rail map

Column key:

- **ztOS switch**: who switches the rail in ztOS.
- **ztOS idle**: its state now, with the screen off and no app open.
- **Stock**: what stock does with it.
- **Stock DT modes** (from `regulator-initial-mode`): `AO1` is always-on with mode 1, so it drops in SoC sleep. `AO2` drops when MIF goes down. `AO3` stays on in sleep.

### A. S2MPS18 (main PMIC, behind the APM "speedy" bus; reached only through ACPM channel 2)

#### Bucks

The registers are `BnCTRL` (mode) and `BnOUT1` (voltage); see `s2mps18-private.h`.

The ztOS-idle column for bucks comes from the early ztOS dump, because the `ldo` file does not cover bucks. The MIF level is confirmed now by its PLL (`power/always-on`).

| Output (ctrl/out) | Voltage | Feeds (DT) | ztOS switch | ztOS idle | Stock |
|---|---|---|---|---|---|
| BUCK1 (0x1c/0x1d) | 0.45-0.95 V | `vdd_mif`, the memory interface | ACPM DVFS (firmware). No Linux requester. | On, mode 2. **0x43 = 0.719 V (MIF 1794 MHz)** **obs** | AO2. **0x2d = 0.581 V** at rest (TWRP) **obs**. devfreq drops MIF to 421 MHz when idle and in suspend **src** |
| BUCK2 (0x1f/0x20) | 0.45-1.3 V | `vdd_cpucl1` (big cores) | `s9p-cpufreq` | On, mode 1 | AO1 |
| BUCK3 (0x22/0x23) | 0.45-1.3 V | `vdd_cpucl0` (little cores) | `s9p-cpufreq` | On, mode 1 | AO1 |
| BUCK4 (0x25/0x26) | 0.45-0.95 V | `vdd_int` | ACPM, no requester | On. **0x4b = 0.769 V** (INT 534 MHz) | AO2. **0x3a = 0.663 V**. devfreq: INT 107 MHz in suspend **src** |
| BUCK5 (0x28/0x29) | 0.45-0.95 V | `vdd_cam` | ACPM `dvfs_cam`, requested by zrn-cam at 690 MHz and **never released** | 0.606 V before any camera use. Raised after a camera is used **inf** | AO2. Released when the camera closes **src** |
| BUCK6 (0x2a/0x2b) | 0.45-0.95 V | `vdd_g3d` | `s9p-g3d` (GPU genpd) | On. GPU runtime-suspended **obs** | AO2 |
| BUCK7 (0x2c/0x2d) | 0.45-1.3 V | `vdd_cpucl1_m` | never | On, mode 1 | AO1 |
| BUCK8 (0x2e) | 0.9-1.3 V | `vdd2_mem` (LPDDR4 VDD2) | never | On, mode 3 | AO3 |
| BUCK9 (0x30) | 0.45-0.95 V | `vddq_mem` | never | On, mode 2 | AO2 |
| BUCK10-13 (0x32-0x39) | 0.85 / 1.1 / 1.3 / 2.0 V | pre-regulators for the LDO groups (`vdd_lldo1-3`, `vdd_mldo`) | never | On, mode 3 | AO3 |

#### LDOs

The register is `LnCTRL` = 0x49 + n (L1 is 0x49; L2 is 0x4a/0x4b). Every row is **obs** (`ldo` read, 00:14 and 00:20, identical) against the TWRP dump.

`*` marks a mode that differs from stock. One caveat on every `*` row: the TWRP dump was taken with **the modem off**, and ztOS's modem is running.

| LDO (reg) | Voltage | Feeds (stock DT) | ztOS switch | ztOS idle (ctrl, mode) | Stock |
|---|---|---|---|---|---|
| L1 (0x49) | 0.75 V | SoC (no named consumer) | never | c4, 3 | AO3 |
| L2 (0x4a) | 1.8-3.3 V | `vqmmc`, SD card I/O | never | 28, **off** | on with a card (dwmmc2) |
| L3 (0x4c) | 1.8 V | SoC | never | ec, 3 | AO3 |
| L4-L14 (0x4d-0x57) | 0.6-3.0 V | SoC analog and I/O (`vdd_ldo4`..`vdd_ldo14`, no named consumer) | **not ztOS code** (below) | **cc-f0, mode 3** `*` | **AO1: off in SoC sleep** (stock 4c-70, mode 1) |
| L15, L16 (0x58-59) | (not in DT) | firmware/CP-managed (the vendor driver leaves them out) | never | c4/d0, 3 | mode 3 |
| L17-L19 (0x5a-5c) | 0.8 / 1.2 / 1.8 V (not in DT) | firmware/CP | **not ztOS code** | **48/68/6c, mode 1** `*` | mode 3 |
| L20-L22 (0x5d-5f) | (not in DT) | firmware/CP | never | off | off |
| L23 (0x60) | (not in DT) | firmware/CP | never | 6c, 1 | mode 1 |
| L24-L28 (0x61-65) | (not in DT) | firmware/CP | never | mode 2 | mode 2 |
| L29 (0x66) | (not in DT) | firmware/CP | never | off | off |
| L30 (0x67) | 0.6 V | SoC | **not ztOS code** | **d0, mode 3** `*` | AO1 (50, mode 1) |
| L31 (0x68) | 2.8 V | `vmmc`, SD card | never | off | on with a card |
| L32 (0x69) | 2.8 V | SoC (no named consumer) | **not ztOS code** (it was already mode 3 in the early dump) | **e8, mode 3** `*` | AO2 (a8, mode 2) |
| L33 (0x6a) | 1.8 V | `vdd_pmic_codec_1p8`: codec AVDD/DBVDD1/CPVDD1 | snd-zrn-abox (checks it) | ec, 3 | AO3 |
| L34 (0x6b) | 1.2 V | `vdd_codec_1p2`: codec DCVDD/CPVDD2 | **snd-zrn-abox switches it on at load** | e8, 3, always | Taken by the codec driver; it stays on while the codec is bound (stock use count 1) **obs (TWRP)** |
| L35 (0x6c) | 1.8 V | `tsp_io`, touch DVDD | s9p-acpm (power-cycle at boot) | ec, 3 | Boot-on. **The touch driver cuts it at screen off** unless low-power mode is on **src** |
| L36 (0x6d) | (not in DT) | - | never | off | off |
| L37 (0x6e) | 1.8 V | `vdd_mcu_1p8`, the sensor hub (BCM4773) | zrn-bbd (on; it refuses "off") | ec, 3 | AO3 (lhd's off-on is a no-op on stock) |
| L38 (0x6f) | 1.8 V | `VDD_NFC_1P8`: NFC PVDD, HRM I2C pull-up | never | **2c, off** `*` | mode 2. **On while NFC is enabled** (sec_nfc probe; stock use count 2) |
| L39-L41 (0x70-72) | 1.05 / 2.8 / 1.8 V | iris camera (5F1) | zrn-cam, only with `iris_allowed=1` (never set) | off | Off; on only while iris runs |
| L42 (0x73) | 2.05 V | SoC/board (no named consumer) | **not ztOS code** | **20, off** `*` | AO2 (8a, mode 2) |
| L43 (0x74) | 3.0 V | `tsp_avdd`, touch AVDD | s9p-acpm (power-cycle) | **fc = 3.3 V**, mode 3 (stock **f0 = 3.0 V**; DT max 3.0 V) | Like L35 |
| L44 (0x75) | 1.8 V | `VDD_ESE_1P8`, eSE | never | off | On only during an eSE session (`ese_p3`) |
| L45 (0x76) | (not in DT) | - | never | off | off |

**Who changed L4-L14, L17-L19, L30, L42 and L43?** The early ztOS dump still matched stock for every one of these. No ztOS writer exists:

- A search of the repo found none: the only `s9p_acpm_pmic_update/write` callers touch L33, L34, L35, L37, L39-L43 enable bits, BUCK2, BUCK3 and BUCK6.
- The deployed kernel's own sources were checked the same way.
- zrn-bbd's `ldo`/`pmic` store functions log every write, and none appears this boot.

Leading hypothesis (**inf**): the CP boot or the APM firmware rewrites them once the modem runs. The modem was off in TWRP and not yet up in the early dump. This boot has had no SoC sleep (`sleeps=0`), so a sleep entry did not do it either. Until this is settled, **treat the TWRP column as "stock, modem off", not as stock Android's working state.**

### B. S2MPB02 (camera/sub-PMIC, I2C 0x59)

Stock reaches it on `hsi2c@10920000`; ztOS bit-bangs bus 97 (gpp6-0/1) in zrn-flash.

Registers (`s2mpb02-private.h`) **src**:

- BUCK1 0x0b/0x0c, BUCK2 0x0d/0x0e, buck-boost 0x0f/0x10;
- LDOn CTRL = 0x1d+n: bit 7 enables, bits 6:0 set the voltage;
- buck enable is bits 7:6;
- flash: 0x12 CTRL1 (FLED1), 0x13 CTRL2 (FLED2).

Stock regulator use counts in TWRP are 0 for every output, but `regulator_summary` does not show always-on enables. **obs**

There is no read path for this chip on the phone today that loads nothing, so ztOS-idle is from the drivers' code and logs. Each row says so.

| Output (reg) | Voltage | Feeds (stock DT name) | ztOS switch | ztOS idle | Stock |
|---|---|---|---|---|---|
| BUCK1 (0x0b) | 1.3 V | `VDD_EXT_1P3_PB03`, the S2MPB03's input | never | on as booted **inf** | AO, boot-on |
| BUCK2 (0x0d) | 1.3 V | `VDD_EXT_1P3_PB02`, the front-camera core supply | zrn-cam (front); at close it **restores the boot value 0x58** | **0x58: bits 7:6 = 01, which the vendor's `is_enabled` counts as on** **src**; whether 01 really drives the output is not verified **inf** | Not always-on: switched off by the unused-rail cleanup and enabled only by the front camera **src, inf** |
| BB (0x0f) | 2.6-4.0 V (3.5 V) | `VREG_BOOST_BYP`, the S2MPB02 LDO input stage | never | on as booted | AO, boot-on |
| L1, L2, L4 (0x1e, 0x1f, 0x21) | 0.9 / 1.0 / 1.2 V | unused on G960 (no consumer) | never | off **inf** | off |
| L3 (0x20) | 1.05 V | `VDDD_1.05V_SUB` (unused on single-camera G960) | never | off **inf** | off |
| L5, L6 (0x22, 0x23) | 1.05 / 1.8 V | front camera (`VT`) core/IO | zrn-cam | off after close **src** | camera only |
| L7 (0x24) | 1.8 V | `VDDIO_1.8V_SUB` | never | off **inf** | camera only |
| L8 (0x25) | 1.8 V | `HRM_1.8V_AP`, HRM (MAX86915) | the HRM experiment only (`feature/hrm`, restores the value it read) | off **obs (TWRP) / inf** | On only while HRM is enabled (`hrm_power_ctrl`) |
| L9 (0x26) | 1.8 V | `S2MPB02_LDO9`; the sensor hub fails without it | **zrn-flash at load** (`30 -> b0`, **obs** in the log) | on | AO |
| L10, L15 (0x27, 0x2c) | 1.8 / 2.8 V | OIS digital | zrn-cam (rear) | off after close | camera only |
| L11, L12 (0x28, 0x29) | 2.8 V | SUB analog/AF (unused on G960) | never | off **inf** | off |
| L13, L16 (0x2a, 0x2d) | 2.8 V | front camera analog / AF | zrn-cam | off after close | camera only |
| L14 (0x2b) | 3.3 V | `VDDAF_COMMON_CAM`, the rear AF | zrn-cam | off after close | camera only |
| L17 (0x2e) | 1.8 V | `VDD_GRIP_1P8`, the grip sensor supply | never | on as booted **inf** | AO, boot-on |
| L18 (0x2f) | 3.3 V | `VDD_PROXY_LED_3P3`, the proximity LED | never | as booted (unknown) | No stock consumer in DT. Not always-on, so the unused-rail cleanup turns it off if the bootloader left it on **inf** |
| FLED1 (0x12/0x14) | - | rear flash/torch | zrn-flash | off | on demand |
| FLED2 (0x13/0x16-0x1b) | - | iris IR LED | never | off | iris only |

### C. S2MPB03 (rear camera PMIC, I2C 0x56, bit-banged gpm0-0/gpm1-0)

Register: `LDOn` CTRL = 0x02+n; bit 7 enables, bits 5:0 set the voltage. **src**: `camera/driver/REGISTERS.md` (on `feature/camera`).

| Output (reg) | Voltage | Feeds | ztOS switch | ztOS idle | Stock |
|---|---|---|---|---|---|
| L1 (0x03) | 1.025 V | `VDDD_1.0V_CAM`, rear core | zrn-cam | off after close | rear camera only |
| L2 (0x04) | 1.1 V | `VDDQ_1.1V_CAM` | zrn-cam | off | rear only |
| L3 (0x05) | 1.8 V | `VDDIO_1.8V_CAM` | zrn-cam | off | rear only |
| L4 (0x06) | 1.025 V | `VDDD_RET_1.0V_CAM` | zrn-cam | off | rear only |
| L5 (0x07) | 2.15 V | `VDDA_2.15V_CAM` | zrn-cam | off | rear only |
| L6 (0x08) | 2.95 V | `VDDA_2.95V_CAM` | zrn-cam | off | rear only |
| L7 (0x09) | 2.8 V | `VDD_VM_2.8V_OIS`, OIS motor | zrn-cam | off | rear only |

The whole chip is fed from S2MPB02 BUCK1 (always on in both).

### D. MAX77705 (charger, USB-C, haptic)

| Output (reg) | Voltage | Feeds | ztOS switch | ztOS idle | Stock |
|---|---|---|---|---|---|
| SAFEOUT1 (CHG 0xc6) | 3.3-4.9 V | USB PHY supply | never | boot-on, as booted **inf** | boot-on. The muic driver switches it with the USB path **inf** |
| SAFEOUT2 (CHG 0xc6) | 3.3-4.9 V | (unused) | never | as booted | off |
| CHG / OTG boost | 5 V | `max77705-otg`, VBUS out | max77705 charger and OTG patches | **disabled** **obs** | on demand |
| Haptic driver (PMIC 0x02 BIASEN, 0x10 MCONFIG) | - | LRA motor | zrn_haptic: BIASEN set at probe, MCONFIG MEN cleared `idle_ms` = **300** ms after a pulse **obs** | bias on, motor off | **The same: BIASEN always on, MEN only while playing** **src** (`max77705_haptic.c`) |

### E. S2DOS05 (display PMIC, I2C 0x60)

Display only: buck1 `vdd1` 1.6 V, ldo1 `vdd2` 1.8 V, ldo4 `vdd5` 3.0 V (panel, boot-on), ldo2 and ldo3 unused, ELVSS.

- ztOS: never switched. The panel is put to sleep by command (`panel_sleep`), and the rails stay on as booted.
- Stock: the panel driver holds them through AOD; they are cut only in full panel power-off. **src (DT consumers `panel_drv`)**
- This is a panel item (`power/README.md`: the panel draws 157 mA when not in sleep mode), not part of this map's gaps.

### F. GPIO power switches

| Switch | Pin | Feeds | ztOS switch | ztOS idle (obs, gpio debugfs) | Stock |
|---|---|---|---|---|---|
| UFS VCC | gpd0-2 (`ufs-vcc`) | UFS flash VCC | kernel fixed regulator | high | Same. UFS links runtime-hibernate; ours stay active (`power/always-on`) |
| WLAN_REG_ON | gpg0-5 | BCM4361 | brcmfmac | high | high while Wi-Fi is on |
| BT_EN | expander 96-0022 line 7 | BT | zrn-bt | high | high while BT is on |
| GPS_PWR_EN | gpg0-2 | BCM4773 (hub and GNSS) | zrn-bbd/lhd | high | Driven by lhd. The pdn state is "previous", so it stays on through sleep |
| AP_SLEEP | gpa0-5 | hub's view of AP state | zrn-bbd | high | High, pulsed around suspend (sensors/README) |
| Motor boost | gpg1-7 | the motor's boost | zrn_haptic (direct write, high) | high (from code) | High awake. The pdn state is output 0, so off in SoC sleep **src (DT motor-boost-en)** |
| HRM LED boost | gpg1-6 | HRM LEDs | never | input, pull-down (off) | high only while HRM runs |
| Iris enable | gpg0-7 | iris module | never | as booted (off) | iris only |
| Fingerprint LDO | gpm43-0 (`etspi-ldoPin`) | Egis sensor | never | low **obs** (`FINGERPRINT.md`) | Raised by the fingerprint HAL while it listens. Cut with an SMC at suspend **src** |
| MST power | 0x92 pin 4 (`mst_pwr_en`) | MST coil | never | as booted | pay sessions only |

### Count

There are 112 outputs or switches in the map. A typed row covering several outputs counts each of them:

| Chip | Outputs |
|---|---|
| S2MPS18 | 58: 13 bucks and 45 LDOs |
| S2MPB02 | 23: 3 bucks, 18 LDOs, 2 flash channels |
| S2MPB03 | 7 |
| MAX77705 | 4 |
| S2DOS05 | 6 |
| GPIO switches | 10 |
| UFS fixed regulator, counted with them | 4 |

**On at idle without need**, from the evidence above:

1. **BUCK1/BUCK4 at high voltage (MIF/INT pinned)**. On in both systems, but at 0.72/0.77 V against stock's 0.58/0.66 V at rest. **obs**
2. **BUCK5 after a camera session** (`dvfs_cam` never released). **src**
3. **S2MPB02 BUCK2 left at the boot value 0x58**, probably enabled, with nothing on it. **inf**
4. **S2MPB02 L18 (proximity LED)**: state unknown, needs a read.
5. **L34 (codec core)**: on as long as snd-zrn-abox is loaded. It matches stock while the codec is bound, so it is not a gap by itself.
6. **The audio block's power domain (pd-aud) and the abox DSP**. Not a PMIC output, but the largest "on without need" block in the audio path. **obs** (~1000 IRQ/s)
7. **The idle power domains** DSPM, DSPS, IVA, G2D, ISPPRE, ISPHQ, ISPLP, MFC and VTS: 15 of 16 are on (`power/README.md`). **obs**

ztOS keeps **off** some rails that stock powers: L38 (NFC), L42, L44 (eSE), the fingerprint LDO and HRM.

## Samsung's policy, from source

### Regulator suspend states and initial modes

- **No regulator in the rev26 tree has a `regulator-state-mem` node.** Suspend behaviour is entirely in the S2MPS18 mode bits plus the PWREN and PWREN_MIF signals that the PMU and ACPM drop in sleep. **src** (`stock-regulators.py`)
- The `regulator-initial-mode` values are in the rail map. **src**
  - Mode 1 (off in SoC sleep): BUCK2, BUCK3 and BUCK7 (CPU), and L4-L14 and L30.
  - Mode 2 (off when MIF is down): BUCK1, BUCK4, BUCK5, BUCK6 and BUCK9, and L32, L38-L42 and L44.
  - Mode 3 (on in sleep): BUCK8 and BUCK10-13 (DRAM VDD2 and the LDO pre-regulators), and L1, L3, L33, L37 and L43.
- `s2m_enable()` writes the stored mode, not plain "on". So a rail a Samsung driver enables gets its sleep behaviour from the DT. **src** (`s2mps18.c`)

### PMIC probe and LP settings (`s2mps18_pmic_probe`)

| Setting | Stock | ztOS |
|---|---|---|
| RTCBUF (0x13) bit 2 | set | already set: 0x45 in both dumps **obs** |
| B1OUT2-B4OUT2 (0x1e, 0x21, 0x24, 0x27) | written 0x20 (0.5 V, the second DVS set) | not written |
| PWREN_SEL2 (0xc9) | 0xff | not written; value unknown |
| SMPL/WTSR | configured from the DT | firmware defaults |
| Powermeter ADC | `adc_mode 2` runs the ADC. `s2mps18_pmic_suspend` clears ADC_CTRL3 (0xd1) bit 7 in suspend and restores it on resume | ADC state unknown. It is enabled only by Samsung's driver, so probably off **inf** |

In the stock TWRP dump the buck OUT2 registers equal OUT1, not 0x20 (`0x1e = 0x2d`). The driver's 0x20 is evidently overwritten by ACPM DVFS. **obs, inf**

`lpm_init`, ACPM's own sleep sequencers and `mif_always_on` are covered in `power/suspend/README.md`.

### Runtime PM per subsystem

| Subsystem | What stock does when idle | ztOS today |
|---|---|---|
| **Audio DSP (abox)** | `pm_runtime` autosuspend **500 ms** after the last stream (`abox.c:6202`). `abox_disable()` then: Calliope suspend IPC, DRAM request dropped, registers saved, pads to "idle" plus retention, the CPU clock off, GIC off. The genpd then switches **pd-aud** off (DT `power-down-ok = 1`, gated by `abox_is_on()`). System suspend itself is a no-op (`abox_suspend`). **src** | Powered from boot; the DSP ticks at 1 kHz forever. `aud_power_cycle` (the same PMU and SMC sequence) exists, but only as recovery, off by default. **obs** |
| **Camera (fimc-is)** | Rails off in reverse order at close. The ISP domains (ISPPRE, ISPHQ, ISPLP) go off through genpd when no device holds them. The `dvfs_cam` pm_qos is removed, so CAM DVFS falls to its floor (suspend 590 MHz, idle lower per the ECT). **src** | Rails: same (zrn-cam clears them). ISPPRE stays on. `dvfs_cam` is held at 690 MHz until reboot. **src** (REGISTERS.md) |
| **Sensor hub (ssp/BCM4773)** | L37 always on, and the hub runs its own low-power loop. Android batches through the HAL (max latency per sensor). At system suspend `ssp_suspend` pulses AP_SLEEP and sends nothing; `ssp_resume` sends 0xd3. **0xd2/0xd1 are never sent** on this kernel (they belong to early suspend, not built). **src**, REA in `sensors/README.md` | Same supply. The light sensor is dropped at screen off, 0xd2/0xd1 are no longer sent, and AP_SLEEP is pulsed (`ap_sleep_pulse`). **obs** |
| **GNSS** | Same chip as the hub; GPS_PWR_EN is held; the GNSS core sleeps between fixes (lhd). **src** | Same; `timed-gnss` holds sleep off while in use (sensors/README) |
| **NFC (sec_nfc)** | PVDD (L38) enabled at probe and kept on while NFC is on; the chip uses its own standby; the eSE (L44) only per session. **src** | Off, as unused |
| **HRM (MAX86915)** | LDO8, the LED boost GPIO and the pins on only while a client has it enabled (`hrm_power_ctrl`). **src** (`feature/hrm` README) | Off |
| **Fingerprint (etspi)** | LDO GPIO raised by the HAL while listening, then lowered. At system suspend: `MC_FC_FP_PM_SUSPEND` SMC. **src** (FINGERPRINT.md) | Off |
| **Touch (sec_ts)** | At screen off (`input_close`): unless `lowpower_mode` is set (AOD, double-tap or force-touch home key, set by the framework), `sec_ts_stop_device()` switches off **both L35 and L43**. With lowpower_mode: the controller's LP mode, rails on. This tree has no `always_lpmode`. **src** | Rails always on; the controller gets its sleep command (`patch-touch-suspend.py`) **src** |
| **Modem (modem_v1)** | CP runs its own PM. At AP suspend `pda_active` goes to 0 so the CP holds its indications. RIL screen-state filters come from userspace. **src** | Same driver. Screen off and awake, the CP sends signal indications at 5.8/min (modem/README) **obs** |
| **Haptic** | Bias on; motor enable only while playing; "boost" calls are no-ops on this chip. **src** | Same **obs** |
| **Power domains (exynos-pd + pmucal)** | Each PD is a genpd. Unused ones are switched off at late init (`pm_genpd_poweroff_unused`) and by runtime PM afterwards. Each on/off is wrapped in the TZPC SMC (`need_smc`, `SMC_CMD_PREPARE_PD_ONOFF` 0x82000410). The sequence is a single CONFIGURATION write and a STATUS wait (`flexpmu_cal_local_exynos9810.h`), for AUD, DPU, DSPM, DSPS, G2D, G3D, ISPPRE, ISPHQ, ISPLP, IVA, MFC and VTS. DCF, DCPOST and DCRD are disabled in the DT, so `pmucal_lpm_init` shuts them down at boot. **src** | Only G3D is a genpd (`pm_genpd_summary` **obs**). The other 15 stay as the bootloader left them, on. |

### What is off in deep sleep, stock

- Everything in mode 1 (the CPU bucks, L4-L14, L30) follows PWREN down. **src, inf**
- When ACPM takes MIF down, everything in mode 2 follows: BUCK1, BUCK4-6, BUCK9, L24-L28, L32, L38-L42, L44. DRAM is in self-refresh on BUCK8 (mode 3). **src, inf**
- Always on in sleep: the LDO pre-regulators, L1, L3, L15-L19, L33-L35, L37 (hub) and L43 (touch).
- The motor boost and other pads drop to their pdn state.
- The audio block and the idle domains are already off before suspend.

ztOS does not reach this state today.

## Gaps and plan

Ordered by expected gain. The owner's ~365 mW of unexplained idle floor (`power/always-on/README.md`, after Wi-Fi's 300 mW) is the target.

A note on what the PMIC itself costs: an LDO left enabled with no load costs microamps, well under 1 mW. **The rails matter through what they feed and the clock and voltage that load runs at.** That is why the top items are DVFS and power domains, not LDO bits. **inf**

| # | Gap | Est. saving | Change | Risk |
|---|---|---|---|---|
| 1 | **MIF (and INT) pinned at boot level while awake.** BUCK1 at 0.719 V for 1794 MHz against stock's 0.581 V at rest; INT 0.769 against 0.663 V. Stock devfreq idles MIF at 421 and INT at its floor. | **80-200 mW** awake, screen off **inf**: dynamic power scales roughly with V²f on the memory controller and bus, and the DRAM I/O drops too. This is the most likely single owner of the 365 mW. | Ask ACPM DVFS for MIF 421000 kHz at screen off and back to 1794000 at screen on, in zrnperfd or zrn-sleepd. Later, a real devfreq. | Medium. The 421↔1794 awake round trip is proven (`power/suspend/README.md`, October). Bandwidth for DP or desktop at 421 is untested, so stay at 1794 while DP is up. |
| 2 | **Audio DSP and pd-aud never off.** Stock powers off 500 ms after the last stream. | **20-60 mW** **inf**: the DSP core and the AUD block's leakage and clocks, plus ~1000 wakeups/s of CPU-side IRQ handling. It also unblocks future cpuidle/SICD (AUD is in SICD's idle list). | snd-zrn-abox: after 500 ms with no stream, suspend the DSP (Calliope IPC), save, pad retention, SMC PD_SAVE, AUD_CONFIGURATION = 0. Reverse on open. Reuse `aud_power_cycle`, split into off and on. | Medium. A failed power-on leaves no audio until reboot (`aud_broken`). The sequence works as recovery but is unproven as an idle policy. |
| 3 | **`dvfs_cam` held at 690 MHz after any camera use.** | **5-30 mW** after the first camera session, until reboot **inf**: BUCK5 voltage and the CAM block clocks | zrn-cam: at close, request `dvfs_cam` at its lowest ECT level (item (a) in REGISTERS.md) | Low. The same request path as the open, with another value. |
| 4 | **15 SoC power domains on** (DSPM, DSPS, IVA, G2D, ISPPRE, ISPHQ, ISPLP, MFC, VTS, DCF, DCPOST, DCRD and others). | **0-10 mW** measured: "moved nothing" one at a time (`power/README.md`). Their real value is as SICD/cpuidle prerequisites. | Genpd providers for DSPM/DSPS/IVA/ISP*/MFC, or a one-time boot shutdown, as `unused-domains.py` already does for DCF, DCPOST and DCRD. | Low-medium. A domain switched off under a driver that touches it raises an SError. |
| 5 | **Touch rails L35/L43 always on.** Stock cuts them at screen off without low-power mode. **L43 also sits at 3.3 V, 0.3 V above stock and the DT maximum.** | 1-5 mW at screen off **inf** (the controller's sleep current against a cut) | First restore L43 to 3.0 V. Cutting both rails at screen off conflicts with wake-by-touch and AOD, so leave that until AOD and wake gestures are settled. | Low for the voltage. Medium for cutting: re-init cost, and s9p-acpm has a touch power-cycle workaround. |
| 6 | **S2MPB02 BUCK2 left at 0x58.** Stock's cleanup switches it off. | <1 mW **inf** (no load; quiescent) | zrn-cam: at close, write 0x0d bits 7:6 = 00 instead of restoring 0x58. Or zrn-flash at load. | Low. A front-camera bring-up item ("restore as found" was a deliberate approval). |
| 7 | **LDO sleep modes differ** (L4-L14 and L30 mode 3 instead of 1; L32 mode 3 instead of 2; L17-L19 1 instead of 3). | 0 today (no SoC sleep). **1-10 mW in SoC sleep** **inf** (the leakage of the domains those LDOs feed) | Not before the writer is found. If the CP or APM sets them, overriding them could break the modem. Read them in stock Android with the modem up first. | High until the writer is known. |
| 8 | **S2MPS18 powermeter ADC** possibly running through suspend | 0-2 mW **inf** | Read 0xd1 first; clear bit 7 around suspend, as stock does | Low |
| 9 | Non-rail items in the same floor (for completeness) | already tracked | No cpuidle/SICD (ROADMAP 3.5), UFS runtime PM, dwc3 runtime PM, fuel-gauge IRQ 2/s | see `power/always-on/README.md` |

**Link to the ~365 mW.** None of the PMIC enable bits is big enough to explain it, because idle LDOs and bucks cost microwatts to a few milliwatts. Items 1 and 2 are both **on and clocked while idle** in ztOS and **off or slowed** in stock. Together they plausibly cover 100-260 mW of it (**inf**); cpuidle is a further share. Measure each with `ztos-power-measure` (the A/B in `power/always-on/exp.sh` style), not by these estimates.

## Register writes for the top three fixes

**None of these has been made. Each needs the owner's per-register approval.** Values are from source unless marked otherwise.

### Fix 1: MIF to 421 MHz at screen off (ACPM DVFS, the APM mailbox)

The same transport zrn-cam already uses for `dvfs_cam` (REGISTERS.md item J), with a different domain.

| Step | Register | Value |
|---|---|---|
| 1 | APM SRAM (base 0x02039000), the channel-0 command ring at the slot `rx_front` points to | 4 words: `[seq<<16 | 0 (dvfs_mif), 421000, 0 (set), 0]` |
| 2 | That channel's front/rear index in APM SRAM | advanced by one |
| 3 | Mailbox 0x14100000 + 0x08 (INTGR0) | channel 0's doorbell bit |
| 4 | Mailbox + 0x20 (INTCR1) | on the reply, clear the reply bit (as item J) |
| 5 | Screen on | steps 1-4 with 1794000 |

- Domain number: `dvfs_mif` is 0 in `acpm_dvfs_exynos9810.h`; `dvfs_int` is 1.
- 421000 kHz is stock's resume level for every MIF ASV row (`audit-samsung-minmax.py`).
- The optional INT step is the same request with domain 1 and INT's floor (stock suspend: 107000 kHz). Approve it separately.
- No PLL or PMIC register is written directly; ACPM does the voltage (BUCK1, 0x1d).

### Fix 2: power the audio block off when idle

Samsung's `blkpwr_aud` and the existing `aud_power_cycle`, split into off and on.

**Off**, 500 ms after the last stream closes:

| Step | Write | Value |
|---|---|---|
| 1 | Calliope suspend IPC to the DSP | the abox mailbox (as `abox_cpu_pm_ipc(false)`) |
| 2 | ABOX CPU clock, CMU_AUD 0x17c00000 | gate off (`clk_disable(clk_cpu)`); the CMU registers listed in snd-zrn-abox `aud_regs` are saved, read only |
| 3 | PMU 0x14060000 + 0x1424 (GPIO_MODE_ABOX) | bit 0 = 1 (pad retention) |
| 4 | SMC 0x82000410 (PREPARE_PD_ONOFF) | args `0` (PD_SAVE), `0x17c10204` (AUD TZPC) |
| 5 | PMU 0x14064000 (AUD_CONFIGURATION) | bits 3:0 = 0. Then poll 0x14064004 until bits 3:0 = 0 (read) |

**On**, at the next open:

| Step | Write | Value |
|---|---|---|
| 1 | 0x14064000 | bits 3:0 = 0xF; poll 0x14064004 until 0xF |
| 2 | SMC 0x82000410 | args `1` (PD_RESTORE), `0x17c10204` |
| 3 | CMU_AUD 0x17c00000 + the `aud_regs` offsets | the saved values, in the driver's order (PLL_CON0 0x0120 before its lock) |
| 4 | ABOX SYSREG 0x17c10000 + 0x104 and + 0x300 | the saved values |
| 5 | PMU + 0x1424 | bit 0 = 0, releasing pad retention |
| 6 | The DSP's firmware boot | the driver's existing path |

`aud_power_cycle` already uses steps 3-5 of the off sequence and steps 1-5 of the on sequence. It is off by default (`recover_power_cycle=N`, `aud_cycles=0`). **obs**

### Fix 3: release `dvfs_cam` at camera close

| Step | Register | Value |
|---|---|---|
| 1-4 | As Fix 1, channel 0 | `[seq<<16 | 7 (dvfs_cam), LOW, 0, 0]` |

- `LOW` is the lowest CAM level in the ECT DVFS table. Read it from the ECT; stock's suspend value is 590000 kHz.
- Once approved, this is REGISTERS.md item (a).

### Smaller items, if wanted

- **L43 back to 3.0 V**: S2MPS18 0x74 = **0xf0**, with mask 0x3f on the voltage bits only, through ACPM. The current value is 0xfc. First find what set 0x3c.
- **S2MPB02 BUCK2 off at camera close**: 0x0d bits 7:6 = 00 (mask 0xc0), through bus 97.

## Fixes applied and measured (2026-11-12)

The owner approved fixes 1, 2 and the L43 part of 4 on 2026-11-12. Fix 3 and the S2MPB02 buck belong to the camera agent (zrn-cam).

| Fix | Code | On the phone |
|---|---|---|
| 1. MIF 421 MHz at screen off | `power/rails/zrn-rails.c` (`mif_khz`, kill switch `mif_enable`), `zrnperfd` (`mif-scale on\|off`, 1794 MHz while a DP connector is `connected`) | `/etc/modules-load.d/zrn-rails.conf`; zrnperfd rebuilt |
| 2. Audio block off when idle | `audio/driver/snd-zrn-abox.c`, the block "the audio block off while nothing plays" (`idle_off_ms`, 0 = kill switch) | `extra/snd-zrn-abox.ko` (includes `feature/headset`) |
| 4. L43 (touch AVDD) at 3.0 V | `zrn-rails.c` at load: 0x74 voltage bits = 0x30 (`l43_fix`) | with zrn-rails |

Backups: `*.pre-rail-map` next to each replaced file. Restore with `sudo sh power/rails/install.sh restore`, then reboot.

### Measurements

All runs used `power/rails/ab.sh`: screen off, AOD off (sleep inhibited), awake, on battery, interleaved A (fix off) / B (fix on) rounds, 60 s settle, then 60 one-second samples. The rows below are **obs**.

| Fix | A | B | Difference |
|---|---|---|---|
| 1. MIF | 455, 341, 355 mA | 317, 304, 314 mA | The two clean pairs (A2/B2, A3/B3) give **37 and 41 mA, i.e. 141 and 154 mW**. A1 ran at 67 % CPU busy and is left out of the estimate. CPU busy rose from 12-17 % to 27-34 % at 421 MHz, so memory-bound work takes longer. |
| 2. Audio idle | 369, 346, 461 mA; abox IRQ ~1050-1090/s | 362, 350, 459 mA; abox IRQ **0/s** | +7, -4, +2 mA (+27, -13, +11 mW). That is **under 10 mW, within the noise**: the CPU ran at 67 % busy from another agent's load in all six windows. |

- An earlier run on the charger is discarded. The charge current was tapering by 70 mA per window.
- **Audio wake:** power-on of the block plus the DSP boot takes **143-196 ms** (`wake_us_last`, 8 wakes, 0 failures).
- **Audio playback after a wake:**
  - A 10 ms `aplay` to hw:ZrnAbox,1 took 513-619 ms with the block off, against 311-506 ms with it on.
  - PipeWire (`pw-play`) woke the block, played, and the block was off again ~7 s later: WirePlumber's 5 s suspend plus 0.5 s.
- **The 1 kHz DSP tick comes from a fresh module load, not from open streams.**
  - At 07:00 every PCM was closed (`/proc/asound/card0/pcm*/sub0/status`), and the DSP still raised ~1000 IRQ/s.
  - After a wake from idle the DSP does not tick.
  - pid 94 from the overnight watch was gone after that boot, so it could not be identified.
- **MIF requests:** 19 sent, 0 failures, 0.5-4 ms each. Screen on (09:19) raised MIF to 1794 MHz in 1.4 ms: the lock screen drew normally, and MIF went back to 421 at screen off.
- **L43 (touch AVDD):**
  - 0xfc became 0xf0 at the first load.
  - The boot after the 08:09 reboot (not this agent's) left the voltage bits at 0x30 already.
  - The touch driver stayed bound, with no errors.

## Open reads

These are reads, no writes. Each needs a go-ahead because it loads a module or uses a selector file.

1. **S2MPS18 0xc8/0xc9 (PWREN_SEL), 0xcc-0xce (LPM_MODE), 0xd1 (ADC), and the buck CTRL/OUT registers now.**
   - How: zrn-bbd's existing `pmic` file. Writing `RR` only selects the register for the read, with no PMIC write. But it is a shared selector, so it needs a quiet moment with no other agent.
2. **The S2MPB02 output registers 0x0b-0x2f, especially BUCK2 (0x0d) and L18 (0x2f).**
   - How: `zrn-cam-probe pmic`, which loads zrn-cam. That needs your go-ahead and the camera agent's release.
3. **Stock Android's LDO modes with the modem running.** This would settle gap 7, and with it who rewrote L4-L19, L30, L42 and L43. It needs the stock image booted, so it is not possible on this phone without reflashing.
