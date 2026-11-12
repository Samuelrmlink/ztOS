# The 3.5 mm jack: headphones, headsets and their buttons (ROADMAP 8.35)

Status 2026-11-12: researched, built and tested on the host. Nothing has run on
the phone yet. The driver code is in `snd-zrn-abox`, behind `headset=0`. That
default writes nothing new to the codec, because the codec register list below
is waiting for the owner's approval.

**The short answer.** The jack can do all of it:

- tell 3-pole headphones (TRS) from a 4-pole headset (TRRS);
- use the headset's microphone;
- read the play/pause, volume up and volume down buttons, plus a voice button
  if the headset has one.

The CS47L93 codec does all of this in hardware. Only a codec driver was
missing.

## Wiring, from the stock device tree

Sources:

- `stock/BOOT.img`, unpacked with `camera/iris/stock-dt.py` (branch
  `research/iris`). The values are the same in Star rev16 to rev26.
- Samsung's GPL kernel: `third_party/vendor-kernel`, LineageOS exynos9810
  `6a9461f`. Files: `drivers/extcon/extcon-madera.c`,
  `sound/soc/samsung/star_madera.c` and `include/dt-bindings/extcon/extcon-madera.h`.
- Stock `mixer_paths.xml` for starlte (a vendor dump: `ExyHyperBrick/android_device_samsung_starlte`).

| What | Where | Stock DT / source |
|---|---|---|
| Codec | CS47L93 on `spi@10470000` CS0, ID 0x6371 | `cs47l93@0` |
| Codec interrupt | **gpa2-2**, low | `interrupts = <2 0 0>` on gpa2; pinctrl `codec-irq` (function 0xf EINT) |
| Codec reset | gpg1-0 | `reset-gpios`; `codec-reset` keeps its state in sleep (CON_PDN 3) |
| Headphone output | **HPOUT2 L/R (OUT2)**, not HPOUT1 | `samsung,routing` "HEADPHONE" = HPOUT2L/R; `route-headphone`: HPOUT2L/R Input 1 = AIF1RX1/2; accdet `reg = <2>` |
| HPOUT2 ground feedback | HPOUTFB2 | `micd-configs` hp_gnd 1 |
| Series resistance on HP | 33 Ω | `cirrus,hpdet-ext-res = <3300>` (x100) |
| Jack detect | the codec's **JD1 and JD2** pins, through the MICD clamp | `cirrus,jd-use-jd2`, `cirrus,micd-clamp-mode = <8>` (JD1 low and JD2 low = no jack) |
| Mic detect channel | **MICDET1** sense, **MICDET2** ground | `micd-configs = <0 1 4 0 1>`: src 0, gnd 1 |
| Headset mic bias | **MICBIAS2A**, 2.8 V, external capacitor, soft start, active discharge | `micd-configs` bias 4; `MICBIAS2` 2800000 µV, `MICBIAS2A` |
| Headset mic input | **IN1BL**, differential | `routing` HEADSETMIC → IN1BL via MICBIAS2A; `cirrus,inmode` IN1B = 0; `cirrus,hs-mic = <1>`; `route-headset-mic`: IN1L Mux B |
| Button ladder | ≤116 Ω KEY_MEDIA, ≤173 KEY_VOICECOMMAND, ≤321 KEY_VOLUMEUP, ≤752 KEY_VOLUMEDOWN, ≤1257 KEY_SEARCH | `cirrus,micd-ranges` |
| Extra switch | the codec's GP switch 1 in mode 3 | `cirrus,gpsw = <3 0>` |
| Polarity GPIO | **none**: there is no OMTP/CTIA swap switch | one `micd-configs` entry, no `cirrus,micd-pol-gpios` |
| Moisture check | HPDET on JD2, 2000 Ω, debounce 100 | `cirrus,moisture-pin/imp/debounce` |
| 32 kHz clock | MCLK2 | madera-core always sets `CLOCK_32K_1` to MCLK2; stock's FLL2 also references MCLK2 at 32768 Hz |
| Supplies | AVDD/DBVDD1/CPVDD1 = LDO33 (always on), DCVDD/CPVDD2 = LDO34 | as `snd-zrn-abox` already uses them |

