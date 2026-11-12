# Heart rate and SpO2 sensor (ROADMAP 8.26): research

Status: **phase 1 done (read-only). Nothing has been written to the phone.** The
experiment below needs the owner's approval before anything is touched.

Every finding is labelled:

- **observed**: seen on the phone or in a capture of it;
- **source**: read in Samsung's GPL kernel or the stock device tree;
- **inferred**: worked out from the other two, not checked.

Sources used:

- The stock device tree, taken from the 12 DTBs in `stock/BOOT.img`. It is the
  same for board revs 16 to 26.
- Samsung's kernel for starlte (`exynos-linux-stable/starlte`, branch
  `tw10-android-4.9-q`):
  - `drivers/optics/{hrmsensor,hrm_max86915,max86915}.c`;
  - `include/linux/mfd/s2mpb02-private.h`;
  - `include/linux/mfd/samsung/s2mps18-private.h`;
  - `drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c`.

  There is no `third_party/vendor-kernel` or `stock/twrp-live.dts` on this PC.
- `sensors/stock-state/`: TWRP captures.
- The running phone: `/sys`, the device tree, and pinctrl debugfs.

## Verdict

The sensor is a **Maxim MAX86915**, a 4-LED optical sensor. It sits on its own
SoC I2C bus at **0x57** and is **not** behind the BCM4773 hub. Nothing in the
path is owned by the secure world. ztOS can almost certainly drive it the same
way it already drives the amplifier and the flash: a bit-banged I2C bus on the
stock pins, plus one sub-PMIC LDO.

The verdict is that it can be done, with **high confidence**. Two points are
still open, and the experiment answers both: whether the chip answers on a
bit-banged bus, and whether it needs its LED boost on to answer.

## Hardware

| What | Value | How we know |
|---|---|---|
| Chip | Maxim **MAX86915**: IR, red, green and blue LEDs, one photodiode front end, 19-bit ADC, FIFO. PART_ID register `0xFF` = **`0x2B`**, REV_ID `0xFE` (`0x01` expected) | source: `max86915.c`, `hrm_max86915.c` (`MAX86915_PART_ID 0x2B`); starlte defconfig has `CONFIG_SENSORS_MAX86915=y` |
| DT node | `hrmsensor@57`, `compatible = "hrmsensor"`, `reg = <0x57>` | source: stock DTB |
| Bus | `hsi2c@108E0000` (stock alias `hsi2c20`). This is PERIC1 **USI08**, shared with `spi9` and `uart10` at the same address. Stock runs it at 400 kHz, with `samsung,usi-i2c-v2` | source: stock DTB; cmucal-sfr names |
| Same bus | `sec-nfc@27`, the NFC controller, which ztOS does not use | source: stock DTB |
| I2C address | **0x57** (7-bit) | source: DTB. observed in TWRP: pinctrl owner `6-0057` (`sensors/stock-state/pinctrl.txt`) |
| SCL / SDA | **gpp5-0 / gpp5-1** (PERIC1 pinctrl `10830000`), function 2, no pull | source: DTB `hsi2c20-bus` and `gpio_scl`/`gpio_sda` |
| SCL / SDA now | input, **PUD = pull-down**, DAT reads **0** on both lines, not claimed by anything | observed: `/sys/kernel/debug/pinctrl/10830000.../pins`, `pinmux-pins` |
| Interrupt | **gpp3-0** (PERIC0 `10430000`), EINT, no pull; the driver asks for the falling edge (`IRQF_TRIGGER_FALLING`) | source: DTB `hrm-irq`. observed in TWRP: `gpio-135 hrm_int in lo IRQ` |
| Interrupt now | input, no pull, DAT 0, not claimed | observed |
| LED boost enable | **gpg1-6**, `hrmsensor,hrm_boost_en-gpio`. Driven high on power-up and back to an input on power-down. The pin is pulled down, so the boost is off by default | source: DTB, `hrm_power_ctrl()`. observed: input, pull-down, DAT 0 |
| LED supply | A separate boost converter, switched on by gpg1-6. Its input is most likely VBAT/VSYS; the part is unknown | inferred from the name and the driver; the DT names no LED regulator, and `led_3p3` is compiled out for the MAX86915 |
| VDD (1.8 V) | **`HRM_1.8V_AP`** = **S2MPB02 LDO8** (sub-PMIC, I2C 0x59). Control register **0x25**: bit 7 enable, bits 6:0 voltage select, 600 mV + 25 mV/step, so 1.8 V = `0x30` and on = `0xB0` | source: DTB `s2mpb02-ldo8`; `s2mpb02-private.h` (L8CTRL is 0x25, next to L9CTRL 0x26 that `leds/zrn-flash.c` already uses); `s2mpb02-regulator.c` (LDO8: MIN1 600 mV, STEP2 25 mV). observed in TWRP: off (use 0) |
| I2C pull-up rail | **`VDD_NFC_1P8`** = **S2MPS18 LDO38**, register 0x6F | source: DTB (`hrmsensor,i2c_1p8`), `s2mps18-private.h` |
| Pull-up rail now | Probably on at 1.8 V. The mainline dump has `0x6F = 0xAC`: bits 7:6 = 10 means enabled, and select `0x2C` gives 700 + 44×25 = 1800 mV | observed: `sensors/stock-state/s2mps18-regs-mainline.txt`, an older capture. "Still on today" is inferred |
| S2MPB02 bus in ztOS | bus **97**, bit-banged on gpp6-0/1, owned by `zrn-flash` (client 0x59), and shared by `zrn-cam` | observed: `/sys/bus/i2c/devices/97-0059`, lsmod; source: `leds/zrn-flash.c` |
| Mainline DT | No node for the bus or the chip. Only `i2c@104b0000`, `14360000` and `14370000` exist | observed: `/sys/firmware/devicetree/base/soc@0` |
| Kernel | 7.1.0-zboot-wd-dp6. `CONFIG_I2C_CHARDEV=y`, `CONFIG_I2C_EXYNOS5=y`, `CONFIG_EXYNOS_USI=y`. **`CONFIG_IIO` is not set** | observed: `/proc/config.gz` |
| Hub? | No. The S9's HRM is an AP-side I2C device with its interrupt on an AP GPIO. ztOS's SSP tooling has no HRM sensor type | source: DTB, ztOS `sensors/` |
| Secure world | No. The stock drivers are plain Linux I2C drivers: no SMC calls, no tzdev, no TEE. The bus is an ordinary non-secure USI | source: grep of the three vendor files. inferred for the bus |

