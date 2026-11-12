# Fingerprint sensor on the Galaxy S9: feasibility (2026-11-12)

Same rules and labels as [README.md](README.md): **observed**, **source**
(Samsung's 9810 kernel), **inferred**. Nothing was written to the phone. There
were no SPI transfers and no SMCs.

## Verdict

**Not feasible as a fingerprint reader. Confidence about 85 %.**

- The sensor's four SPI pads are secure-owned from boot. They read as zero and
  ignore writes, like the iris pads.
- The SPI controller is marked `secure-mode`, which the stock kernel never
  touches.
- No open driver exists for this chip.
- The finger-detect interrupt on its own is very probably no use either (about
  75 %): the sensor's detect mode is armed over SPI by the trusted app. One cheap
  experiment, with no SPI and no SMC, would settle that.

## Hardware

| Piece | Value | Evidence |
| --- | --- | --- |
| Sensor | **Egis ET510** (capacitive, under the camera), `etspi,et5xx`, `etspi-chipid = "ET510"`, orientation 0. Builds compile `CONFIG_SENSORS_ET5XX=y` only | source: DT, defconfig |
| Variants the driver knows | ET510C (regs 0x20/21/23/24 = 00 66 00 33), ET510D (0xFD-0xFF = 03 0A 05), ET516B, ET520, ET523. Register 0x00 reads 0xAA when ready | source: `et5xx-spi.c` |
| SPI | `spi@10450000`, which is **USI00** in PERIC0. Clocks GATE_USI00 (202) and USI00 (221). PDMA0 channels 1/0, GIC SPI 0x18e, max 13 MHz, `secure-mode` | source: DT, `dt-bindings/clock/exynos9810.h` |
| SPI pads | gpp0-0 CLK, gpp0-1 MOSI, gpp0-2 MISO, gpp0-3 CS (also `cs-gpio`) | source |
| Data-ready IRQ | **gpa1-3**, an alive wake-up EINT. Pull-down in sleep | source; observed: reads normally |
| Sensor LDO | **gpm43-0** (`etspi-ldoPin`), output. Reads normally, currently low | source; observed |
| Sleep/reset | **pcal6524@0x22 pin 6** (`etspi-sleepPin`), the I/O expander on hsi2c@10930000. Mainline already has it as `gpiochip50` (96-0022) | source; observed |
| Power-on | pins on, LDO high, 1.1 ms, sleep high, 10 ms | source: `etspi_power_control()` |
| Power-off | SMC `MC_FC_FP_CS_SET`, sleep low, LDO low, pins off | source |

## Secure world

| Question | Answer | Evidence |
| --- | --- | --- |
| Are the SPI pads secure-only? | **Yes.** gpp0-0..3 read zero in every field, including CON_PDN and PUD_PDN, while gpp0-4..7 show normal values. They are part of the 13-pad secure set described in README.md | observed |
| Is it switched by a runtime SMC, or locked from boot? | **Locked from boot.** The state is there on a boot where Kinibi was never initialised (`MC_FC_INFO` = 0). The stock kernel's SMCs are only PM and chip-select fixups around the locked state (below) | observed; source |
| Is the SPI controller protected? | Probably. With `secure-mode` the stock driver never initialises the controller (`need_hw_init`, no `s3c64xx_spi_hwinit`). The kernel only switches its clocks on for the TA (`FP_SET_SPI_CLOCK`) | source; inferred: TZPC |
| What the kernel does in stock | Power, reset, the IRQ (`INT_TRIGGER_INIT` arms a wake IRQ; `poll()` reports it), clocks and wakelocks. Register and frame ioctls return 0 in secure builds (`etspi_io_get_frame` → `return 0`) | source |
| Stock SMCs | `MC_FC_FP_PM_SUSPEND` 0x83000021, `PM_RESUME` 0x83000022 (called from pinctrl resume), `PM_SUSPEND_RETAIN` 0x83000026, `CS_SET` 0x83000027, `PM_SUSPEND_CS_HIGH` 0x83000028. These are SiP fast calls, which EL3 handles | source; inferred: EL3 |
| What the TA does | `ffffffff00000000000000000000002e.tlbin` holds Egis code for ET510/516/520/522/523. It drives the sensor over secure SPI, calibrates (`et510_calibrate_detect_mode`, detect thresholds), captures, and matches ("CrossMatch Verifier 300"). It wraps templates with `tlApiWrapObjectExt` under a REK-derived key; `bauth` handles auth tokens | observed: strings |
| Where templates live | Sealed blobs in the normal world: `/data/vendor/biometrics/fp/User_%d/*_%dtmpl.dat`, calibration in `/data/vendor/biometrics/meta/calib.dat` and `cbge_*.dat`. **Not EFS.** ztOS's root is that same userdata partition (`/dev/sda25`), so stock's enrolments are already gone | observed: `libbauthserver.so` strings; inferred: sda25 |

We must not touch EFS or any protected partition. Nothing here needs to.

## Paths

- **(a) Non-secure capture, matching in our own code:** **not feasible.**
  - SPI cannot reach the sensor: the pads are locked, and the controller is
    probably TZPC-protected.
  - Even if it could, the driver and matcher would have to be written from
    scratch:
    - Samsung's *factory* build talks to the sensor from the kernel. Its source
      gives the opcodes (`OP_REG_R` 0x20, `OP_REG_W` 0x24, image read, NVM,
      VDM), but sensor setup and calibration live in the TA.
    - libfprint supports Egis only over USB (0570, the Match-on-Chip parts,
      ES603). It has no SPI ET5xx driver.
- **(b) Stock TEE path from Linux:** **not feasible.** Like iris: it needs the
  Kinibi MobiCore driver and daemon on 7.1, Kinibi initialised from Linux,
  Samsung's signed Egis TA, `bauth` and keymaster, and the Android HAL protocol.
  The TA only returns match results.
- **(c) Neither:** **this is the verdict.** Leave gpp0-0..3, the SPI controller and
  the five fingerprint SMCs alone.

## Uses, honestly

- **Unlock, and sudo/polkit through fprintd:** only with (a). Matching in the
  normal world would be a convenience unlock, not a security boundary. The
  template sits where root can read it, and the matcher can be bypassed.
  Capacitive sensors in this class are also routinely fooled by moulded fakes
  without the vendor's anti-spoofing. Never use it for disk or key unlock, nor
  for sudo without a password fallback that you consider stronger.
- **Gestures** (swipe for the notification shade): Samsung does these in the
  TA's navigation mode. Not available.
- **Finger-detect IRQ alone:** the line itself is ours: gpa1-3, wake-capable,
  reads normally. But the TA arms detect mode over SPI and calibrates its
  thresholds, so after a plain power-on the sensor most likely stays silent
  (inferred). If it did pulse on touch, it would be a useful "tap to wake" key.
  Only the experiment can say.

## Smallest safe experiment

**Question:** does the sensor raise data-ready on a finger with no SPI
configuration?

**Setup:** attended and short, inside `ztlock`. No SPI, no CS, no SMC, no
pinctrl change to gpp0.

| Step | Access | Kind |
| --- | --- | --- |
| 1 | Read gpa1-3 level (gpiochip22 line 3) | GPIO read |
| 2 | gpm43-0 (gpiochip46 line 0) output high: the sensor LDO | GPIO write (CMGP pinctrl 0x14220000, bank 0x260 CON/DAT) |
| 3 | wait 1.1 ms; pcal6524 96-0022 line 6 output high: sleep/reset released | GPIO write, i.e. I2C writes to the expander's output and config registers through the mainline driver |
| 4 | wait 10 ms; request a both-edge IRQ on gpa1-3 (or poll its level). Count edges for 30 s while the owner touches and lifts | GPIO/IRQ read |
| 5 | sleep low, LDO low, IRQ freed | GPIO write |

**Outcomes:**
- **Edges that follow the touches:** a usable touch key, no imaging.
- **No edges, or one edge at power-up only:** the IRQ is unusable without the TA.
  Close the item.

**Approvals:** two GPIO lines (gpm43-0 and expander line 6) driven high then low,
and one IRQ requested on gpa1-3. Nothing else.

**Not proposed:** a read of the USI00 SPI registers at 0x10450000. It risks an
SError, and with the pads locked it could not reach the sensor anyway. It would
only become worth asking for if the iris experiment shows that a locked pad
still carries its peripheral function.

## Size

- The experiment: about 80 lines (a small module, or `gpioset` and
  `gpiomon`), half an hour attended.
- If the IRQ works: a wake-key driver, about 150 lines, plus shell wiring.
- Everything else: not pursued.

## Sources

- Samsung 9810 kernel (LineageOS mirror, `lineage-20`):
  - `drivers/fingerprint/et5xx-spi.c`
  - `drivers/fingerprint/et5xx-spi_data_transfer.c`
  - `drivers/fingerprint/et5xx.h`
  - `drivers/fingerprint/fingerprint.h`
  - `Makefile` (`ENABLE_SENSORS_FPRINT_SECURE` unless `CONFIG_SEC_FACTORY`)
  - `drivers/spi/spi-s3c64xx.c`
  - `drivers/pinctrl/samsung/pinctrl-samsung.c`
  - the starlte defconfig
- Stock DT from `stock/BOOT.img`. `/vendor` binaries read on the phone.
- [libfprint supported devices](https://fprint.freedesktop.org/supported-devices.html)
