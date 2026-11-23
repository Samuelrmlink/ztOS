# Bluetooth audio on stock Android (ROADMAP 8.59, research)

The question was whether stock Samsung Android 10 on the S9 (SM-G960F) uses
A2DP hardware offload. **It doesn't.** Stock encodes A2DP on the CPU in the
Bluetooth process and sends it over HCI on the UART, which is what ztOS does
with BlueZ and PipeWire. The chip's PCM/I2S link to ABOX carries only SCO/HFP
voice. Stock runs the UART at **3,000,000 baud**, and that speed is all ztOS's
A2DP needs.

## Sources

- **Stock firmware:** `SM-G960F_2_20220722002331_bk4xenjd19_fac.zip`, from
  `AP_G960FXXUHFVG4`, Android 10, build `QP1A.190711.020.G960FXXUHFVG4`
  (security patch 2022-03-01). This is the last firmware for the phone.
  `vendor.img` and `system.img` were unpacked in a scratch directory with
  liblz4, a sparse-to-raw converter and `debugfs`. Nothing from them is
  committed.
- **Stock device trees:** the 12 DTBs inside `stock/BOOT.img` (Star board revs 16, 17, 18, 20,
  23 and 26, each twice). Quotes below are from the rev16 tree; the UAIF3
  dai-link and the `/bluetooth` node are in all 12.
- **Online, read-only:**
  - LineageOS `android_kernel_samsung_universal9810`, the
    `sound/soc/samsung/abox/` file list;
  - LineageOS `android_device_samsung_universal9810-common`:
    `product_prop.mk` and `bluetooth/bdroid_buildcfg.h`.
- No EFS image was read, and the phone wasn't touched.

## 1. Does stock use A2DP hardware offload? No (high confidence)

| Evidence | What it shows |
|---|---|
| `vendor/build.prop`, `vendor/default.prop`, `system/build.prop`, `system/product/build.prop`, `system/etc/prop.default`, every `vendor/etc/init/*.rc` | None of them sets `ro.bluetooth.a2dp_offload.supported`, `persist.bluetooth.a2dp_offload.*` or `vendor.audio.feature.a2dp_offload.*`. The only BT audio props are `ro.config.bluetooth=external` and `ro.config.a2dp_by_primary=no` (`vendor/build.prop` lines 56 and 59), so A2DP is **not** routed through the primary (ABOX) HAL. |
| `vendor/etc/audio_policy_configuration.xml` line 117 and `audio_policy_configuration_sec.xml` line 158 | Both include `a2dp_audio_policy_configuration.xml`: the **legacy `a2dp` module, `halVersion="2.0"`**, with plain `AUDIO_FORMAT_PCM_16_BIT` 44.1 kHz ports. This is `system/lib/hw/audio.a2dp.default.so`, which passes PCM over a socket to the Bluetooth stack. No `bluetooth_audio_policy_configuration.xml` and no `bluetooth` module exist. |
| `vendor/manifest.xml` and `vendor/etc/vintf/manifest.xml` | Bluetooth HALs are `android.hardware.bluetooth@1.0::IBluetoothHci` and `vendor.samsung.hardware.bluetooth@2.0::ISehBluetooth`. There is **no `android.hardware.bluetooth.audio`** and **no `vendor.samsung.hardware.bluetooth.a2dp`**, and the vendor partition has no binary for either. |
| `system/lib64/libbluetooth.so` | It links `android.hardware.bluetooth.audio@2.0.so` and `vendor.samsung.hardware.bluetooth.a2dp@1.0.so` (`ISehBluetoothAudioOffload`), and has the offload code (`bta_av_vendor_offload_start`, `A2DP_CTRL_CMD_OFFLOAD_START`). This is Samsung's shared system image, which other devices use with offload. On the S9 neither HAL is in the vendor manifest, so the stack stays on the software path. |
| `vendor/etc/mixer_paths*.xml` | There is no A2DP path. The only Bluetooth routes are `*-bt-sco-*`, all going to **ABOX UAIF3**. The HAL string `set-bt-a2dp-playback` (in `audio.primary.exynos9810.so`) has no matching path in any mixer XML. |
| ABOX firmware `vendor/firmware/calliope_{sram,dram,iva}.bin` | No `a2dp`, `sbc`, `aptx`, `ldac` or `btsco` strings. |
| The stock DT (all 12 DTBs) | It has no `abox-bt` / `abox_bt` node and no A2DP node. |
| The Samsung 9810 kernel, LineageOS `sound/soc/samsung/abox/` | It has **no `abox_bt.c`**. The files are `abox.c`, `abox_if.c` (UAIFs), `abox_rdma.c`, `abox_wdma.c`, `abox_vss.c` and so on. Later Exynos SoCs added the ABOX-to-BT offload. |
| LineageOS `universal9810-common/product_prop.mk` | It sets `ro.bluetooth.a2dp_offload.supported=false`, `persist.bluetooth.a2dp_offload.disabled=true` and `vendor.audio.feature.a2dp_offload.enable=false`, which matches stock. |