Why DAT is 0 on both lines today (inferred): HRM_1.8V_AP is off. An unpowered
MAX86915 clamps SCL and SDA through its ESD diodes, and the SoC's pull-down
does the rest. The experiment checks this: once LDO8 is on, both lines should
read 1.

### Two ways to reach the bus

1. **Bit-banged (recommended).** Use `i2c-gpio` on gpp5-0 and gpp5-1, as
   `snd-zrn-abox` (bus 98) and `zrn-flash` (bus 97) already do. This touches
   no CMU, sysreg or USI registers. 100 kHz is enough: one 4-LED sample is
   12 bytes, so 100 Hz × 12 B ≈ 10 kbit/s.
2. **Hardware HSI2C.** This needs:
   - a DT node, which means a BOOT flash (not approved);
   - the PERIC1 USI08 gates opened. From cmucal-sfr: `0x108020f8` USI08_USI
     PCLK, `0x108020f4` IPCLK, `0x1080209c` RSTNSYNC, `0x108020c0` SYSREG
     PCLK, `0x1a2420c4` GATE_CLKCMU_PERIC1_IP, and DIV `0x10801820`;
   - SW_CONF set to I2C. The address is inferred to be `0x1081102C` from the
     order of the stock `usi@1081xxxx` nodes, and it is not checked;
   - USI_CON and USI_OPTION at `0x108E00C4` / `C8` written.

   That is all SoC register writes, each needing approval, and a gated block
   read hangs the bus silently on this SoC. It is not worth it for a 10 kbit/s
   sensor.

## Driver situation

