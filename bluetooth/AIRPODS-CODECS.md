# AirPods two-way audio: what's possible on ztOS (ROADMAP 8.60)

Research, 2026-11-22. Branch `feature/airpods-codecs`. Nothing below was sent to
the AirPods or the controller: every phone fact was read from what's there
(bluetoothctl, PipeWire, debugfs, bluetoothd's SDP cache, ztos-podsd's state file).

## The verdict

**Feasible, without Apple's call codec and without kernel changes.** The high-quality
path that open source has working is not an HFP codec. It is a second, proprietary
microphone stream: the AirPods send their microphone as **AAC-ELD, mono, about
80 kbit/s, inside AAP (Apple calls it AACP) on L2CAP PSM 0x1001**, while music keeps
playing over ordinary A2DP AAC. LibrePods' Linux PR #655 does this on a stock
BlueZ and PipeWire, as a user-space program. ztos-podsd already holds that same
L2CAP channel. Turning on the stream takes one AAP packet, decoding it takes
FFmpeg's own AAC decoder (LGPL), and PipeWire gets the result as a source node.

Two-way audio is therefore:

| Direction | Today (HFP) | mSBC (quick win) | AACP mic + A2DP (the Apple-style path) |
|---|---|---|---|
| Into the AirPods | CVSD, 8 kHz, mono | mSBC, 16 kHz, mono | A2DP AAC, 44.1/48 kHz stereo (music stays as it is) |
| From the microphone | CVSD, 8 kHz | mSBC, 16 kHz | AAC-ELD, mono, 64 kHz by LibrePods' account (see *Open questions*) |
| Latency | low (eSCO, ~30-40 ms) | low | higher: A2DP buffering on the way in (~150-250 ms typical); the mic stream itself is 7.5-10 ms frames |
| Phone calls (modem) | possible, but see *Calls* | possible | the voice firmware would need to reach the host's PipeWire graph |
| Needs the faster UART | yes (SCO over HCI) | yes | yes (A2DP + mic together) |

Not available on these AirPods: **LC3-SWB** (they announce HFP 1.8, not 1.9) and
**LE Audio** (no public evidence that any AirPods model ships it).

## The owner's AirPods

Read on the phone, 2026-11-22:

| What | Value | Where from |
|---|---|---|
| Model | AirPods Pro 2 (USB-C), A3047 | `Modalias: bluetooth:v004Cp2024d242F`; ztos-podsd's model table (`ztmetro/pods/pods-model.h`) and its state file |
| Firmware | 9B47 | `$XDG_RUNTIME_DIR/ztos-pods-state.json` (ztos-podsd, from AAP) |
| DID record | vendor 0x004C (source 1, Bluetooth SIG), product 0x2024, version 0x242F | SDP cache, record 0x1200 |
| HFP | **version 1.8** (0x0108), RFCOMM channel 7, SupportedFeatures **0x003B**: EC/NR, three-way calling, voice recognition, remote volume, **wideband speech (mSBC)**. Bit 8 (super-wideband, HFP 1.9) is clear | SDP cache, record 0x111E |
| A2DP sink endpoints | SBC; AAC; and a **vendor codec 0x0000004C / 0x8001** (Apple's own A2DP codec; USBPods-Pico2W streams AAC-ELD to AirPods through it) | bluetoothd's `[Endpoints]` cache |
| LE | not paired over LE; no LE Audio service seen | `bluetoothctl info` |

The ztOS side:

| What | Value |
|---|---|
| Controller | BCM4361B2, HCI 5.0 (version 9), Broadcom (manufacturer 15) |
| LMP features page 0 | `bf fe cf fe db ff 7b 87`: **transparent SCO** (byte 2 bit 3) and **eSCO** (byte 3 bit 7) both set, so PipeWire's adapter check for mSBC passes |
| Voice setting | 0x0060 (CVSD default) |
| UART | `btattach -B /dev/ttySAC0 -P bcm -S 115200`: 115200 baud at the time of writing |
| PipeWire card | profiles `a2dp-sink` (AAC, active), `a2dp-sink-sbc`, `a2dp-sink-sbc_xq`, and **`headset-head-unit` with CVSD only**. No mSBC profile is offered |
| Libraries | Arch-based; `libfdk-aac` 3.0.0 and FFmpeg 9.0.2 (`libavcodec.so.63`, native `aac` decoder) are installed |

## What Apple does with an iPhone

* **Calls on the iPhone still use HFP**, as the AirPods' SDP record shows (the only
  call profile they advertise). Apple's iOS 26 announcement promises "studio-quality
  audio recording" and more natural voice on calls, FaceTime and CallKit apps for
  AirPods 4 and AirPods Pro 2. It says nothing about the codec.