The owner's guess was reasonable: later Samsung phones (S10 and newer) do
offload. On the S9, though, the vendor partition never provides a way to do it.

## 2. The BT PCM/I2S link: used only for SCO/HFP voice (high confidence)

- **The wiring (stock DT):**
  - `/sound/uaif@3`: `cpu` is `abox_uaif@0x17C50530` (UAIF3, id 3) and
    `codec` is `audio_codec_dummy`. No codec driver exists because the BT
    chip is the far end.
  - The UAIF3 pins are `aud-i2s3_bus`: `gpb2-0..3`, function 2 (I2S). The
    idle state is the same pins as function 0 with pull-down.
  - `/sound: samsung,routing` contains `"BLUETOOTH SPK", "ABOX UAIF3 Playback"`
    and `"ABOX UAIF3 Capture", "BLUETOOTH MIC"`.
  - `audio/MIC-REA.md` already gives UAIF3 as Bluetooth.
- **What uses it (`vendor/etc/mixer_paths.xml`):**
  - `set-bt-sco-{rx,tx}-rate-nb` sets `ABOX UAIF3 rate` = 8000 (line 447);
    `set-bt-sco-{rx,tx}-rate-wb` sets it to 16000 (line 455).
  - The playback paths `media-bt-sco-headphone` and `incall-bt-sco-headset`
    are `route-rdma6-to-sifs2`, `route-sifs2-to-uaif3`.
  - The capture path `media-bt-sco-mic` is `route-uaif3-to-nsrc0`,
    `route-nsrc0-to-wdma1`.
  - `incall_nb-bt-sco-headset` sets `ABOX Sound Type = BTVOICE`.
  - Every SCO use goes through these paths: calls, VoIP, VoLTE, Wi-Fi
    calling, recognition, recording, and ringtone or media during SCO.
    Nothing else uses UAIF3.
- **The chip side (`vendor/lib64/libbt-vendor.so`, Samsung's build of the
  Broadcom libbt):**
  - Strings: `hw_sco_config`, `SCO I2S interface change the sample rate to
    8K` / `16K`, `I2SPCM config {...}`, `SCO PCM configure {...}`, `SCO PCM
    data format {...}`, `hw_set_SCO_codec 0x%x`, `wrong codec is use in
    hw_sco_i2spcm_config, goes default NBS`.
  - Samsung also added `hw_pcm_pin_cfg` / `hw_read_pcm_pin_cfg`.
  - This means libbt was built with `SCO_CFG_INCLUDED` and
    `SCO_USE_I2S_INTERFACE`: SCO is routed to the chip's PCM/I2S pins, and
    the I2S rate switches between 8 and 16 kHz with the codec.