The jack has no separate detect GPIO. Detection is in the codec, and the SoC
hears about it only through the codec's interrupt on gpa2-2.

## How stock does it (`extcon-madera.c`, with the values above)

1. **Insertion.** JD1/JD2 move, and the MICD clamp's "rise" status clears.
   Mode 8 means a jack is present when the raw status `MICD_CLAMP_RISE_STS1`
   is 0. Stock keeps a wakelock for `jd-wake-time` (5 s) and waits
   `init-delay-ms` (30 ms).
2. **Moisture check.** Stock first runs HPDET on JD2 against 2000 Ω, to tell
   a wet jack from a plug. (Not done here; see Plan.)
3. **Microphone or not.** This uses `micd-software-compare`:
   - MICVDD comes up. The clamp override is lifted. MICBIAS2A is forced on.
   - MICD runs in ADC mode, sensing MICDET1 against MICDET2.
   - When the MICDET interrupt fires, stock waits `micd-detect-debounce-ms`
     (300 ms), stops MICD and reads `MICDET_ADCVAL`, which a 65-entry table
     turns into ohms.
   - **1258 Ω or more, or an open circuit, means a microphone: a TRRS
     headset.** With `micd-open-circuit-declare`, open counts as a mic.
   - **Below that, the sleeve is grounded with the ring: TRS headphones.**
   - The timeout is 1000 ms. If it runs out, stock reports headphones.
4. **OMTP and CTIA.** Stock tries only one configuration and has no polarity
   switch, so it does **not** handle OMTP. An OMTP headset (mic and ground
   swapped) reads as a short and is treated as plain headphones: sound but
   no mic. That matches the hardware: Samsung phones are CTIA.
5. **HPDET.**
   - Stock then measures the headphone impedance on HPOUT2L, sensing HPDET2,
     with calibration trims from the codec's OTP.
   - The result picks `star_madera.c`'s gain step, the "HPOUT2L/R Impedance
     Volume" controls (`imp_table`), and the low-impedance tuning.
   - It does not change the plug type.
6. **Buttons.**
   - With a headset in, MICD runs continuously in level mode.
   - The `micd-ranges` thresholds go into Mic Detect 1 Level 1-4 (register
     values below).
   - The codec interrupts on a change, and stock reads which level fired.
   - Debounce: `micd-dbtime` 1 in hardware, plus a 300 ms software debounce
     on detection.
   - The microphone input is muted while a button is held.
7. **Reports.**
   - extcon cables `MECHANICAL`, `JACK_HEADPHONE` and `JACK_MICROPHONE`.
   - `CONFIG_EXTCON_MADERA_INPUT_EVENT=y`: an input device "Headset" with
     `SW_JACK_PHYSICAL_INSERT`, `SW_HEADPHONE_INSERT` and
     `SW_MICROPHONE_INSERT`, and the range keys.
   - Android maps KEY_MEDIA to HEADSETHOOK: play/pause, and answer or end a
     call.

## What mainline (7.1) has

- **`drivers/mfd/madera-core.c` and `madera-spi.c`**:
  - supports CS47L93 (`cirrus,cs47l93`);
  - sets the 32 kHz clock from MCLK2;
  - provides the codec's interrupt controller (`madera-irq`).
- **`sound/soc/codecs/cs47l92.c` and `madera.c`**:
  - the whole audio path (HPOUT1-3, IN1-4, the AIFs and the DSP);
  - HPDET clamp hooks (`out_clamp`, `MADERA_NOTIFY_HPDET/MICDET`).
- **No accessory detection.**
  - Cirrus's `extcon-madera` was never merged. Mainline has
    `arizona-jack.c`, which serves the older WM5102/WM5110 family only.
  - On mainline, the madera machine drivers have no jack, MICDET or button
    code at all.