* **The high-quality microphone is AACP message 0x58.** Apple's dissector project
  (apple-wireshark) labels AACP 0x58 "Microphone Stream?". LibrePods' README says:
  "On iOS/iPadOS, you can continue using A2DP while AirPods send the hi-res (AAC-ELD
  mono 64kHz?) audio stream from its microphone over AACP." Whether the iPhone uses
  this stream for phone calls, or only for recording and the new iOS 26 modes, is
  not public. Only a capture from an iPhone would tell. It doesn't matter for ztOS:
  the stream is what's reachable.
* **Transport: ACL, not SCO/eSCO.** The stream runs inside the AAP L2CAP channel
  (PSM 0x1001, basic mode). Nothing in the air protocol changes, so the controller
  and kernel need nothing new.

### The 0x58 stream, as LibrePods PR #655 implements it

Source: `linux-rust/src/bluetooth/aacp.rs`, `aacp_audio.rs`, `audio/eld.rs`,
`audio/hires_mic.rs` at PR head `99b0e580` (2026-07-14), by Luan Ademi, with the
command sequence from Kavish Devar's captures. Facts, not code, are taken here.

| Step | Bytes |
|---|---|
| Start (host → pods) | `04 00 04 00 58 00 00 00 09 00 00 01 82 00 00 00 04 96 00` |
| Stop (host → pods) | `04 00 04 00 58 00 00 00 02 00 03 01` |
| Audio (pods → host) | SDU with `sdu[0]=04`, `sdu[2]=04`, LE16 at 4 = `0x0058`, LE16 at 6 = `0x0001`; a 22-byte header; then repeated `[timestamp u32 LE][length u8][one AAC-ELD access unit]` |
| Decoder setup | raw AUs, AudioSpecificConfig `F8 E6 30 00`; FFmpeg's native AAC decoder (`AV_CODEC_ID_AAC`) with `sample_rate = 48000`, mono |
| Output | 480 samples per AU, played as **64 kHz** mono (480 / 7.5 ms) |

What the PR does around it:

* Sends Start only when an application is actually recording from its virtual
  source, and Stop when it stops (poll every 400 ms). If no SDU arrives for 2 s
  it restarts the capture.
* **Turns conversation awareness off** while the mic runs (AAP control command
  0x28 ConversationDetectConfig = 2), and back on afterwards (maintainer's review
  comment: needed, not only cosmetic).
* **Resets the A2DP card profile 800 ms after Start** (card profile `off` then back
  to A2DP, through the PulseAudio API). Without it, testers heard music from only
  one bud. This is a profile switch on the owner's AirPods (approval needed; see
  *Next steps*).
