# Iris scanner on the Galaxy S9: feasibility (ROADMAP 8.22, 2026-11-12)

This is a study, not an implementation. Nothing was written to the phone. Every
finding is labelled **observed** (seen on this phone or in a stock binary),
**source** (Samsung's open kernel for the 9810), or **inferred**. The
fingerprint sensor is in [FINGERPRINT.md](FINGERPRINT.md).

## Verdict

**Raw IR capture from Linux is probably blocked. Confidence about 70 %. One
experiment would settle it.**

- On this phone, the secure world owns the iris sensor's I2C pads (gpp4-6/7) and
  its enable pin (gpg0-7) from boot. Both read as zero in every field and ignore
  writes.
- Samsung's TEE is not running on our boot, yet the pads are still locked. So the
  lock is set before Linux starts, not by an SMC that a session makes.
- The sensor itself is driven by Samsung's *non-secure* kernel, with its whole
  register table in public source. Only the frames go to secure memory.
- So the question is one thing: can a normal-world I2C controller reach the
  sensor through pads that the secure world has already configured? If the
  secure world left the pads in I2C function, the answer is yes. If it left them
  as inputs, the answer is no.
- The stock secure path (talking to Samsung's trusted app) is not possible.

Separately, the camera driver's earlier iris attempt was doomed for a second
reason, not only gpg0-7: it bit-banged I2C on gpp4-6/7, and those pads are
read-as-zero, write-ignored too (see "What the gpg0-7 result means").

## Hardware

Stock DT: `stock/BOOT.img` holds twelve trees (Star rev16 to rev26, twice).
`camera/iris/stock-dt.py` extracts them as text. The iris nodes are the same in
rev16 and rev26 apart from phandle numbers.

| Piece | Value | Evidence |
| --- | --- | --- |
| Sensor | Samsung S5K5F1, `samsung,sensor-module-5f1`, node `fimc-is-secure@10`, position 3, id 3 | source: DT |
| I2C | address **0x10** on `hsi2c@108D0000`: USI07's I2C, clocks GATE_USI07_I2C (257) and PERIC1_USI_I2C (282), `sensor_i2c_ch = 6`, 400 kHz, IRQ SPI 0x1af | source: DT, `dt-bindings/clock/exynos9810.h` |
| I2C pads | gpp4-6 SCL, gpp4-7 SDA. Stock's `default` is function 0 (input); `on_i2c` is function 3 | source: DT `hsi2c19-bus` / `-in` |
| MCLK | channel 2, pad **gpc1-6**, function 2. Pinctrl reads it normally | source: DT; observed: CON 0, PUD 1, DRV 2 |
| Reset | **gpg2-3**, active high. Reads normally | source; observed |
| Enable | `gpio_iris_en` **gpg0-7**, first step of power-on. What it gates is not stated | source: `fimc-is-device-module-5f1.c` |
| Rails | S2MPS18 LDO41 VDDIO 1.8 V, LDO40 VDDA 2.8 V, LDO39 VDDD 1.0 then 1.05 V, in that order, all through ACPM | source: module-5f1.c, DT; observed by zrn-cam: registers 0x72, 0x71, 0x70 |
| Power-on order | iris_en 1, VDDIO, VDDA, VDDD 1.0 V, then 1.05 V, `on_i2c` (10 us), reset high, MCLK function (2 ms) | source |
| Receiver | **CSIS3** `fimc_is_sensor@16260000`, `csi_ch 3`, `dma_ch 3`, `scenario = 6` (SENSOR_SCENARIO_SECURE), PHY `0x168`, behind sysmmu@16310000 (secure base 0x16300000) | source: DT |
| Mode | 2400x2400 RAW8, 30 fps, **2 lanes**, 2041 Mbps, VC0 only. The full register table (reset, clock, mode) is in the module file | source |
| Secure memory | ION `secure_camera`, 0xE1900000, 30 MB, `ion,untouchable`. Our DT already reserves it no-map | source; observed: `/proc/iomem` |
| IR LED | Second flash channel (FLED2) of the **S2MPB02** at I2C 0x59, the same chip as the rear flash. Registers 0x13 CTRL2, 0x16 CUR2, 0x17 TIME2, 0x18/0x19 IRON1/2 (pulse width), 0x1A/0x1B IRD1/2 (pulse delay) | source: `leds-s2mpb02.c`, `s2mpb02-private.h`, `CONFIG_LEDS_IRIS_IRLED_SUPPORT=y` |
| LED trigger | CTRL2 = 0x38: "remote mode off, power-LED mode on, GPIO polarity". So the pulse starts on a hardware edge, not on an I2C write. The edge is most likely the sensor's strobe output | source: comments; inferred: which pin |
| Wavelength | Not in source. Osram markets 810 nm for iris LEDs | inferred |

## Secure world

**TEE:** Trustonic Kinibi (`CONFIG_TRUSTONIC_TEE=y`; TAs in
`/vendor/app/mcRegistry`). On our kernel Kinibi is **not initialised**:
`MC_FC_INFO` returned state 0 (observed 2026-10-22,
`power/suspend/secure-handoff-audit.md`). The EL3 monitor and whatever the
bootloader set up are still active.

### What stock does for iris (source and observed in binaries)

- **Kernel:** the 5F1 module driver powers the sensor and does all of its I2C:
  reset, clock, the 30 fps table, exposure, gain and stream on/off. The kernel
  also drives the IR LED parameters through V4L2 controls
  `V4L2_CID_IRLED_SET_{CURRENT,WIDTH,DELAY,ONTIME}`, which go to
  `s2mpb02_ir_led_*()`. (source)
- **No kernel SMC for iris on this SoC:** the three secure-camera SMCs in
  `fimc-is-device-sensor.c` (`MC_SECURE_CAMERA_PREPARE`, `_UNPREPARE`,
  `_CFW_ENABLE`) are commented out with `/* HACK :: smc code is not prepared */`.
  (source)
- **HAL:** `libexynoscamera3.so` has a "Vision" frame factory for the S5K5F1 and
  logs `[IRIS_LEAK] Check Secure Memory Leak`. It sets the IR LED fields
  `irLedCurrent/Width/Delay/OnTime`. (observed: strings)
- **Service:** `/system/lib64/libIrisService.so` and `libIrisTlc.so` talk to the
  TA through `libMcClient`. Calls include `IrisTlc_AutoExposure`,
  `GetPreviewFrame_NV21`, `Enroll`, `Authenticate` and `sendIRProperty`.
  Exposure and LED settings are computed inside the TA. (observed: strings)
- **TA:** `ffffffff000000000000000000000047.tlbin` (sec_iris):
  - It works on secure buffers: `prepare_buffer_for_secure`,
    `is_tag_buffer_for_secure`, `check_sec_camera_session_id`,
    `clear_secure_memory`.
  - It runs `irlib_*` matching, including a multi-biometric mode with the front
    camera (`on_identify_front_frame`).
  - It wraps templates with a key derived from the device REK and hands them
    back for storage. Storage is in the normal world, under
    `/data/system/users/0/bio/ir/`, as sealed blobs. Not EFS.
  - The 8.5 MB `ffffffffd0000000000000000000001a.tlbin` holds the recognition
    library.

  (observed: strings)

### What is protected

| Resource | State | Evidence |
| --- | --- | --- |
| Iris I2C pads gpp4-6/7 | **Secure-owned**: every field reads 0, including CON_PDN and PUD_PDN | observed |
| `gpio_iris_en` gpg0-7 | **Secure-owned**: same signature; zrn-cam's writes had no effect. Neighbour gpg0-6 reads normally | observed |
| MCLK gpc1-6, reset gpg2-3, rails | Normal world | observed |
| HSI2C USI07_I2C controller (0x108D0000) | Unknown. In stock the non-secure kernel uses it, so probably not protected by TZPC | source; inferred |
| CSIS3 and its DMA | Unknown. CSIS0/1 work non-secure. CSIS3 sits on the same bus | inferred |
| SysMMU for ISPPRE | Has a secure instance at 0x16300000 that the secure world can program | source |
| Frame memory 0xE1900000 | Reserved by stock as secure. Whether TZASC protects it right now is unknown. We don't need it | source |

### What the gpg0-7 result means

A full read of the pinctrl debugfs shows exactly **13 pads** whose registers
all read zero, including the power-down fields. Every other pad on the SoC
shows the bootloader's sleep defaults (CON_PDN 2, PUD_PDN 1). The 13 are
(observed; `camera/iris/secure-pins.sh` lists them):

| Pads | Stock use |
| --- | --- |
| gpp0-0..3 | SPI1 (USI00), fingerprint, `secure-mode` |
| gpp4-0..3 | SPI7, eSE (`CONFIG_ESE_SECURE_SPI_PORT=7`) |
| gpp4-6, gpp4-7 | iris I2C |
| gpg0-7 | iris enable |
| gph0-6, gph0-7 | MST (Samsung Pay) data and enable |

That set is exactly the pads Samsung hands to the TEE. Samsung's
`et5xx.h` calls the feature "GPIO protection", and warns that touching a
protected pin "will happen kernel panic" on some SoCs.

On the 9810 the pads behave as **read-as-zero, write-ignored**:
- reads gave zeros with no fault (observed);
- writes to gpg0-7 had no effect (observed by zrn-cam).

The protection is per pin, not per bank: gpg0-6 and gpp4-4/5 are normal.

**When is it set?** It is present on a boot where Kinibi was never initialised,
so it is not switched on per session. The bootloader or EL3 firmware sets it
before Linux starts (inferred, strongly). Nothing in Samsung's source or in
Linux can clear it from the normal world. The only fingerprint SMCs in stock
(`0x83000021`..`0x83000028`) adjust chip select and sleep, and EL3 handles them
(source).

**What we can't see:** the pads' real function and level. If the secure side
leaves gpp4-6/7 in function 3, the normal-world HSI2C controller still drives
them. That is how the stock kernel's own I2C to the sensor would work while the
pads are locked (inferred). The same applies to whatever level gpg0-7 is held at.

## Paths

### (a) Non-secure raw IR capture, like the rear and front cameras

- **Needs:**
  1. The sensor reachable over I2C through the *hardware* USI07_I2C controller.
     Bit-banging is impossible.
  2. The sensor powered even though gpg0-7 is out of our hands.
  3. CSIS3 and its DMA usable from the normal world.
  4. The IR LED armed with hard limits. Without it, the sensor sees only ambient
     near-IR (sunlight works; a room at night shows nothing).
- **Work:**
  - A minimal HSI2C master for USI07_I2C, or a DT node for mainline
    `i2c-exynos5`. Mainline has no PERIC1 clock driver, so the gate has to be
    opened by hand.
  - The 5F1 table, which is already public.
  - CSIS3 in zrn-cam: RAW8, 2 lanes, 2400x2400.
  - FLED2 control with caps.
- **Risks:**
  - A TZPC-protected controller faults on first access. That is an SError: panic
    and a watchdog reset in about 48 s (observed earlier, see `camera/README.md`).
  - gpg0-7 may hold the module or its I/O off, in which case the sensor never
    answers.
  - The LED could fire uncapped if FLED2 is left armed (see Eye safety).
- **Verdict:** possible but unlikely. Settle it with the experiment below.

### (b) Using Samsung's secure path from Linux

- **Needs:**
  - The MobiCore driver and `mcDriverDaemon` on 7.1.
  - Kinibi initialised from Linux, which our boot never does today and which
    changes the secure-handoff picture for suspend.
  - Samsung's iris TA, which:
    - is signed;
    - is bound to the device REK and keymaster;
    - expects Samsung's HAL protocol, ION secure heap buffers, and an Android
      `bio` stack around it.
  - fimc-is's secure scenario.
- **Even if it ran,** the TA only returns match or no match and a downscaled
  preview. There is no IR camera in it.
- **Verdict:** **not feasible.** Months of reverse engineering against a closed,
  signed component, with risk to suspend and the boot path, for a feature we
  could not inspect.

### (c) Neither

Keep the iris camera off. Leave gpg0-7, gpp4-6/7 and S2MPB02 FLED2 alone. This
is today's state, and the default if the experiment fails.

## Uses, if (a) works

- **IR camera / face in the dark:** realistic. 2400x2400 mono at 30 fps with
  synchronised IR pulses gives usable faces at 20 to 35 cm in darkness. The
  shell could use it for an "is someone looking" check or a night selfie.
- **Iris unlock in our own code:** possible with open matchers (Daugman-style
  segmentation plus Gabor codes, e.g. OSIRIS or USIT ports), and honestly
  weak:
  - **Matching runs in the normal world.** Root, or any bug in the shell, can
    read the template, replay a frame or skip the check. Samsung keeps both
    capture buffers and templates in secure memory, and the result goes into
    keymaster.
  - **Liveness:** we would have none. A printed IR photo of an eye is the
    classic attack on iris scanners (it beat the Galaxy S8 publicly in 2017).
    Samsung's own liveness is in the TA and is not documented.
  - It can never unlock keys or the disk. At most it is a convenience screen
    unlock with a PIN fallback and a lockout counter.

## Eye safety

- **The emitter:** an LED, rated **IEC 62471 "Exempt"**. Samsung says that rating
  holds only for their operating envelope:
  - scans at 20 to 35 cm, within about 1 s;
  - recognition stops after 10 s of looking at the LED, or when the face is too
    close (public: Samsung support pages). `libIrisSensorListener.so` reads the
    light sensor for this (observed).
- **Stock's limits:**
  - **Kernel defaults at probe** (source, `leds-s2mpb02.c`):
    - CUR2 = 0xAF, commented "1050 mA". The IR current code is the high nibble
      (code 0xA). `s2mpb02_ir_led_current(v)` writes `(v-1)<<4 | 0xF`.
    - IRON1/2 = 0x19/0x0B: pulse-width code 100 (`IRON1 = w>>2`,
      `IRON2[7:6] = w&3`), and max-time code 0x0B in IRON2[4:0].
    - IRD1/2 = 0x00/0x2C: delay 0; 0x2C is reserved bits.
    - CTRL2 = 0x38: IR mode, hardware trigger.
  - **Runtime values:** the TA's auto-exposure computes them and sends them
    through the HAL. They are not in any string; the units of the width, delay
    and max-time codes are not in source.
  - **Hardware cap:** the chip has an **IR max-on timer**, enabled by TIME2 bit 0,
    with the limit in IRON2[4:0] (`s2mpb02_ir_led_max_time`).
  - **Continuous IR:** the factory-only torch mode
    (`LEDS_IRIS_IRLED_CERTIFICATE_SUPPORT`) is **off** in the S9 build, so stock
    never runs the IR LED continuously.
- **Rules for any code of ours:**
  1. Never put FLED2 in torch mode.
  2. Keep TIME2's max-timer enabled, with its limit no larger than stock's
     (code 0x0B).
  3. Cap the current at or below stock's code 0xA (1050 mA), and the width at or
     below code 100.
  4. Limit the session to 10 s.
  5. Allow it only with the proximity sensor reading "far".

  Rules 1 to 3 belong in the driver, rejected by the setter, not in the shell.
  Until the units are measured, run only at a fraction of stock's current.
- **Warm reboots:** our kernel never touches CTRL2. A warm reboot keeps whatever
  Samsung's kernel left (as `leds/zrn-flash.c` found for LDO9). FLED2 may sit
  **armed at 0x38 / 0xAF**, and any strobe edge from a streaming iris sensor
  would then fire 1 A pulses. Before any iris stream, read CTRL2 and disarm FLED2
  if it is set.

## Smallest safe experiment

**Question:** with the pads locked as they are, does the S5K5F1 answer on the
hardware USI07_I2C controller?

**Setup:** run attended, inside `ztlock`, from a module (zrn-cam's pattern),
with a pstore mark so that a hang is not retried. Stop at the first failure.
**No SMC is made at any stage. No GPIO protection is touched. gpg0-7 is not
written.**

| Step | Access | Kind | Why |
| --- | --- | --- | --- |
| 1 | S2MPB02 0x59 read 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B (bit-banged gpp6, as zrn-flash) | I2C read | See whether FLED2 is armed |
| 1a | if 0x13 & 0xF0 is non-zero: write 0x13 ← 0x00 | I2C write | Disarm the IR LED before the sensor ever streams |
| 2 | gpg2-3 output low (reset) | GPIO | stock order |
| 3 | S2MPS18 via ACPM: 0x72 (LDO41) on, 0x71 (LDO40) on, 0x70 (LDO39) on, voltage untouched (1.05 V code 0x1c) | PMIC write, as zrn-cam's existing `iris-rails` | power |
| 4 | gpg2-3 high; gpc1-6 CON = 2 (MCLK2, 26 MHz as booted) | GPIO | as stock |
| 5 | CMU_PERIC1 (base 0x10800000) **read** 0x2050, 0x2090, 0x20DC, 0x20E0, 0x3034 | devmem read of CMU | Are the USI07_I2C gates open? |
| 5a | if closed: set the enable bit in 0x2050 (GATE_CLK_PERIC1_USI_I2C), 0x20DC (USI07_I2C IPCLK), 0x20E0 (PCLK), 0x2090 (RSTNSYNC). Take the bit layout from cmucal and confirm it before approval | CMU write | clock the controller |
| 6 | **One read of 0x108D0000 + 0x00 (CTL)** | SFR read | **The risky step.** If USI07_I2C is TZPC-protected this is an SError: panic, then a watchdog reset |
| 7 | if 6 survives: USI_CON (0xC4) clear reset; CTL master; TIMING for 400 kHz; one combined write+read to 0x10: reg 0x0000, 2 bytes (0x5F1x expected, inferred from the part number), then 0x0002 (version) | SFR write; I2C transfer | **Answers the question.** No register in the sensor is written |
| 8 | power down in reverse; gate restored; LDOs off; reset low; MCLK pad back to output low | | |

**Outcomes:**
- **Step 7 returns an ID:** path (a) is open. Next comes CSIS3 capture with FLED2
  kept off, then a separate approval for the LED.
- **Step 7 times out or NACKs:** the pads or iris_en block it, and path (c) is
  final.
- **Step 6 faults:** the controller is TZPC-protected, and path (c) is final.

**Approvals the owner would give:**
- S2MPB02 writes: only 0x13 ← 0x00.
- ACPM writes to 0x70–0x72: the enable bit only.
- CMU_PERIC1 writes: four gate enables at 0x10802050/90/DC/E0.
- USI07_I2C SFR reads and writes at 0x108D0000 (CTL, USI_CON 0xC4, FIFO_CTL,
  TIMING, ADDR, TXDATA, RXDATA, INT_STAT).
- One I2C read transaction to 0x10.
- No SMC.

### The code (approved 2026-11-12, attended; not yet run)

- `probe/iris-seq.h` holds steps 1–8. `probe/test-iris-seq.c` runs them on
  simulated hardware and fails on any access outside the approved list, on any
  write to 0x13 other than 0x00, and on any sensor register write.
- `probe/zrn-iris-probe.c` is the module glue: S2MPB02 through zrn-flash's
  i2c-97, ACPM for the LDOs, gpiolib for gpg2-3, and ioremap for the rest.
- `probe/build.sh` runs the host test and builds the module against
  `build/linux-7.1-dp/out-target`, whose config hash matches the phone's.
- `fp-irq-probe.c` is the fingerprint experiment: GPIO character device v2,
  userspace.
- `run-fp.sh` and `run-iris.sh` run on the phone. They refuse outside
  `ztlock run iris`, before 07:00, below 15 % battery, with `zrn_cam` loaded,
  or with a mark left by an unfinished run.

Clock gates (step 5a), from `cmucal-sfr.c` (`SFR_ACCESS`) and `ra.c`
`ra_set_gate()`:

- Each of 0x10802050, 0x10802090, 0x108020DC and 0x108020E0 has bit 20 MANUAL,
  bit 21 CG_VAL and bit 28 ENABLE_AUTOMATIC_CLKGATING.
- To open a gate: if MANUAL is 1, set bit 21; if MANUAL is 0, clear bit 28.
- Each register gets its saved value back at step 8.
- 0x10803034 (Q-channel) is only read.

## Size

- **The experiment:** about 250 lines added to zrn-cam (a polled HSI2C master
  plus the step list), and one attended hour.
- **If it passes, IR capture:**
  - CSIS3 RAW8 in zrn-cam: about 300 lines.
  - A "nir" source in `ztmetro/camera`: small.
  - Capped FLED2 control: about 200 lines.

  Two to three weeks, attended.
- **Iris unlock on top:** a matcher port plus enrolment UI, three to six weeks,
  with the security caveats above.
- **If it fails:** nothing more to do.

## Proposals for the camera agent (not applied)

- In `camera/README.md`, the iris line should say that the I2C pads gpp4-6/7 are
  also secure-owned. Bit-banged I2C on them cannot work, whatever gpg0-7 does.
- zrn-cam's `iris` cycle should stay refused (`iris_allowed`) until the
  experiment above has run. It should also read S2MPB02 0x13 and refuse to
  stream while FLED2 is armed.

## Sources

- Samsung 9810 kernel (LineageOS mirror, `lineage-20`):
  - `drivers/media/platform/exynos/fimc-is2/sensor/module_framework/modules/fimc-is-device-module-5f1.c`
  - `.../fimc-is2/fimc-is-device-sensor.c`
  - `.../fimc-is2/fimc-is-video-sensor.c`
  - `drivers/leds/leds-s2mpb02.c`
  - `include/linux/leds-s2mpb02.h`
  - `include/linux/mfd/s2mpb02-private.h`
  - `include/dt-bindings/clock/exynos9810.h`
  - `drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c`
  - `drivers/pinctrl/samsung/pinctrl-{samsung,exynos}.c`
  - `arch/arm64/configs/exynos9810-starlte_defconfig`
- Stock DT from `stock/BOOT.img`. Vendor and system binaries read on the phone
  (read-only mounts `/mnt/vendor` and `/mnt/system`).
- [Samsung: Galaxy S9 iris IR safety](https://www.samsung.com/lb/support/mobile-devices/galaxy-s9-s9-plus-is-there-harm-if-we-use-the-ir-for-iris-authentication/)
- [Osram iris LED press release](https://ams-osram.cn/news/press-releases/pr-23-02-2017a)