- **Mainline:** there is no MAX86915 driver. `drivers/iio/health/max30102.c`
  handles MAX30101/30102/30105 (source: 7.1 tree). The MAX86915 is the same
  family and its register map is close:
  - The same in both: FIFO pointers `0x04`-`0x07`, FIFO config `0x08`, mode
    config `0x09` (reset bit 6, shutdown bit 7), and `0x0A` (ADC range 6:5,
    sample rate 4:2, pulse width 1:0). LED amplitude starts at `0x0C`.
  - Different on the MAX86915:
    - a second interrupt status and enable pair (`0x01`/`0x03`);
    - a fourth LED (`0x0F`);
    - a per-LED range register (`0x11`), a pilot PA (`0x12`), and LED sequence
      slots at `0x13`/`0x14` (Maxim's "flex" mode);
    - crosstalk DACs at `0x26`-`0x29`;
    - PART_ID `0x2B` where the MAX30102 has `0x15`.

  Adding it to `max30102.c` is a moderate patch, roughly 250 lines (inferred).
  It would also need IIO built, because the running kernel has
  `CONFIG_IIO` unset.
- **Samsung:** `drivers/optics/max86915.c` (6000 lines, GPL) works, but it is an
  input device plus misc device built around Samsung Health's SDK mode, AGC and
  factory EOL tests. It is a good reference for register values, LED currents
  (IR `0x46`, red `0x41`, green `0x28`, blue `0x3C`, range 150 mA) and the
  power sequence. Don't port it.
- **Recommendation:** an out-of-tree **`zrn-hrm.ko`** in the style of
  `zrn-flash`, with no kernel rebuild and no BOOT flash:
  - it registers `i2c-gpio` bus 99 on gpp5-0/1;
  - it switches S2MPB02 LDO8 through bus 97, the way `zrn-cam` already shares
    it;
  - it raises gpg1-6 only while measuring;
  - it takes gpp3-0 as the FIFO-almost-full interrupt;
  - it exposes `/dev/zrn_hrm`: `read()` returns raw FIFO samples (IR, red,
    green, timestamps), and an ioctl or write sets mode, rate and currents.

  An IIO driver can come later, upstream, if IIO is ever enabled.

## Algorithms

Stock does heart rate and SpO2 in closed user space (Samsung Health and its
SDK, which uses the driver's `MODE_SDK_*` and `/dev/max_hrm` paths; inferred
from the driver). The kernel only delivers raw ADC counts. Open options:

- **Heart rate** (green LED, or IR on a finger): band-pass at 0.5-4 Hz, then
  peak detection or autocorrelation, then the median of the beat intervals.
  - Peak detection: Elgendi's two-moving-average method. NeuroKit2's
    `ppg_findpeaks` (MIT) implements it.
  - HeartPy (GPL-3) is a reference.
  - SparkFun's MAX3010x `heartRate.c` (PBA) is a small C example.
  - All of these are easy to write in about 150 lines of C or Python.
- **SpO2** (red and IR):
  - The "ratio of ratios": R = (AC_red/DC_red) / (AC_ir/DC_ir).
  - SpO2 is then an empirical curve, f(R). Maxim's reference designs use
    `110 − 25·R`, and the RD117 code by R. Fraczkiewicz uses
    `−45.06·R² + 30.35·R + 94.85`.
  - Those coefficients are for Maxim's own optics, not the S9's window, LED
    spacing and cover glass.
- **Perfusion and quality gating** matter more than the formula. Reject a
  window when:
  - DC is too low (no finger);
  - AC/DC is out of range;
  - the accelerometer moves (the hub's accelerometer is already in zrnsensord);
  - successive beat intervals disagree.

### An honest note on accuracy

- **Not a medical device.** Samsung itself labelled the S9's readings "for
  fitness and wellness only". ztOS's will be weaker, because it has none of
  Samsung's per-model tuning.
- **Heart rate** at rest, with a still fingertip, should come within a few beats
  per minute of a chest strap; reflective finger PPG is generally good for
  resting heart rate. Motion, cold fingers and pressing too hard break it.
- **SpO2** cannot be calibrated properly by us. Real calibration means a
  controlled-desaturation study against arterial blood gas (ISO 80601-2-61).
  Without it, our number is a curve fitted at one point at best: for example,
  set against a borrowed fingertip oximeter at about 97-98 %. It will read
  "normal" plausibly, but it can't be trusted to detect low oxygen, which is
  the one case that matters. Reflective sensors are also known to read high on
  darker skin.

  The app should show SpO2 as experimental, with the caveat on screen, or
  leave it out and ship heart rate only.

## Plan and size

| Step | What | Size |
|---|---|---|
| 0 | **Experiment: read the part ID** (below). Needs approval. | S, a 120-line probe module |
| 1 | `zrn-hrm.ko`: bus, power, interrupt, FIFO, `/dev/zrn_hrm`; no LEDs until asked | M, about 500 lines |
| 2 | First LED test: IR only, low current (for example `0x10` = 3.2 mA at range 0, which is 0.2 mA per step by the vendor's `CURRENT_PER_STEP`), finger on the sensor, 100 Hz for 10 s, raw dump. Needs its own approval: it drives the LEDs | S |
| 3 | `zrnhrmd` or a zrnsensord extension: heart rate from green or IR, SpO2 from red and IR, quality gates, results in `/run/zrn-sensors/hrm` | M, about 400 lines |
| 4 | ztMETRO heart rate app or shell tile: "place finger", live waveform, BPM, SpO2 marked experimental | S-M |

In all, the size is **M**, as the roadmap says. The risk is low once step 0
passes.

## Proposed experiment (step 0): read the part ID

The goal is to show that the MAX86915 answers at 0x57 on gpp5-0/1 and returns
PART_ID `0x2B`. Nothing is written to the MAX86915, and the LEDs are never
driven. The boost enable (gpg1-6) is not touched.

Preconditions:

- the camera is idle;
- run it inside `ztlock run feature/hrm`.

It is one small out-of-tree module, `sensors/hrm/probe/zrn-hrm-probe.c` (not
written yet). It does the work at load and undoes everything before
`insmod` returns.

### Every access, in order

| # | Kind | Target | Access | Why |
|---|---|---|---|---|
| 1 | module load | `zrn-hrm-probe.ko` (`i2c-gpio` and `i2c-algo-bit` are already loaded) | `insmod` | runs steps 2-11, then exits with `-EAGAIN` so nothing stays loaded |
| 2 | I2C read | bus 97, S2MPB02 0x59, reg **0x25** (L8CTRL) | write 1 byte (register pointer `0x25`), repeated start, read 1 byte | expect `0x30` (off, 1.8 V). **Stop** if bit 7 is already set or bits 6:0 are not `0x30` |
| 3 | **I2C register write** | bus 97, 0x59, reg **0x25** | write `0x25, 0xB0` | **LDO8 on at 1.8 V** (HRM_1.8V_AP) |
| 4 | I2C read | bus 97, 0x59, reg 0x25 | as in 2 | confirm `0xB0` |
| - | wait | | 2 ms | rail settles; stock waits 1 ms |
| 5 | GPIO (kernel gpiolib) | gpp5-0 (SCL), gpp5-1 (SDA) | requested by `i2c-gpio` as open-drain "high" = **input**. CON stays 0 (input), DAT is not driven, and PUD is left as it is (pull-down) | new bus 99. Registering the bus makes no I2C transfer (`bit_test` = 0) |
| 6 | pin read | gpp5-0, gpp5-1 level | `gpiod_get_value` | both should be 1 (pulled up). If either is 0, skip 7 and go to 9 |
| 7 | I2C read | bus 99, **0x57**, reg **0xFE** | write 1 byte (pointer `0xFE`), repeated start, read **2 bytes** (REV_ID, PART_ID); at most 3 tries, 10 ms apart | exactly the vendor's WHOAMI read. Expect `xx 2B` |
| 8 | log | dmesg | `zrn-hrm-probe: rev 0x.. part 0x..` | result |
| 9 | GPIO | gpp5-0/1 | released (they stay inputs) | bus 99 removed |
| 10 | **I2C register write** | bus 97, 0x59, reg **0x25** | write `0x25, <value read in 2>` (`0x30`) | **LDO8 back off** |
| 11 | I2C read | bus 97, 0x59, reg 0x25 | as in 2 | confirm it is off |

Nothing else:

- no write to any MAX86915 register;
- no write to gpg1-6 (LED boost) or gpp3-0 (interrupt);
- no change to S2MPS18 (VDD_NFC_1P8);
- no devmem, CMU, sysreg or USI register access;
- no `i2cdetect`.

Bus 97 is shared with `zrn-flash` and `zrn-cam`. The module reaches it through
`i2c_get_adapter(97)`, as `zrn-cam` does, so each transfer is serialised. The
read-modify-write of `0x25` is not atomic against `zrn-cam`, but `zrn-cam`
never touches `0x25` (its S2MPB02 list is `0x03`, `0x04`, `0x0d`, `0x0e`,
`0x22`, `0x23`, `0x27`, `0x2a`-`0x2d`). Hence the "camera idle" precondition.

### If the chip does not answer (a separate approval, not part of the first ask)

- **7b.** The stock driver always raises the boost enable before its WHOAMI.
  Repeat steps 5-9 with **gpg1-6 driven high** (output 1) around step 7, then
  set it back to an input (its pull-down holds it low).

  The LEDs should stay dark: after power-on, the chip's LED amplitude
  registers are 0 and no mode is set (inferred from the MAX3010x family's
  reset defaults; the MAX86915 datasheet is not checked). So this powers the
  boost and no LED.
- **7c.** If the lines read 0 with LDO8 on, clear the gpp5-0/1 pull-down:
  PUD register `0x10830028`, bits 1:0 and 3:2 set to 0, as stock's
  `hsi2c20-bus` has it.

  That is a raw pinctrl register write. Ask first.

### Rollback

- The module leaves nothing loaded, and LDO8 is restored to what it read.
- If the phone wedges, reboot: LDO8 is not always-on, so it comes up off.

## Not done (phase 1 rules)

There were no I2C transfers, GPIO, regulator or module changes, and no devmem
on the phone. `stock/EFS.img` was not read.
