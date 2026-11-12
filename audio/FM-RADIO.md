# FM radio on the Galaxy S9 (ROADMAP 8.36): research

Status 2026-11-12: research only. Nothing touched the FM chip or its bus (no
I2C reads either), as the task required.

**Verdict:** every software source for this phone (SM-G960F, Exynos) says it
has no FM receiver.

- Samsung's own device tree and kernel configuration for it have none.
- The `rtcfmradio@64` / LDO45 description in `sensors/HARDWARE-AUDIT.md`
  matches Samsung's **Snapdragon** S9 (SM-G960U/W, starqlte) and the Exynos
  9825 Note 10, not this phone.

Unless a board photo or an approved probe shows otherwise, there is nothing
to port, and 8.36 should close as "not on this model".

## What the sources say

| Source | Finding |
|---|---|
| Stock device trees in `stock/BOOT.img` (all twelve, Star rev16 to rev26, unpacked with `camera/iris/stock-dt.py`) | No `rtc6213n`, `rtcfmradio`, `fmradio` or `richwave` node, and no `fmint-gpio` or `fm-lna-gpio` |
| `hsi2c@104C0000` (label `hsi2c_11`, pins gpp1-6/gpp1-7, function 3) in the stock tree | Only the SX9320 grip sensor at 0x28 |
| The S2MPS18 regulators in the stock tree | They end at **LDO44** (`VDD_ESE_1P8`). There is no LDO45 and no `VDD_FM_2P8` |
| gpa2-1 (the audit's FM interrupt) | It is in stock's pinctrl `initial-state` list as an input with a pull-down: how Samsung parks unconnected pins |
| `exynos9810-starlte_defconfig` and `exynos9810_defconfig` (exynos-linux-stable, `tw10-android-4.9-q`; the audit's own source) | `# CONFIG_MEDIA_RADIO_SUPPORT is not set`, so no RTC6213N driver is built. The audit's "`CONFIG_RADIO_RTC6213N=y` in stock" is not in either |
| `exynos9810-starlte_eur_open_26.dts` and `_common.dtsi`, the same tree | Their `hsi2c_11` has only the SX9320. No FM node |
| LineageOS exynos9810 kernel (`third_party/vendor-kernel`, `6a9461f`) | Has the driver source, `drivers/media/radio/rtc6213n/`, about 2,600 lines. No star or crown DTS uses it |
| GitHub code search for `rtcfmradio` | Samsung's sdm845 kernels: `sdm845-sec-star-audio.dtsi`, `rtcfmradio@64` on `i2c_20`, `fmint-gpio` = tlmm 133 (r04: tlmm 145), `radio_vol` table. Also the Exynos 9825 `d1`/`d2` (Note 10) DTs |

What *is* on this phone, and is easy to mistake for FM support:

- The codec's machine node routes **"FM" → IN2BL and IN2BR**, and
  `cirrus,inmode` sets IN2B single-ended.
- Stock `mixer_paths.xml` has `dev-fm_radio-input` (IN2L/R Mux B → AIF3TX1/2)
  and `erap-fmradio-on`.

These are Samsung's common Exynos 9810 audio configuration, shared across
models. They show where an FM chip's analogue output would go on this codec,
not that one is fitted. IN2B may simply be unconnected on the G960F.

## How to settle it for certain (each needs the owner)

1. **A board photo.** Look for a Richwave RTC6213N (a small QFN) near the
   jack on a G960F main board, for example in a teardown.
2. **An I2C probe at 0x64 on hsi2c_11** (needs approval for the pins and the
   bus):
   - hsi2c_11 isn't in our device tree. The way `snd-zrn-abox` reaches the
     speaker amplifiers would work here too: an `i2c-gpio` bus on gpp1-6 and
     gpp1-7, which means reconfiguring those pins;
   - the SX9320 at 0x28 on the same bus should answer, which proves the bus;
   - a NACK at 0x64 would settle it.
3. **The PMIC.** Read the S2MPS18 register that would be LDO45's control.
   On this PMIC the stock tree describes none, so this proves little.

## If it were there: what a minimal "tune and play" would need

| Piece | What | Size |
|---|---|---|
| Bus | `i2c-gpio` on gpp1-6/7, as for the amplifiers, or the mainline `i2c-exynos5` with a DT node for 0x104C0000 (needs a boot image) | S |
| Power | the chip's supply and LNA enable, from the board's real wiring (unknown: the stock DT has none) | ? |
| Driver | Samsung's `radio-rtc6213n` is a V4L2 radio driver: power-up, band and spacing, tune via the channel register, seek, RSSI, RDS. Our kernel has no media core (`CONFIG_MEDIA_SUPPORT` unset in `out-target`). So either build `videodev.ko` out of tree, as was done for the sound core, or write a small driver with a sysfs interface (frequency, rssi) of about 400-600 lines | M |
| Audio | codec IN2BL/IN2BR, single-ended, through the ADC. Either to AIF1TX on the existing capture path (UAIF0 → WDMA → "Microphone"-like PCM), with PipeWire looping it to the output, or stock's AIF3 → ABOX route. New codec writes: IN2 mux B, IN2 mode, input enables, ADC volume, AIF1TX sources | M |
| Antenna | the headphone cable: 8.35 (jack detection and the headphone path) first, done here | — |
| App | a tuner UI in the shell | M |

The total, if the chip were fitted: **L**, one to two weeks. As things stand
it is **not applicable**.

## Alternatives that need no FM chip

- Internet radio through the existing music app.
- A USB FM receiver over USB-C (the `si470x` USB driver is mainline), with
  a hub for headphones.