- **No `bt_vendor.conf`:** neither `/vendor/etc/bluetooth/` nor
  `/system/etc/bluetooth/` has one (`/system/etc/bluetooth/` holds only
  `bt_stack.conf`, `bt_did.conf`, `av_performance.conf` and `iop_bt.db`).
  So there's no `ScoUseBtPcm` or `PCM_CONFIG` override, and the compiled-in
  defaults apply.
  - The exact PCM parameter bytes (clock rate, master/slave, frame type)
    weren't decoded from the stripped binary.
  - **Assumption:** AOSP libbt's defaults are `SCO_PCM_ROUTING=0` (PCM),
    I2SPCM mode 1, and BT as I2S slave. If ztOS ever does HFP over PCM, read
    the actual values from a stock boot's `btsnoop` or logcat (`I2SPCM
    config {..}`, `SCO PCM configure {..}`), or capture the
    `0xFC1C` / `0xFC6D` / `0xFC1E` commands.

## 3. Codecs and where each is encoded (high confidence)

All A2DP encoding runs on the application CPU, inside the Bluetooth process
(`libbluetooth.so`). Then:

1. The encoded frames go out as L2CAP/ACL over HCI.
2. `android.hardware.bluetooth@1.0-impl` (H4) carries them on `/dev/ttySAC1`.

| Codec | Encoder |
|---|---|
| SBC | AOSP SBC encoder, linked into `libbluetooth.so` |
| AAC | Fraunhofer FDK AAC, linked statically (`FDKaacEnc_*`, `aacEncOpen`) |
| aptX / aptX HD | dlopen of `libaptX_encoder.so` / `libaptXHD_encoder.so`. Only `libaptX_encoder.so` is in `/system/lib64`, so aptX HD probably isn't available. |
| LDAC | `libldacBT_enc.so` + `libldacBT_abr.so` (`/system/lib64`) |
| Samsung Scalable (SSC) | `libssc_encoder.so` (`a2dp_vendor_ssc_encoder`, `AvoidAvScalable`) |

Neither ABOX nor the BT firmware encodes any A2DP codec. The only codec work
outside the CPU is **mSBC for HFP, which runs in the BCM4361 firmware**; see
the next section.

## 4. The HFP voice path: SCO over PCM/I2S, mSBC on the chip (high confidence)

- **`libbluetooth.so`:**
  - It supports wideband speech: `BTA_AG_WBS_EVT ... 1=CVSD 2=MSBC`,
    `Received AT+BAC, updating sco codec to MSBC` and `falling back to mSBC
    T1 settings`.
  - Its SCO setup log prints `path %d` and `data_path%d`.
- **`libbt-vendor`:** `hw_set_SCO_codec` and its I2S 8 kHz / 16 kHz
  switching show that, for mSBC, the host sets the chip's codec with a
  Broadcom vendor command and moves the I2S rate to 16 kHz. The chip
  encodes and decodes mSBC itself, and the voice samples cross as linear PCM
  on the I2S pins into ABOX UAIF3.
- **Nothing in stock** carries SCO audio over HCI. The vendor HCI HAL has an
  unused `scoDataReceived` hook, as AOSP does.
- **LineageOS `bdroid_buildcfg.h`** has `BTM_WBS_INCLUDED TRUE` and
  `BTIF_HF_WBS_PREFERRED FALSE`.

## 5. Stock's UART speed: 3,000,000 baud (high confidence)

- `/dev/ttySAC1` (`init.samsungexynos9810.rc` line 738 `chown bluetooth
  bluetooth /dev/ttySAC1`; the `/dev/ttySAC1` default is in libbt-vendor).
  No `bt_vendor.conf` exists, so the `UartPort` and `UartBaudRate` keys are
  never overridden.
- **The `.rodata` of `libbt-vendor.so` at file offset 0x4778** holds the
  ready-made HCI command `18 fc 06 00 00 c0 c6 2d 00`: opcode `0xFC18`
  (Broadcom Update UART Baud Rate), with a length of 6, two zero bytes and
  **0x002DC6C0 = 3,000,000** little-endian.
  - The code also builds 3,000,000 in four places (`mov`/`movk`).
  - The log strings are `bt vendor lib: set UART baud 115200`, then `baud_1`
    / `baud_2: set UART baud %i`.
  - **The sequence:** stock comes up at 115200, sends `0xFC18` with 3 M,
    then switches the host UART to 3 M. This happens twice (`baud_1` before
    and `baud_2` after the patch download).
- **The firmware patch** is `/vendor/firmware/bcm4361B2_semco.hcd`, picked by
  module info (strings such as `semco_sem_r02b_e32a_b2`); a
  `bcm4361B2_murata.hcd` is there for other modules. The `.hcd` file in
  `vendor/firmware` is the one ztOS ships.
- **The DT:** `/bluetooth` is `compatible = "samsung,bcm43xx"`, with BT_EN
  and host-wake GPIOs and pinctrl `bt-hostwake` (gpa2-3) / `bt-btwake`
  (gpg2-0). It has no baud property, because the speed is set from userspace.

This is for the `fix/bt-uart-speed` work; that branch owns the change. At 200
MHz source clock, 3,000,000 baud is 200e6 / (16 × 3e6) = 4.1667, which needs
the Exynos fractional divider (`UBRDIV` 3, `UFRACVAL` about 3). btbcm and
hci_bcm already do the `0xFC18` step when given `max-speed` / `oper-speed`
3000000.

## What this means for ztOS

- **Offload isn't worth pursuing.**
  - Stock doesn't do it, so there's no reference sequence to copy.
  - The S9's ABOX has no BT block and no `abox_bt` driver.
  - The BCM4361 firmware has never been seen taking A2DP from I2S on this
    board.
  - Building it would take reverse-engineering Broadcom's A2DP offload
    vendor commands, plus an ABOX route for the phone to generate. All of
    that would be to match a design stock itself doesn't use.
- **The faster UART is enough.**
  - At 3 Mbaud the link carries about 300 KB/s (8N1), about 2.4 Mbit/s of
    payload. SBC at its highest bitpool is about 330 kbit/s, AAC about 256
    kbit/s, and LDAC at most 990 kbit/s, so even LDAC fits with room for
    HCI overhead.
  - At 115200 the link carries about 92 kbit/s, less than any A2DP codec
    needs. That's why playback stops after the controller's buffers drain
    (about 0.5 s).
  - The fix: match stock's 3,000,000 baud and keep BlueZ and PipeWire
    encoding on the CPU. That is the same architecture as stock.
- **Later, for HFP calls:** stock carries voice over the PCM/I2S pins into
  ABOX UAIF3, at 8 kHz CVSD or 16 kHz mSBC, with the codec on the chip.
  - The ztOS equivalent would be an ABOX UAIF3 DAI plus BlueZ/oFono or
    PipeWire set to the PCM SCO route. It would need the `0xFC1C` /
    `0xFC6D` / `0xFC1E` setup from section 2.
  - The simpler first step is SCO over HCI (`0xFC1C` routing = HCI), and
    the 3 Mbaud UART is enough for that too.