- **Our kernel**:
  - is built without `CONFIG_MFD_MADERA` and has no SPI controller node for
    the codec;
  - drives the codec with `snd-zrn-abox`'s own bit-banged SPI (gpp0-4..7);
  - has the register map and bit definitions in
    `include/linux/mfd/madera/registers.h`, which the new code uses.

So bringing in mainline's madera stack would give the audio routing, but not
the jack. It would also mean replacing the codec half of `snd-zrn-abox` (SPI
controller, DT, clocks, and a machine driver) for no gain in detection. The
plan instead ports the parts of `extcon-madera.c` the S9 uses onto the codec
access `snd-zrn-abox` already has.

## What was built

| Piece | File | What it does |
|---|---|---|
| Driver | `audio/driver/zrn-headset.c`, included by `snd-zrn-abox.c` | Its own file, so the DSP work in that driver (the power agent's idle shutdown) doesn't touch the same lines. Hooks in `snd-zrn-abox.c`: prototypes, one line in `amp_set`, one in prepare, close, `codec_stop`, `cap_path_on/off`, `hs_init()` and `hs_exit()`. |
| Logic | `audio/driver/zrn-headset-logic.h` | The arithmetic (levels, ADC to ohms, TRS/TRRS, button decoding, the headphone cap), shared with the host test. |
| Daemon | `devices/samsung-starlte/overlay/usr/local/bin/ztos-headsetd`, a user service | The headphone output in PipeWire and pausing on unplug. |
| udev | `70-zrn-headset-jack.rules` | `uaccess` on the jack's input device. |
| ztos-audiod | `rootfs/overlay/usr/local/bin/ztos-audiod` | Headphones are capped like Bluetooth; the jack's sink takes the audio. |
| Shell | `ztmetro/ztmetro.c` (one block in `key_event`) | KEY_PLAYPAUSE and KEY_MEDIA → `playerctl play-pause`. |

Module parameters:

| Parameter | Default | What it does |
|---|---|---|
| `headset` | 0 | 0: off, no codec writes beyond what `snd-zrn-abox` does today. 1: detection and buttons. Needs `capture=1`, as on the phone. |
| `hp_route` | 0 | Plays to HPOUT2 while headphones are in, instead of the speakers. |
| `headset_mic` | 0 | Records from IN1BL while a headset is in. |
| `hp_db` | -40 | The headphone level in dB, writable at run time. Clamped to the ceiling and to -64. |
| `hp_max_db` | -20 | Can only lower the ceiling. |
| `hs_mic_gain` | 12 | The headset mic's digital gain in 0.5 dB steps (+6 dB). |
| `headset_poll_ms` | 500 | How often the jack is polled. |
| `headset_button_ms` | 40 | How often the buttons are polled while a headset is in. |
| `hs_unplug_quiet_ms` | 1500 | How long the speakers stay off after an unplug. |
| `headset_state` | (read only) | What the jack is doing, for the bench. |

### Detection and buttons (`headset=1`)

- At load, it sets up accessory detection with stock's values (list A
  below).
- It polls the clamp's raw status every 500 ms on a deferrable timer, so it
  never wakes an idle CPU by itself. It is polled because using the codec's
  interrupt needs gpa2-2 set as an EINT, which isn't approved (list C).
- **On insertion**:
  - `SW_JACK_PHYSICAL_INSERT` is reported at once;
  - after 30 ms, one ADC reading through MICBIAS2A with a 300 ms settle, as
    stock;
  - then `SW_HEADPHONE_INSERT`, plus `SW_MICROPHONE_INSERT` for a headset.