* Receives with a 4096-byte buffer: "0x58 hi-res audio SDUs can exceed 1 KB".
  ztos-podsd's receive buffer is 2048 bytes and it doesn't raise the L2CAP
  receive MTU; both must be checked against the real SDU sizes (an SDU above the
  channel's MTU is dropped by the kernel; `recv` on SOCK_SEQPACKET truncates).
* Hands PCM to PipeWire through `module-pipe-source` and a FIFO (an earlier
  version used a null sink that leaked the microphone into the playback).
* Tested by its author and a reviewer on **AirPods Pro 3**. The owner's
  **Pro 2 (USB-C) is not confirmed** by anyone; firmware 9B47 is recent, and Apple
  lists Pro 2 for the iOS 26 recording feature, so it is likely but must be tried.

**Device ID spoofing.** The PR has no spoofing code and LibrePods' feature table
does not mark hi-res audio as needing it. But han-um's Pico 2 W port of the same
PR advertises an Apple DID record and says it is "required for the mic protocol"
(and that "several AACP features are gated on it"). So: unknown for a Linux host.
The first test should be done **without** it (ztos-podsd's rule is no vendor ID
spoofing). If the AirPods ignore Start, the fallback is
`DeviceID = bluetooth:004C:...` in `main.conf`, which changes the phone's identity
for every device, cubridge's Chameleon included, and needs the owner's decision.

## Other projects

| Project | Licence | Two-way audio? | Useful for |
|---|---|---|---|
| [LibrePods](https://github.com/librepods-org/librepods) (Kavish Devar) | GPL-3.0 | Linux: [PR #655](https://github.com/librepods-org/librepods/pull/655), open, not merged (2026-10). Android: a recorder in the `android/rewrite` branch | The 0x58 command sequence, framing, decoder config; AAP in general (already used by ztos-podsd as a source of facts). Docs: `docs/AAP Definitions.md`, `docs/opcodes.md` |
| [han-um/USBPods-Pico2W-2-way-audio-implement](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement) | GPL-3.0 (Apache-2.0 for upstream parts) | Yes: A2DP (Apple AAC-ELD vendor codec) + 0x58 mic at once, on a Pico 2 W; AirPods Pro 3 only; decodes with fdk-aac | Confirms the PR's facts independently on BTstack; the Apple DID question |
| [wasdwasd0105/USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W) | GPL-3.0 per GitHub (fork says Apache-2.0) | A2DP only, but implements **Apple's AAC-ELD A2DP vendor codec** | A future low-latency downlink (vendor codec 0x004C/0x8001, which the owner's AirPods advertise) |
| [pabloaul/apple-wireshark](https://github.com/pabloaul/apple-wireshark) | GPL-3.0 | Dissector only | AACP 0x58 = "Microphone Stream?"; capability names (`farFieldUplinkCapability`) |
| [SpriteOvO/AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop) | GPL-3.0 | No (battery, ear detection, "low audio quality" workaround) | Nothing for this |
| [adolfintel/OpenPods](https://github.com/adolfintel/OpenPods) | GPL-3.0 | No (BLE battery) | Nothing for this |
| [steam3d/MagicPods-Windows](https://github.com/steam3d/MagicPods-Windows) | no licence (all rights reserved) | No | Nothing; can't reuse anyway |

No other open implementation of an AAC-ELD **voice** path with AirPods was found:
nobody has a two-way AAC-ELD *call* codec, only the 0x58 mic stream plus A2DP.

**Reuse.** ztOS can't copy LibrePods' Rust into `ztmetro/pods` as is (different
language, and it's built on bluer/libpulse), but the licence is not the problem:
ztmetro/pods is GPL-3.0-or-later and already credits LibrePods for facts. The
plan below reimplements the four facts above in C, as ztos-podsd did for the
rest of AAP, and credits PR #655 in `ztmetro/pods/README.md`'s table.

## HFP: mSBC, the quick win

What has to be true, and where it stands:

1. **The AirPods offer mSBC.** Yes: SDP wideband-speech bit, HFP 1.8. PipeWire's
   hardware table already has AirPods entries (they block only the USB alt-1
   variant, which doesn't apply to a UART controller).
2. **The controller does transparent eSCO.** Yes, by its LMP features. PipeWire
   probes this with the standard Read Local Extended Features command
   (`spa/plugins/bluez5/hci.c`), allowed for an unprivileged raw HCI socket.
3. **WirePlumber allows it.** Yes: `52-ztos-bluetooth.conf` has
   `bluez5.enable-msbc = true`, `bluez5.hfphsp-backend = "native"`, and the
   `hfp_ag` role. (The `hfp_ag` role is the one used with headsets.)
4. **PipeWire actually offers it.** **No, today**: the card lists
   `headset-head-unit` with CVSD only. The reason isn't in the logs at the default
   level. In PipeWire 1.6 (`backend-native.c`), mSBC appears only after the
   headset's `AT+BRSF` (codec negotiation bit) and `AT+BAC=1,2` have been seen on
   the RFCOMM link, *and* `device_supports_codec()` passes. bluetoothd's journal
   shows several HFP links that ended early today ("Unable to get io data for
   Hands-Free Voice gateway: ... not connected") and "SDP record: Host is down" at
   boot, so the service-level connection may simply not have completed with
   these AirPods. To find out: debug logging for `spa.bluez5` and one HFP
   reconnect (approval needed: test **T1** below).
5. **SCO audio must travel over HCI**, because PipeWire's native backend reads
   and writes the SCO socket. On the stock phone it does not: stock
   `mixer_paths.xml` names **ABOX UAIF3 "Bluetooth"** (see `audio/MIC-REA.md`), i.e.
   the BCM4361's PCM/I2S pins go to ABOX, where Samsung's voice firmware (VSS,
   `audio/CALL-AUDIO.md`) mixes calls. Broadcom chips route SCO by a vendor
   command (`Write_SCO_PCM_Int_Param`, OCF 0x01C), not by any standard setting,
   and the Linux btbcm driver never sends it; the routing stays whatever the
   firmware patch's default is. If it's PCM, an SCO link comes up and the socket
   carries nothing (or zeros). Changing it is an **HCI vendor command**, which needs
   the owner's approval (test **T3**). `bluetooth/STOCK-AUDIO-PATH.md`
   (`research/bt-audio-stock`) hasn't landed yet; when it does, it should say what
   stock's libbt sends (routing, PCM format, `Enable_WBS` 0x07E for in-controller
   mSBC), and this section should be updated from it.
6. **Bandwidth.** At 115200 baud the UART carries 11.5 kB/s, 92 kbit/s, each way.
   mSBC over HCI is 60-byte frames every 7.5 ms, plus a 4-byte H4 SCO header:
   about 68 kbit/s each way, with no flow control for SCO. That's 74% of the line
   before any ACL traffic (AAP, AVRCP, the Chameleon). CVSD is the same 64 kbit/s.
   **Expect HFP over HCI to need the faster UART** (`fix/bt-uart-speed`); at
   3 Mbaud it's about 2%.

So mSBC is likely to work once the UART is fast and SCO goes over HCI. What the
owner should expect from it: 16 kHz voice both ways (telephone "HD voice"), and
music down to mono 16 kHz while the call profile is on.

## Calls on the modem

Cellular calls are mixed by VSS on ABOX, not by the host. On stock, Bluetooth
calls go VSS → UAIF3 → BCM PCM pins → eSCO. The two ways ztOS can get call audio to
the AirPods:

* **Stock's way (PCM):** the controller does the codec; mSBC needs Broadcom's
  `Enable_WBS` vendor command; ABOX UAIF3 needs to be brought up in
  `snd-zrn-abox`. Only HFP (CVSD/mSBC) is possible this way: the 0x58 stream is
  ACL data and never reaches the PCM pins.
* **Through the host:** VSS's voice to and from a host buffer (WDMA/RDMA, as the
  call path already does for the earpiece), PipeWire in the middle, and SCO over
  HCI or the 0x58 stream. More flexible, needs the voice firmware's buffers routed
  to the host.

Either way, a cellular call's own codec limits the quality: AMR-WB (VoLTE "HD
voice") is 16 kHz, so **mSBC already matches a VoLTE call**, and the 0x58 stream's
extra bandwidth pays off in VoIP (Signal, Matrix, Jitsi, Discord), recording and
voice notes. This is the honest expectation for the owner: the "iPhone-quality
microphone" is real, but for cellular calls it is capped by the network.

## Licensing (not legal advice)

* **AAC-ELD is patented** (Fraunhofer and others; licensed through the Via LA AAC
  pool, which covers the AAC family including ELD). The older AAC-LC patents have
  largely expired; ELD (standardised 2008) is younger. Using it on one's own
  devices for interoperability is what LibrePods' users do; distributing a ztOS
  image that decodes it is the same position ztOS already takes for A2DP AAC
  (PipeWire's AAC codec, which links fdk-aac, is in the image today).
* **FFmpeg's native `aac` decoder** (which decodes ELD) is **LGPL-2.1+**; linking it
  from GPL-3.0 ztos-podsd code is fine. This is what PR #655 uses. Its own TODO
  says "Check license compatibility (especially if we need a patent license)".
* **fdk-aac** ("Software License for The Fraunhofer FDK AAC Codec Library for
  Android") grants copyright permission but **expressly no patent licence**, and
  the FSF and Debian treat it as GPL-incompatible. Don't link it into GPL code
  here; FFmpeg's decoder is enough for the mic. (PipeWire's own AAC plugin is a
  separate shared object and stays as it is.)
* The 0x58 stream is **reverse-engineered interoperability** with the owner's own
  AirPods, on the owner's own phone. No Apple code, keys or firmware are involved.

## Design: the AACP microphone on ztOS

Keep ztos-podsd dependency-free and let it own the channel, as it does now:

```
AirPods ──L2CAP PSM 0x1001──▶ ztos-podsd ──unix socket (SOCK_SEQPACKET, SDUs)──▶ ztos-podsmic
                              (Start/Stop,                                     (libavcodec AAC,
                               conv. awareness)                                 pw_stream Audio/Source
                                                                                "AirPods microphone (HQ)")
```

1. **ztos-podsd** (C, no new libraries):
   * new pure header `pods-mic.h` next to `pods-proto.h`: the Start/Stop packets,
     `is_mic_audio(sdu)`, and the 0x58 demux (22-byte header, `[ts][len][au]`
     records). Tests in `tests/pods-proto.c` (`make check-pods`), from bytes.
   * the AAP socket's receive buffer to 4096, and the L2CAP receive MTU raised
     before `connect()` if real SDUs need it;
   * a `mic` request on its existing control socket: send Start, forward each 0x58
     SDU to the subscriber, send Stop when the subscriber goes away or the link
     drops; turn conversation awareness off for the duration and restore it.
   * the `ZTOS_PODSD_TRACE` recorder already exists: use it on the first real run to
     turn the owner's own 0x58 SDUs into test data.
2. **ztos-podsmic** (small C program, libpipewire + libavcodec), a user service
   started on demand: a `pw_stream` source that is always present but idle; when a
   client links to it, ask podsd for the mic; decode each AU (ASC `F8 E6 30 00`) to
   S16, packet-loss → one silent frame, write to the stream. Release on unlink.
   A real PipeWire node, not a FIFO: mute and permissions apply, and nothing else
   can read the raw stream (the FIFO leak PR #655 had to fix).
3. **WirePlumber:** a rule giving the node a sensible priority below the HFP source,
   so nothing picks it by default until it's proven; ztos-audiod untouched (it
   routes outputs; this is an input). `module-echo-cancel` in front of it for VoIP
   if the AirPods don't cancel echo on this stream (unknown).
4. **The A2DP glitch** (one bud after Start): first see whether it happens on a
   Pro 2 at all. If it does, the reset (card profile off → `a2dp-sink`) belongs in
   ztos-podsmic, run once after Start, and `ztos-bt-reconnect` must not mistake it
   for a drop (it acts on device disconnects and doesn't watch profiles; check).

A PipeWire SPA plugin inside the bluez5 monitor would be tidier one day, but it
would have to share the AAP channel with ztos-podsd; two programs opening PSM
0x1001 to the same AirPods is something to avoid. Kernel changes: none.

### Host prototype first

On the PC, with the AirPods paired to it (the owner's choice; it's their pair):

1. `pods-mic.h` and its byte tests, without any radio.
2. Run ztos-podsd (the Qt build in `ztmetro/pods/qt/README.md` already runs on a PC)
   with the `mic` request and `ZTOS_PODSD_TRACE`; capture a few seconds of 0x58
   SDUs; decode them offline with a 30-line libavcodec program; listen. This
   settles Pro 2 support, the sample rate question and the SDU sizes before
   anything runs on the phone.
3. ztos-podsmic on the PC with PipeWire; record with `pw-record`.

## Open questions

* **Sample rate.** The ASC says 48 kHz (index 3), 480-sample frames, mono, no SBR;
  PR #655 plays the output at 64 kHz ("480-sample frame, 7.5 ms"), and every
  report says it sounds right. Check against the per-AU timestamps in the 0x58
  records on a real capture.
* **Pro 2 (USB-C)** support of 0x58: not yet confirmed by anyone.
* **Apple DID** needed or not, on a Linux host.
* **Echo cancellation** on the 0x58 stream.
* **Which SCO route** the BCM4361 firmware patch defaults to (PCM or HCI).

## Next steps and the approvals they need

| # | Step | Touches | Needs |
|---|---|---|---|
| T1 | Find out why PipeWire offers only CVSD: `wpctl set-log-level 4` (WirePlumber's own level, at run time, no restart), then **disconnect and reconnect the AirPods' Hands-Free profile once**: `bluetoothctl disconnect-profile 40:DA:5C:62:CA:1B 0000111e-...` / `connect-profile` (or the whole device), and read `AT+BRSF`, `AT+BAC` and "has msbc/esco transport" in the journal; log level back to default afterwards | AirPods link (one HFP reconnect) | owner's approval (profile connect/disconnect) |
| T2 | Switch to `headset-head-unit` (mSBC if T1 shows it) and record 5 s with `pw-record` | AirPods profile switch | owner's approval; **after the UART fix** |
| T3 | If T2 records silence: route SCO over HCI, Broadcom `Write_SCO_PCM_Int_Param` (OGF 0x3F, OCF 0x01C), parameters `01 02 00 01 01` (routing = transport; the rest ignored for HCI; values as used on Raspberry Pi BCM43xx boards). Lost at controller reset; would go in `zrn-bluetooth`'s setup, owned by the UART agent | HCI vendor command | owner's approval; coordinate with `fix/bt-uart-speed` |
| T4 | 0x58 on the host first (prototype steps 1-3), then on the phone: ztos-podsd sends AACP Start `04 00 04 00 58 00 00 00 09 00 00 01 82 00 00 00 04 96 00` and, after a few seconds, Stop `04 00 04 00 58 00 00 00 02 00 03 01`; conversation awareness off (control 0x28 = 2) and back | AirPods (new AAP opcode, unverified on Pro 2) | owner's approval for the opcode on the owner's AirPods |
| T5 | If T4 shows one-bud playback: card profile `off` → `a2dp-sink` once | AirPods profile switch | owner's approval |
| T6 | Only if T4 is ignored without it: `DeviceID = bluetooth:004C:...` in `main.conf` | phone identity for every device, cubridge included | owner's decision |

What depends on the UART fix: T2 and T3 (SCO over HCI at 64 kbit/s each way is
74% of 115200 baud), and running the 0x58 mic **with** music: A2DP AAC at about
250 kbit/s doesn't fit 92 kbit/s on its own, and the mic stream (about 80 kbit/s
from the AirPods) would fill the receive direction. T1 and the host prototype
don't depend on it. T4 alone (mic, no music) might just fit at 115200, but isn't
worth trying before the fix.

## Sources

* LibrePods: README ("High quality two-way audio", "VendorID Spoofing"),
  `linux/README.md`, `docs/`; commit `89356c0` (2026-10-16).
  <https://github.com/librepods-org/librepods>
* LibrePods PR #655, "Added support for Apple's proprietary high-resolution
  microphone stream", head `99b0e580`, and its review thread (2026-07 to 2026-10).
  <https://github.com/librepods-org/librepods/pull/655>
* han-um, USBPods-Pico2W-2-way-audio-implement, README (2026-07-15).
  <https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement>
* wasdwasd0105, USBPods-Pico2W. <https://github.com/wasdwasd0105/USBPods-Pico2W>
* pabloaul, apple-wireshark, `plugins/aacp/aacp_enums.lua`.
  <https://github.com/pabloaul/apple-wireshark>
* SpriteOvO/AirPodsDesktop, adolfintel/OpenPods, steam3d/MagicPods-Windows (checked
  for any microphone or ELD code; none).
* Apple Newsroom, "AirPods now more versatile with studio-quality audio recording
  and camera remote", 9 June 2025.
  <https://www.apple.com/newsroom/2025/06/airpods-now-more-versatile-with-studio-quality-audio-recording-and-camera-remote/>
* On LE Audio: MacRumors, "iPhone 14 and new AirPods Pro support Bluetooth 5.3",
  7 Sept 2022 (LE Audio status unclear), and 9to5Mac on LC3 in beta firmware,
  2022. Nothing since shows LE Audio shipping on AirPods.
  <https://www.macrumors.com/2022/09/07/iphone-14-new-airpods-pro-bluetooth-5-3>
* PipeWire 1.6.11 source, `spa/plugins/bluez5/backend-native.c`
  (`device_supports_codec`, AT+BAC handling), `hci.c` (`spa_bt_adapter_has_msbc`),
  `bluez-hardware.conf`.
* Bluetooth SIG, Hands-Free Profile 1.8 and 1.9 (SDP SupportedFeatures bits:
  5 = wideband speech, 8 = super-wideband speech).
* ztOS: `audio/MIC-REA.md` (stock UAIF3 = Bluetooth), `audio/CALL-AUDIO.md` (VSS on
  ABOX), `ztmetro/pods/README.md`, `bluetooth/README.md`.