- **With a headset**:
  - MICD stays on in level mode with stock's thresholds. Mic Detect 1
    Control 3 is read every 40 ms;
  - a level needs two equal readings to count;
  - keys are reported as `KEY_PLAYPAUSE` (stock's KEY_MEDIA range),
    `KEY_VOICECOMMAND`, `KEY_VOLUMEUP` and `KEY_VOLUMEDOWN`;
  - the fifth stock range (753-1257 Ω) is measured but reports nothing;
  - while a button is held, the headset mic is muted, as stock.
- **On removal**: MICD, the bias and MICVDD go off, the clamp override comes
  back on, and the keys and switches go to 0.
- **The input device** is "S9 Headset Jack". libinput sees it as a keyboard,
  so ztmetro gets its keys like any other keyboard's:
  - the volume buttons are the phone's own KEY_VOLUMEUP/DOWN. They step the
    volume streams (media while something plays, else the ring), with the
    screen on or off, and snooze a ringing alarm. Nothing new was needed for
    them;
  - play/pause runs `playerctl play-pause`.
- **Codec reset.** The codec loses its registers over a SoC sleep
  (`audio/CALL-AUDIO.md`, 2026-10-30). Each poll checks a register that is
  only set when the setup is done (`JACK_DETECT_ANALOGUE`). If it has gone,
  the codec is set up again and a plug still in is measured again. Its
  switches don't change, so nothing is reported for it.
- **No ABOX access.** Detection never touches ABOX, so it works with the DSP
  off (the power agent's idle shutdown).
- **What doesn't work yet:**
  - buttons and plug changes while the SoC is asleep. They are seen at the
    next wake. Waking on a button press needs the interrupt (list C);
  - answering or ending a call with the button. ztmetro would need the call
    state where it handles KEY_PLAYPAUSE.

### Headphone output (`hp_route=1`)

- Stock's path:
  - ABOX: SIFS0, the mix of every stream, also goes out of UAIF0
    (`route-sifs0-to-uaif0`, `ROUTE_CTRL0[3:0] = 1`, UAIF0 speaker enable);
  - codec: AIF1RX1/2 → OUT2L/R mixers → HPOUT2.
- The codec's clocks: its FLL locks on UAIF0's bit clock, so the capture
  path is started for that (`cap_path_on`), as calls do. `cap_path_off`
  leaves it running while the headphones play.
- The speakers are held off while the jack plays (`amp_set` asks
  `hs_speakers_held_off()`).
- **Unplugged while playing:**
  - the headphones stop at once;
  - the speakers stay off for 1.5 s (`hs_unplug_quiet_ms`) or until the
    next stream starts;
  - ztos-headsetd pauses every player, so music doesn't jump to the
    speaker.
- Calls are not routed to the jack yet (`call_route` keeps its own path).
- **No HPDET**, so no impedance-based gain step or low-impedance tuning
  (Plan).

**Hearing safety:**

- **The cap.** The OUT2 digital volume never goes above
  **-20 dB** (`HS_HP_CEIL_DB`, 0x58), whatever is asked. That is 20 dB under
  the codec's own 0 dB full scale. The module parameter can only lower it,
  and the host test checks every value from -200 to +200 dB.
- **The start.** It starts at **-40 dB** (`hp_db`). The output is enabled
  muted, then unmuted with an 8 ms per 6 dB ramp up (`OUTPUT_VOLUME_RAMP`
  VI=5). It goes down at 1 ms per 6 dB and is muted before it is switched
  off.
- **PipeWire.**
  - PipeWire's volume comes on top of this, never above it. The first time
    the "Headphones" output appears, ztos-headsetd sets it to **40 %**
    (-24 dB on wpctl's cubic scale). After that, WirePlumber remembers what
    was set.
  - So the first sound is about -64 dB under full scale, and the most is
    -20 dB.
- **ztos-audiod.**
  - A stream on headphones is never above unity, as on Bluetooth.
  - A ring or an alarm turns the headphones up to at most **60 %**
    (`HEADPHONE_BOOST_MAX`). The speaker can be boosted above that, but
    headphones can't.
- **No test tones** are played to the jack by anything here.

### Headset microphone (`headset_mic=1`)

- While a headset is in and recording runs, AIF1TX1/2 take IN1L:
  - IN1L Mux B, differential, analogue mode, as stock's `route-headset-mic`;
  - +6 dB digital (`hs_mic_gain`);
  - the bias is MICBIAS2A, already on from detection.
- On removal, or when recording stops, the source goes back to IN3L/IN3R
  (DMIC1/DMIC3).
- The PipeWire source stays the one "Microphone" node; only what feeds it
  changes, as on a phone.
- MICVDD (the charge pump) is shared with the DMICs' MICBIAS1, so
  `codec_stop` leaves it on while the jack holds it.

### PipeWire and WirePlumber

- **The output.**
  - While headphones are in, and only while the driver plays to the jack,
    ztos-headsetd runs `pw-loopback` into the card. It makes an Audio/Sink
    `ztos-headphones`, "Headphones", `device.form-factor=headphone`,
    priority 1100.
  - It sits beside the speaker's crossover and limiter chain, which is made
    for two speakers and would take the bass out of the left ear.
  - ztos-audiod sees a new output named `ztos-headphones` and makes it the
    global output, as it does for Bluetooth, unless Bluetooth or USB audio
    is already there: then the choice is the user's.
  - On unplugging, the loopback goes, players are paused, and WirePlumber
    falls back to the speaker.
- **Why it only exists with the plug in.** A plain sink into the card
  without headphones would reach the speakers without the PipeWire high-pass
  and limiter. The kernel limits in README.md stay as they are either way.
- **Volume.** WirePlumber keeps the headphone volume separately from the
  speaker's (stream-properties, `Audio/Sink:media.name:Headphones`), so
  each output has its own level, as the volume-streams work expects (8.20).
- **The microphone** needs no PipeWire change: the driver switches what
  feeds the existing source.

## Codec registers for approval

These are every new CS47L93 write.

- **Addresses and fields:** mainline `registers.h`.
- **Values:** the stock DT through Samsung's `extcon-madera.c`, mainline
  `madera.c`/`madera-core.c`, and stock `mixer_paths.xml`.
- **Masked writes:** "mask→value" means read, modify, write; other bits are
  kept.
- **Defaults:** the reset defaults are from mainline `cs47l92-tables.c`.

**A. Detection setup, once per codec power-up (`headset=1`):**

| Reg | Name | Mask → value | Why | Who does the same |
|---|---|---|---|---|
| 0x100 | Clock 32k 1 | 0x0043 → 0x0041 | 32 kHz clock on, from MCLK2, for the detect logic | mainline `madera_dev_init` (always); vendor madera-core |
| 0x2C8 | GP Switch 1 | 0x0003 → 0x0003 | SW1_MODE 3 | stock DT `cirrus,gpsw = <3 0>`; `madera_extcon_probe` |
| 0x2A3 | Mic Detect 1 Control 1 | 0xFFF3 → 0x1742 | bias start time 1, rate 7, bias source 4 (MICBIAS2A), dbtime 1, not enabled | `micd-bias-start-time/rate/dbtime`, micd-configs bias; `madera_extcon_probe`, `set_mode` |
| 0x2A2 | Mic Detect 1 Control 0 | 0x80F7 → 0x0001 | sense MICDET1, ground MICDET2, level mode | micd-configs src 0 / gnd 1; `madera_extcon_set_mode` |
| 0x299 | Headphone Detect 0 | 0x0007 → 0x0001 | HPD ground MICDET2 | `madera_extcon_set_mode` |
| 0x41A | Output Path Config 2 | 0x0007 → 0x0001 | HPOUT2 ground feedback HPOUTFB2 (reset 0x0005) | micd-configs hp_gnd 1; `madera_extcon_set_mode` |
| 0x2A4 | Mic Detect 1 Control 2 | 0x00FF → 0x009F | button levels 0-4 reported | `madera_extcon_add_micd_levels` |
| 0x2A6 | Mic Detect 1 Level 1 | 0x3F3F → 0x3F3F | unused slots 6,7: max | same |
| 0x2A7 | Mic Detect 1 Level 2 | 0x3F3F → 0x3F3F | slot 4: 1257 Ω (63), slot 5: max | same, stock micd-ranges |
| 0x2A8 | Mic Detect 1 Level 3 | 0x3F3F → 0x333C | slot 2: 321 Ω (51), slot 3: 752 Ω (60) | same |
| 0x2A9 | Mic Detect 1 Level 4 | 0x3F3F → 0x252B | slot 0: 116 Ω (37), slot 1: 173 Ω (43) | same |
| 0x2C6 | MICD Clamp Control | 0x000F → 0x0008 | clamp mode 8: JD1 low and JD2 low = no jack (override bit 4 stays as reset, on) | `cirrus,micd-clamp-mode = <8>`; `madera_extcon_set_micd_clamp_mode` |
| 0x1A06 | Interrupt Debounce 7 | 0x0015 → 0x0015 | debounce MICD clamp, JD1, JD2 | `madera_extcon_set_micd_clamp_mode`, probe |
| 0x2D3 | Jack Detect Analogue | 0x0003 → 0x0003 | JD1 and JD2 comparators on (written last: the "set up" mark) | probe, jd-use-jd2 |

Values 0x2A4 and 0x2A6-0x2A9 are computed by `hs_compute_levels()`. The host
test checks them against a hand calculation from `madera_micd_levels`.

**B. Per plug, while one is in (`headset=1`):**

| Reg | Name | Write | When | Who |
|---|---|---|---|---|
| 0x2C6 | MICD Clamp Control | bit 4 (OVD) 0, then back to 1 | measuring and buttons; on removal | `madera_micd_start/stop` |
| 0x213 | LDO2 Control 1 | 0x07E0 → 0x0480 (3.0 V) | MICVDD for MICBIAS2 | already written by `codec_start` (capture) |
| 0x200 | Mic Charge Pump 1 | 0x0003 → 0x0001 on, 0x0002 off | MICVDD | already written by `codec_start/stop` |
| 0x219 | Mic Bias Ctrl 2 | 0x81E1 → 0x81A1 on (2.8 V, ext cap, enable); bit 0 → 0 off | MICBIAS2 | stock DT `MICBIAS2`; the regulator `extcon` forces on |
| 0x21E | Mic Bias Ctrl 6 | bit 0 → 1 on, 0 off | MICBIAS2A to the jack | `micd-force-micbias` |
| 0x2A2 | Mic Detect 1 Control 0 | bit 15 ADC mode 1 to measure, 0 for buttons | | `madera_micd_start` |
| 0x2A3 | Mic Detect 1 Control 1 | bit 0 MICD_ENA 1/0 | measuring, buttons | `madera_micd_start/stop`, `adc_read` |

Reads only: 0x1886 (raw status 7: jack present), 0x2AB (MICDET ADC value)
and 0x2A5 (MICD level, the buttons).

**C. Not codec, also needing approval (not used by the code as it stands):**

- **gpa2-2 as an external interrupt** (function 0xf, no pull, as stock's
  `codec-irq`). Instead of polling, this gives:
  - wake from sleep on a plug or a button;
  - no 40 ms button poll.

  It would also need these codec writes:
  - IRQ1 Mask 6 (0x1845): bit 8, MICDET1, cleared;
  - IRQ1 Mask 7 (0x1846): bits 4 and 5, the MICD clamp, cleared;
  - writes to clear IRQ1 Status 6/7.
- **gpg1-0 (codec reset) power-down state.** Stock holds it in sleep
  (CON_PDN 3). If it drops in sleep here, that would explain the codec
  resetting over every SoC sleep. Read the pin's sleep config first.

**D. Headphone output (`hp_route=1`, only while a stream plays and headphones are in):**

| Reg | Name | Write | Why | Who |
|---|---|---|---|---|
| 0x409 | Output Volume Ramp | 0x0077 → 0x0025 | down 1 ms/6 dB, up 8 ms/6 dB | stock `Output Ramp Up` 8ms/6dB (headset paths) |
| 0x419 | DAC Digital Volume 2L | mute+vol: 0x0130 muted at -40 dB, then 0x0030 | the level, clamped ≤ 0x58 (-20 dB) | `madera` OUT2L volume |
| 0x41D | DAC Digital Volume 2R | the same with VU (bit 9) | | |
| 0x690 | OUT2LMIX Input 1 Source | 0x20 (AIF1RX1); 0 off | stock `route-headphone` | |
| 0x698 | OUT2RMIX Input 1 Source | 0x21 (AIF1RX2); 0 off | the same | |
| 0x51A | AIF1 RX Enables | bits 0,1 on/off | AIF1 receive | `madera` AIF1RX widgets |
| 0x400 | Output Enables 1 | bits 2,3 (OUT2R/L) on/off, 6 ms settle | HPOUT2 | `madera_hp_ev`, `out_up_delay` 6000 µs for CS47L93 |

SoC (ABOX), with the existing capture path's UAIF0 setup:

- `ABOX_ROUTE_CTRL0` bits 3:0 = 1 (UAIF0 speaker ← SIFS0; 0 off);
- `ABOX_UAIF0_CTRL0` bit 0, speaker enable (`route-sifs0-to-uaif0`,
  `abox_uaif_trigger`).

**E. Headset microphone (`headset_mic=1`, while recording with a headset in):**

| Reg | Name | Write | Why |
|---|---|---|---|
| 0x310 | IN1L Control | 0x6400 → 0x4000 | IN1L from B, differential, analogue (stock `IN1L Mux` B, inmode IN1B 0) |
| 0x300 | Input Enables | bit 1 IN1L on/off | |
| 0x311 | ADC Digital Volume 1L | mute/vol 0x01FF → 0x008C, then VU (bit 9); bit 8 mute on removal or a button press | +6 dB; the mute as stock's `madera_hs_mic_control` |
| 0x700 / 0x708 | AIF1TX1/2 MIX Input 1 Source | 0x10 (IN1L); back to 0x14/0x15 (IN3L/R) | stock `route-headset-mic` |

### Order for the bench, once approved

1. Load with `headset=1` only, reading `headset_state` and the input device
   (`evtest`):
   - plug and unplug a TRS and a TRRS;
   - press each button;
   - read 0x100 and 0x1886 back.

   If the jack never shows present, MCLK2 may not carry 32 kHz; then list C
   and a look at the PMIC's 32 kHz outputs come next.
2. Then `headset_mic=1`: record through the headset.
3. Then `hp_route=1`: the owner listens, starting from the defaults above.

## Plan for what's left

- **The codec interrupt (list C)**: wake on plug and buttons, and drop the
  button poll.
- **HPDET**:
  - impedance and the stock gain step;
  - a line-out check;
  - CS47L93's low-impedance tuning (`madera_tune_headphone`);
  - the moisture check stock does before the mic check.

  These need the OTP trim reads (`madera_extcon_read_calibration`).
- **Calls.** The jack for voice: HPOUT2 and IN1BL on the voice path
  (stock `incall-headset`), and the button to answer and end.
- **Simultaneous output** for the ring and the alarm (stock
  `dev-speaker-headset`): today, with headphones in, they play in the
  headphones only, through ztos-audiod's boost cap.
- **Codec power.** With `headset=1` the codec stays out of reset with DCVDD
  on (as after the first recording today). Measure the idle cost with
  `ztos-power-measure`.

## Host tests

- `audio/tests/headset-test.sh`:
  - the C test of `zrn-headset-logic.h`: stock's register values, the
    ADC-to-ohm table, TRS/TRRS, button decoding, the headphone ceiling;
  - `headsetd-test.py`: device lookup, the switch bitmap, the loopback's
    properties, the first-volume rule, and that the sink name matches
    ztos-audiod's.
- `make -C ztmetro check-audiod check-volume-streams`: the headphone gain
  cap, the boost cap, and the jack taking the audio.
- The module builds with W=1 and no warnings, against
  `build/linux-7.1/out-target-snd` (7.1.0-zboot-wd-dp).
