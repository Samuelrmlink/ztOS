# NFC on the Galaxy S9: what we can do, and how Flipper-like it can get

ROADMAP 8.28. This is a research and feasibility study only. **Nothing here
has touched the NFC hardware.** All reads on the phone were read-only: DT,
sysfs, debugfs `gpio`/`pinctrl`/`clk_summary`, and files on the read-only
VENDOR and SYSTEM mounts.

Each finding has a label:

- **observed**: seen on this phone or in its stock images;
- **from source**: read in vendor or mainline source;
- **inferred**: reasoned from the above, and not yet tested.

## Summary

- **Chip.** Samsung LSI **S3NRN82**, an NCI controller on I2C at address `0x27`
  (observed). It is the same family as the mainline `s3fwrn5` driver. It uses
  the same firmware file format, the same bootloader protocol and the same
  GPIO scheme (from source).
- **Reading.** NFC-A/B/F/V, Ultralight/NTAG, DESFire, Plus and EMV look fine.
  **MIFARE Classic is declared supported** by the stock image (`com.nxp.mifare`
  on VENDOR, observed). The controller does the Crypto1 itself, so reading
  works only with known keys. The raw frames and parity control needed for key
  recovery (nested, hardnested, darkside) are not available (inferred).
- **Emulation.** ISO-DEP (Type 4) HCE: yes. FeliCa HCE-F: declared (observed).
  A chosen UID / SAK / ATQA might be possible through standard NCI listen
  parameters, but that is untested. Ultralight/NTAG emulation is unlikely and
  untested. MIFARE Classic emulation: no.
- **Biggest gaps against a Flipper.** No 125 kHz, no sub-GHz, no IR, no
  transparent/raw RF mode, no sniffer.
- **Route.** Use a small ztOS char-device driver plus a userspace NCI stack. Do
  not use the kernel NFC subsystem: it cannot do HCE, the MIFARE Classic
  protocol or vendor commands. The first experiment needs **no BOOT flash** (it
  is a loadable module) and **no chip non-volatile writes**.

## 1. The controller

### Which chip

| Fact | Evidence | Label |
|---|---|---|
| Phone is **SM-G960F** (`starltexx`), last stock build `G960FXXUHFVG4` (Android 10) | `/mnt/system/system/build.prop`, `/mnt/vendor/build.prop` | observed |
| Controller **S3NRN82** | `/mnt/vendor/firmware/nfc/sec_s3nrn82_firmware.bin`, `/mnt/vendor/etc/nfc/sec_s3nrn82_rfreg.bin`, `FW_FILE_NAME="sec_s3nrn82_firmware.bin"` in `/mnt/vendor/etc/libnfc-sec-vendor.conf` | observed |
| A second copy of both files under `BRI/` (another board variant), same sizes | `ls` of VENDOR | observed |
| Stock DT node `sec-nfc@27`, `compatible = "sec-nfc"` (Samsung's own driver, not NXP `pn547`) | stock `exynos9810-starlte_eur_open_26.dts` (LineageOS universal9810, the copy in the vendor-kernel tree) | from source |
| G960N (Korea) | Not checked. The owner's phone is a G960F, and the DTs in the tree are `eur_open`. The Snapdragon S9s (G960U/W/0, SC-02K) use NXP and are a different board. | — |

### Wiring (stock DT, `hsi2c@108E0000` / stock "hsi2c20")

| Signal | Stock DT | Meaning | Label |
|---|---|---|---|
| Bus | `hsi2c@108E0000`, pins **gpp5-0 (SCL), gpp5-1 (SDA)**, function 2, no internal pull (`pin-pud 0`), 400 kHz | I2C, not UART | from source |
| Address | `0x27` | — | from source |
| Shared bus | `hrmsensor@57` (heart-rate sensor) on the same bus. Its I2C pull-up supply is `VDD_NFC_1P8`. | — | from source |
| IRQ | **gpa1-2**, EINT, rising edge. Stock name `nfc_int` (gpio-16). | chip → AP, "data ready" | from source, observed in `sensors/stock-state/gpio.txt` |
| VEN (enable) | **PCAL6524 expander line 11** (`<&pcal6524 11>`, port 1 bit 3). The vendor driver uses `SEC_NFC_PW_ON = 0`, so the line is **active low**: high = chip off. | power / reset | from source |
| FIRM / WAKE | **PCAL6524 line 9** (port 1 bit 1). One pin does two jobs: high at power-up selects the bootloader; high in NCI mode wakes the chip (`pdata->wake = pdata->firm`). | — | from source |
| CLK_REQ | **gpa2-6**, the chip asks the AP for a clock. The stock driver gates the clock from an IRQ on this pin. | — | from source |
| Clock | `oscclk_nfc` = clock id `0x398` = 920 = `CLK_CLKOUT_BASE`, i.e. the **PMU CLKOUT pad** (26 MHz OSC, inferred). `FW_CFG_CLK_SPEED=0x12` in the HAL config. | — | from source |
| Supply | `VDD_NFC_1P8` = **S2MPS18 LDO38**, 1.8 V, not always-on, `ldo_control` (the driver enables it at probe) | — | from source |
| Stock state (TWRP + stock kernel, NFC idle) | `VDD_NFC_1P8` on, 2 users (NFC + HRM). `nfc_ven` out **hi** (= off). `nfc_firm` out lo. | — | observed (`sensors/stock-state/`) |

On the current ztOS kernel (`7.1.0-zboot-wd-dp6`):

| Item | State | Label |
|---|---|---|
| `hsi2c@108E0000` | Not in the mainline DT. gpp5-0/1 are unclaimed in pinmux. | observed |
| PCAL6524 | Up at `96-0022` (bit-banged `i2c-gpio`, gpiochip50). Only lines 1, 4 and 7 are claimed (SBU mux, AUX_SW_OE, bt-en). **Lines 9 and 11 are unclaimed**, and their current levels are unknown (reading them is an I2C transfer). | observed |
| gpa1-2, gpa2-6 | Unclaimed (gpiochip22 / 23) | observed |
| LDO38 | S2MPS18 register `0x6F` = `0xAC` in **both** the stock and mainline dumps. Bits 7:6 = 2 means "on with PWREN", the same mode as stock. Code `0x2C` = 1.8 V, on the pattern decoded for the other LDOs. So the rail is probably already **on while awake**. The dump is not from today's boot. | observed (dump), inferred (decode) |
| CLKOUT | No clkout node in the DT. `CONFIG_EXYNOS_CLKOUT=y`, but no 9810 binding. Whether the bootloader leaves CLKOUT running is **unknown**. Finding out needs a PMU register read. | observed / unknown |
| Kernel | `# CONFIG_NFC is not set`. `CONFIG_I2C_GPIO=m`, `CONFIG_I2C_CHARDEV=y`, `CONFIG_GPIO_CDEV=y`, `CONFIG_MODULES=y`. | observed |

### Secure element and MST (described only, out of scope)

- **eSE.** A separate chip on SPI: stock `ese_spi@0`, `compatible = "ese_p3"`,
  on the SPI controller at `0x108A0000`. Its supply is `VDD_ESE_1P8` (S2MPS18
  LDO44), and its chip select is gpp4-3 (stock gpio-166 `ese_cs`) (from
  source). The NFC config routes card emulation to it: `ACTIVE_SE=0x02`,
  `DEFAULT_OFFHOST_ROUTE=0x02`, `NFA_HCI_ESE_CHIP_TYPE=0x01` (observed). So
  the eSE is an NFCEE behind the S3NRN82 (over SWP, inferred), and the AP talks
  to it directly over SPI for Samsung Pay and the TEE. `UICC_LISTEN_TECH_MASK`
  also implies a SIM (UICC) NFCEE. **Our stack must route everything to the
  host (DH) and never enable an NFCEE** (`NFCEE_MODE_SET`).
- **MST** (Samsung Pay's magnetic-stripe coil). A separate driver, `sec-mst`:
  `mst-pwr` gpg3-4, `mst-en` gph0-7, `mst-data` gph0-6 (from source). It is
  driven from the TrustZone payment TA. It has nothing to do with NFC
  (inferred).

### The firmware the chip runs

- The firmware lives in the chip's **flash**. Samsung's HAL
  (`/mnt/vendor/lib64/nfc_nci_sec.so`) reads the chip's version at each start.
  It updates the flash only when the image differs ("NFC need to update",
  "Force FW update", "sector update"). It also updates the "RF register" block
  separately when the RF version differs (`hal_vs_rfreg_update`) (observed,
  from strings).
- **Header of `sec_s3nrn82_firmware.bin`** (observed hexdump). It matches
  mainline's `s3fwrn5_fw_request_firmware()` layout exactly (from source):

  | Field | Value |
  |---|---|
  | date | `201810150609` |
  | version bytes at 0x10 | `45 00 4b 02` |
  | signature | at 0x2C, 128 bytes |
  | image | at 0x12C, 37 sectors (37 × 4096 + 0x12C = 151852, the file size) |
  | custom signature | at 0xAC, 128 bytes |

- The HAL config has `FW_BASE_ADDRESS=0x2000` (observed). Mainline's
  hardware table knows only base addresses 0x5000 and 0x3000 (from source), so
  **mainline would refuse to flash this chip** ("Unknown hardware version").
  That is good.
- The stock Android ran on this phone until ztOS, so the chip most likely
  already holds this 2018-10-15 image (inferred). **No download is needed at
  power-up:** the chip boots its own flash.

## 2. Linux support

### Does mainline `s3fwrn5` fit?

Mostly yes (from source, mainline `drivers/nfc/s3fwrn5/`):

- **Transport.** `s3fwrn5_i2c` speaks raw NCI with no framing over I2C. The
  `sec_nfc` driver does the same.
- **Power sequence.** Mainline drives `en`=1 / `wake`=0 for "cold", then sets
  `wake`=1 for bootloader mode. It waits 20 ms (`S3FWRN5_EN_WAIT_TIME`), drives
  `en`=0, and waits again. The vendor `sec_nfc_set_mode()` does the same thing,
  with `PW_OFF`=1 and `VEN_WAIT_TIME` 20 ms. So in DT both lines are
  `GPIO_ACTIVE_HIGH`, as on Exynos5433 TM2.
- **Firmware and bootloader protocol.** The headers, `GET_BOOTINFO` (cmd 0x01)
  and the proprietary NCI opcodes (`0x2F 0x22/0x26/0x27/0x28`) are the same.
  The HAL's strings mention the same commands.
- **Gaps:**
  - No regulator handling. LDO38 must already be on.
  - No CLK_REQ gating. Mainline enables an optional clock forever.
  - It requests `sec_s3fwrn5_firmware.bin` and `sec_s3fwrn5_rfreg.bin`.
  - **Its update path is unsafe for us:**
    - `s3fwrn5_fw_check_version()` compares fields independently, so it can
      "update" to an older image.
    - `s3fwrn5_nci_rf_configure()` sends `FW_CFG` with clk_type 0x01 (external
      crystal), which is wrong for this board's AP clock (`FW_CFG_CLK_SPEED=0x12`).
    - It then rewrites the RF registers.

    It runs only if the firmware file exists, so **never install a file named
    `sec_s3fwrn5_firmware.bin`**, or patch the path out.

The DT a mainline-style node would need (sketch, not applied):

```dts
/* gpp5-0/1 as i2c-gpio, like the expander bus; or hsi2c20 with its USI set up */
nfc_i2c: i2c-nfc {
	compatible = "i2c-gpio";
	scl-gpios = <&gpp5 0 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
	sda-gpios = <&gpp5 1 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
	i2c-gpio,delay-us = <2>;
	#address-cells = <1>; #size-cells = <0>;
	nfc@27 {
		compatible = "samsung,s3fwrn5-i2c";
		reg = <0x27>;
		interrupt-parent = <&gpa1>;
		interrupts = <2 IRQ_TYPE_EDGE_RISING>;
		en-gpios = <&expander 11 GPIO_ACTIVE_HIGH>;	/* VEN, high = off */
		wake-gpios = <&expander 9 GPIO_ACTIVE_HIGH>;	/* FIRM/WAKE */
		/* clocks = <&pmu_clkout ...>;  needs a 9810 CLKOUT provider */
	};
	/* hrmsensor@57 also lives here */
};
```

The `hsi2c` controller (stock "hsi2c20") would also need its USI_CON reset
released and a clock stub, as `make-dts.py` already does for hsi2c41. The
`i2c-gpio` route avoids all SoC register writes (inferred).

### Kernel NFC subsystem or userspace NCI?

**Recommended: userspace NCI over a small char device**, as Samsung's
`/dev/sec-nfc` and Android's libnfc-nci do. Reasons (from source / inferred):

- The Linux NCI core (`net/nfc/nci`, neard, `nfctool`) does poll-mode reading
  of standard tags well. It has **no host card emulation** (target mode is only
  NFC-DEP/LLCP). It has **no MIFARE Classic protocol** (`NFC_PROTO_MIFARE` in
  Linux means Type 2). Vendor commands go through netlink only for opcodes the
  driver lists.
- Everything Flipper-like needs:
  - listen-mode configuration (`LA_*`/`LF_*` parameters, the routing table);
  - this chip's proprietary MIFARE Classic protocol (`0x81` here, see §3);
  - proprietary commands;
  - RF field notifications.

  All of that is plain NCI that userspace can build when it owns the pipe.
- `CONFIG_NFC` is off today. Turning it on means a kernel rebuild and a BOOT
  flash. A loadable char-device module (`zrn-nfc`) works on the running
  kernel.
- A char device that copies `sec_nfc`'s ioctls (`SET_MODE`, `SLEEP`, `WAKEUP`)
  would even let Samsung's own HAL run in `zrn-android-root`, as a reference.
  But that HAL will **flash the chip** if it decides the version differs
  (`FW_UPDATE_MODE`, "Force update by config option"). Keep that option
  owner-gated.

### Firmware download at every power-up?

No. The chip boots from its flash (from source: the HAL and mainline only
download on a version mismatch). A download erases and writes the chip's
flash (bootloader commands `ENTER_UPDATE_MODE` 0x02, `UPDATE_SECTOR` 0x04 and
`COMPLETE_UPDATE_MODE` 0x05). Images are signed (SHA-1 hash and RSA signature
in the header), and a failed download can leave the chip in its bootloader.
**Any download needs explicit owner approval.** We should never need one.

## 3. Reading and identifying cards (Flipper "Read")

Evidence for what this controller exposes:

- `NFA_PROPRIETARY_CFG={00, 81, 82, 80, 8A, 81, 70, 74, F4}` in
  `libnfc-sec-vendor.conf` (observed). In AOSP libnfc-nci's
  `tNFA_PROPRIETARY_CFG` field order, that maps to:

  | Field | Value |
  |---|---|
  | 18092 active | 0x00 |
  | B′ | 0x81 |
  | dual | 0x82 |
  | **ISO 15693 = 0x80** | |
  | Kovio | 0x8A |
  | **MIFARE Classic = 0x81** | |
  | Kovio poll | 0x70 |
  | B′ poll | 0x74 |
  | B′ listen | 0xF4 |

  The field order is from source (AOSP, recalled, not re-checked against this
  exact Android 10 tree). These are NCI 1.x proprietary protocol numbers, so the
  chip is probably NCI 1.0/1.1 (inferred).
- `POLLING_TECH_MASK=0xEF` (all except bit 4) and `ISO_DEP_MAX_TRANSCEIVE=261`
  (observed).
- `/mnt/vendor/etc/permissions/com.nxp.mifare.xml`, which declares
  `com.nxp.mifare`, the feature Android uses for "this phone reads MIFARE
  Classic" (observed). `libnfc_nci_jni.so` carries the NXP `EXTNS_Mfc*` layer
  (`Mfc_Transceive`, auth with embedded keys) (observed, strings).

| Type | What NCI + S3NRN82 give | Raw frames / proprietary? | Verdict |
|---|---|---|---|
| **NFC-A** (ISO 14443-3A) | `RF_INTF_ACTIVATED_NTF` returns the UID (NFCID1, 4/7/10 bytes), **ATQA** (SENS_RES), **SAK** (SEL_RES). The Frame RF interface allows CRC'd byte frames. | No bit-oriented frames and no parity control (inferred). | yes (from source: NCI spec) |
| **NFC-B** (14443-3B) | PUPI, application data, protocol info (ATQB) | — | yes |
| **NFC-F** (FeliCa) | NFCID2, PAD, system code. T3T commands (Read/Write Without Encryption, Polling) through the Frame interface. | Encrypted FeliCa services need keys we don't have. | yes, public blocks |
| **NFC-V** (ISO 15693) | Proprietary protocol 0x80 (above). UID, DSFID, inventory, read/write single and multiple blocks. ICODE SLIX password commands are just frames. | Vendor protocol number | probably yes, needs a test |
| **Ultralight / NTAG** (Type 2) | READ/WRITE/GET_VERSION/READ_SIG/PWD_AUTH/FAST_READ are byte frames with CRC, all through the T2T Frame interface. Ultralight C 3DES runs on the host. | — | yes (inferred, standard on every NCI chip) |
| **MIFARE Classic** | Activated as protocol 0x81. Auth, read, write, increment and transfer go through the NXP-style MFC wrapper. **Crypto1 runs inside the controller, with a key the host supplies.** | **No raw access to encrypted frames or parity**, so no nested, hardnested, darkside or static-nonce attacks, and no reading of the nonce (inferred). | **partial**: dump with known or dictionary keys only, and needs a test |
| **MIFARE Plus** | SL1 behaves as Classic (above). SL3 is ISO-DEP with AES on the host. | — | partial (SL3 with keys) |
| **DESFire** (EV1/2/3) | ISO-DEP APDUs: GetVersion, app IDs, file list, free-read files. Crypto on the host. | — | yes |
| **EMV** contactless | ISO-DEP: PPSE, SELECT AID, GPO, READ RECORD. Public data only (PAN and expiry on many cards, no CVV). | — | yes |
| ST25TB / SRIX (14443-2B proprietary) | The chip has B′ settings, but ST25TB needs raw 14443-2B frames. | — | unknown, needs a test |
| **125 kHz LF** (EM4100, HID Prox, T5577) | **No LF front end or antenna.** The phone's NFC is 13.56 MHz only. | — | no |

Identification flow, like the Flipper's "Read": poll A/B/F/V → read the
activation parameters (tech, UID, ATQA/SAK, ATS, NFCID2, DSFID) → classify by
SAK/ATQA/GET_VERSION (NTAG21x, Ultralight EV1), DESFire GetVersion or the EMV
PPSE → then do the type-specific dump. All of this is NCI that userspace can
reach (inferred).

## 4. Emulation (Flipper "Emulate")

NCI listen mode is configured with `CORE_SET_CONFIG` parameters, which are
volatile in NCI (inferred), plus `RF_SET_LISTEN_MODE_ROUTING` and
`RF_DISCOVER` with listen techs. Android declares `android.hardware.nfc.hce`
and **`android.hardware.nfc.hcef`** on this phone (observed, VENDOR
permissions).

| Emulation | How | Flipper Zero | Verdict |
|---|---|---|---|
| **ISO-DEP / Type 4 (HCE)** | Listen NFC-A with the ISO-DEP RF interface routed to the host. APDUs arrive as NCI data, and we answer. NDEF Type 4 tags, custom applets and DESFire-like APDU apps are all possible. | ISO14443-4A emulation | **yes** (stock does this, observed via the hce feature) |
| **Chosen UID** | `LA_NFCID1` (0x33), with `LA_SEL_INFO` (0x32, SAK) and `LA_BIT_FRAME_SDD` / `LA_PLATFORM_CONFIG` (0x30/0x31, ATQA). Android never sets these, so phones present a random `08 xx xx xx` UID. Whether the S3NRN82 accepts a fixed 4/7-byte NFCID1 is **not known**. Many controllers do accept it. | Any UID | **unknown, needs a test**. If it works: UID-only readers would accept us. |
| **Ultralight / NTAG (Type 2)** | Would need the listen-mode Frame RF interface for NFC-A, with the host answering READ within the reader's timeout. The round trip over I2C at 100–400 kHz adds several ms. NCI 1.x doesn't define T2T listen, and most NCI controllers lack it (inferred). | Full | **unlikely, needs a test** |
| **MIFARE Classic** | Needs Crypto1 in the card side and µs-level timing. No NCI path, and no sign of a vendor command. | Full, with keys | **no** (inferred) |
| **NFC-F / FeliCa (HCE-F)** | Listen NFC-F with `LF_T3T_IDENTIFIERS` (NFCID2 + system code) routed to the host. Android limits NFCID2 to `02FE…`, but at the NCI level the host writes the identifier itself. | Limited | **partial**: NFC-F frames to the host. The real FeliCa crypto (Suica, etc.) is impossible. |
| ISO 15693 tag | No NCI listen for NFC-V | SLIX | no |

## 5. Other Flipper-like features, and the rest of the S9's hardware

| Feature | S9 | Label |
|---|---|---|
| Write tags (NTAG/Ultralight pages, NDEF, Type 4 NDEF, 15693 blocks, Classic blocks with keys) | yes, same paths as reading | inferred |
| "Magic" cards: Gen2/CUID Classic (block 0 writable after normal auth) | probably yes | inferred |
| "Magic" cards: Gen1a backdoor (7-bit `0x40` frame, no auth) | **no**: needs short and raw frames | inferred |
| "Magic" cards: Gen4/UMC | needs a test | inferred |
| Peer-to-peer (NFC-DEP / LLCP) | Supported by the chip (`P2P_LISTEN_TECH_MASK=0xC4`, NFC-DEP interface) | observed config / inferred |
| Reader detection (is a reader's field present?) | `RF_FIELD_INFO_NTF` in listen mode (config `NFCC_CONFIG_CONTROL` / field info) | inferred |
| Flipper's "Detect reader" / mfkey32 nonce capture | needs Classic emulation, so no | inferred |
| Sniffer (passive capture of reader↔card traffic) | **no**: NCI has no sniff mode, and the S3NRN82 exposes none | inferred |
| Sub-GHz radio (300–928 MHz) | **no** hardware. The modem's radio is not usable as an SDR. | inferred |
| 125 kHz RFID | **no** | from source (single 13.56 MHz controller) |
| Infrared TX/RX | **no**: the S9 has no IR blaster | inferred (no IR node in the stock DT) |
| iButton / 1-Wire, GPIO header | **no**: only USB-C | — |
| Bluetooth / BLE | yes: BCM4361 is up in ztOS. BLE advertising/scanning tools are possible, but **need the owner's approval** (paired AirPods). | observed |
| BadUSB / U2F | possible through the USB gadget (dwc3 is already in gadget mode for `172.16.42.1`). A configfs HID function. | inferred |
| MST (magstripe) | hardware exists (Samsung Pay coil). **Out of scope.** | from source |

## 6. Flipper Zero vs S9 (ztOS)

| Flipper feature | S9 verdict | Notes |
|---|---|---|
| Read NFC-A UID / ATQA / SAK / ATS | **yes** | NCI activation data |
| Read NFC-B | **yes** | |
| Read FeliCa (public) | **yes** | |
| Read ISO 15693 / SLIX | **unknown-needs-test** | proprietary protocol 0x80 |
| Read Ultralight / NTAG (incl. password, signature) | **yes** | T2T frames |
| Read Ultralight C | **yes** | 3DES on the host |
| Read MIFARE Classic with dictionary keys | **partial / unknown-needs-test** | controller Crypto1, feature declared |
| MIFARE Classic key recovery (nested / hardnested / darkside / mfkey32) | **no** | no raw parity frames |
| Read DESFire / MIFARE Plus SL3 | **yes** | ISO-DEP |
| Read EMV (public) | **yes** | ISO-DEP |
| Read ST25TB / SRIX | **unknown-needs-test** | |
| Emulate ISO14443-4A (HCE) | **yes** | |
| Emulate a chosen UID | **unknown-needs-test** | `LA_NFCID1` |
| Emulate Ultralight / NTAG | **unknown-needs-test**, likely no | |
| Emulate MIFARE Classic | **no** | |
| Emulate FeliCa | **partial** | HCE-F, no FeliCa crypto |
| Emulate ISO 15693 | **no** | |
| Write tags / NDEF | **yes** | |
| Write magic cards | **partial** | Gen2 yes, Gen1a no |
| NFC sniffer | **no** | |
| Reader field detection | **unknown-needs-test** | likely yes |
| 125 kHz RFID | **no** | |
| Sub-GHz | **no** | |
| Infrared | **no** | |
| iButton | **no** | |
| Bluetooth / BLE | **yes** (owner approval for any radio activity) | |
| BadUSB / U2F | **partial** | USB gadget HID |

## 7. Plan

### Stage 0: read-only pre-checks (needs owner approval: these are transfers)

1. **PMIC read.** One ACPM PMIC read of S2MPS18 register `0x6F` (LDO38),
   with the existing `s9p-acpm read` path. Expect `0xAC`. If bits 7:6 are 0
   (off), **stop**: enabling it is a PMIC write that needs approval.
2. **Expander read.** I2C **reads** on `i2c-96`, address `0x22`, from
   PCAL6524 registers:
   - `0x00`–`0x02` (input);
   - `0x04`–`0x06` (output);
   - `0x0C`–`0x0E` (config, 1 = input).

   Use `i2cget -f` (forced, because `pca953x` owns the address), or better,
   add a debugfs line in a module. This tells us whether VEN (P1.3) and FIRM
   (P1.1) are inputs and what level they sit at, so whether the chip is on or
   off right now. Note that this bus also carries bt-en and the SBU mux, so the
   reads must not overlap a DP hot-plug.

### Stage 1, the first experiment: read the chip's NCI version (needs owner approval)

The vehicle is a new out-of-tree module, `zrn-nfc.ko` (loads on the running
kernel, **no BOOT flash**), plus a userspace tool `ztnfc version`. Run inside
`ztlock`. Each item below is an access:

**Module load**
- `insmod i2c-gpio` (already loaded) and `insmod zrn-nfc.ko`.

**GPIO / pinctrl writes (SoC)**
- **gpp5-0 → GPIO (SCL), gpp5-1 → GPIO (SDA)**, open-drain emulation by
  `i2c-gpio` (line output-low or input; pull-ups are external on
  `VDD_NFC_1P8`). `i2c-gpio` creates a new bus (`i2c-1xx`, named
  `zrn-nfc-i2c`). This writes the gpp5 CON/DAT registers through pinctrl.
- **gpa1-2 → input / EINT, rising edge**, as the NFC IRQ (a gpa1 CON/EINT
  config write through pinctrl).
- **gpa2-6 → input (read-only sample)**, to log CLK_REQ. It is not used as an
  IRQ and no clock is gated from it.

**GPIO writes on the PCAL6524** (I2C writes to `i2c-96`/`0x22` by the
`pca953x` driver, read-modify-write, other bits unchanged)
- Line 11 (VEN):
  - output reg `0x05` bit 3 = **1** (chip held off);
  - config reg `0x0D` bit 3 = **0** (output).
- Line 9 (FIRM/WAKE):
  - output reg `0x05` bit 1 = **0**;
  - config reg `0x0D` bit 1 = **0** (output).

**Power-up sequence** (same as stock `sec_nfc_set_mode(FIRMWARE)` and mainline
`set_mode(NCI)`)
1. VEN = 1 (off), FIRM = 0. Wait 20 ms.
2. **VEN = 0 (chip on, normal firmware, not bootloader)**. Wait 20 ms.
3. FIRM/WAKE = 1 (wake; in NCI mode this is not bootloader entry, because VEN
   is already low). Wait 20 ms.

**Regulators:** none touched (LDO38 must already read on in stage 0).

**Clock:** none touched. No CLKOUT/PMU write. Inferred, and unknown: the host
interface may answer without the XI clock. If it does not answer, we stop and
come back with a CLKOUT proposal.

**I2C transfers** (new bus, **address 0x27 only**; never address `0x57`, the
HRM sensor)
1. Write `20 00 01 00`: **CORE_RESET_CMD**, reset type 0x00 = *keep
   configuration*. Wait for the IRQ (timeout 500 ms).
2. Read 3 bytes (header), then `plen` bytes. Expected:
   - NCI 1.x: `40 00 03 00 <nci-ver> <cfg-status>`;
   - NCI 2.0: `40 00 01 00`, then a `60 00 …` CORE_RESET_NTF carrying the
     manufacturer and firmware info.
3. Write `20 01 00`: **CORE_INIT_CMD** (NCI 1.x form; send `20 01 02 00 00`
   only if step 2 said NCI 2.0).
4. Read `40 01 …` **CORE_INIT_RSP**. Log:
   - status;
   - NFCC features;
   - supported RF interfaces;
   - max logical connections;
   - routing table size;
   - max control payload;
   - manufacturer ID;
   - **4 bytes of manufacturer info (firmware version)**.
5. Any notification that arrives (`6x xx`) is read and logged, never answered.

**NCI commands deliberately not sent**
- `RF_DISCOVER_CMD`: **no field on**.
- `CORE_SET_CONFIG`.
- `NFCEE_DISCOVER` / `NFCEE_MODE_SET`: **no eSE/UICC**.
- Any `0x2F` proprietary command.

**Power-down**
1. FIRM/WAKE = 0.
2. VEN = 1 (off).
3. Release the GPIOs. VEN stays output-high (off). gpp5 goes back to input.
4. `rmmod zrn-nfc`.

**Bootloader alternative (not proposed).** Sampling the version in bootloader
mode (FIRM = 1 at power-up, then `GET_BOOTINFO` `00 01 00 00`) is read-only in
itself, but it puts the chip in the mode where flash writes happen. The NCI
path above is safer.

### Steps that write the chip's non-volatile memory (each needs explicit owner approval; none is planned)

- **Firmware download**: bootloader `ENTER_UPDATE_MODE` (0x02),
  `UPDATE_SECTOR` (0x04), `COMPLETE_UPDATE_MODE` (0x05). Also mainline
  `s3fwrn5`'s automatic update, and Samsung's HAL's automatic update.
- **RF register update**: NCI proprietary `0x2F 0x26` START_RFREG, `0x2F 0x22`
  SET_RFREG, `0x2F 0x27` STOP_RFREG.
- **`0x2F 0x28` FW_CFG** (clock type/speed/request). Treat it as persistent.
  Mainline would send the *wrong* value (external crystal).
- Samsung HAL extras: "comboreset / bootloader disable", "T4T emulation data
  backup". These are unknown commands; treat them as NVM writes.
- `CORE_RESET` with reset type 0x01 (reset configuration): probably volatile
  only, but not needed, so avoided.

### Stage 2: `ztnfc` reader (after stage 1 works)

- **Kernel.** `zrn-nfc` grows into a `/dev/zt-nfc` char device:
  - read/write raw NCI;
  - poll on the IRQ;
  - ioctls `SET_MODE` (off/NCI; *no bootloader mode exposed at all*) and
    `WAKE`.
  - CLK_REQ handling once a CLKOUT clock provider exists (a 9810 entry for
    `exynos-clkout`, or a ztOS stub). That needs the PMU CLKOUT register
    identified and an owner-approved register write.
- **Userspace.** A small NCI 1.x stack in `ztnfc`: init, discover (poll A/B/F/V
  only), activation parsing, and the Frame/ISO-DEP/T3T/MFC transceive.
  Commands:
  - `ztnfc scan`;
  - `ztnfc dump`;
  - `ztnfc ndef`;
  - `ztnfc emv`;
  - `ztnfc mfc --keys dict`.
- **Metro app** "NFC" in `ztmetro/apps/`. It shows the card type, UID,
  ATQA/SAK/ATS, a memory view, and saves the dump in the Flipper `.nfc` text
  format, so dumps move between the two.
- **Power.** The chip is off unless the app is open. No background polling at
  first (battery, and the alarms rule).

### Stage 3: emulation

1. ISO-DEP HCE: NDEF Type 4 tag, and replaying a captured APDU script.
2. Test `LA_NFCID1`, `LA_SEL_INFO` and `LA_SENS_RES` for a fixed UID / SAK /
   ATQA.
3. Test whether NFC-A listen with the Frame interface reaches the host
   (Ultralight emulation).
4. HCE-F (NFC-F listen, host route).

Routing must always be `DH` (0x00), and NFCEEs stay disabled, so the eSE never
answers a reader on our behalf.

## Biggest unknowns

1. **Clock.** Is CLKOUT (`oscclk_nfc`) running under ztOS, and does the chip
   need it for NCI or only for RF? This blocks RF (stage 2), not stage 1.
2. **Current VEN/FIRM level.** The PCAL6524 lines are unclaimed. If VEN floats
   low, the chip may be powered (and drawing current) right now.
3. Whether MIFARE Classic (protocol 0x81) really works through plain NCI
   outside Samsung's HAL, and its exact command wrapper.
4. Whether the S3NRN82 honours `LA_NFCID1` (chosen UID) and allows NFC-A Frame
   listen (Type 2 emulation).
5. The NCI version: 1.0 or 2.0 (the proprietary protocol numbers suggest 1.x).
6. The HRM sensor at `0x57` shares the bus and the 1.8 V pull-up rail. A
   future HRM driver (owned by the sensors work) and `zrn-nfc` must share one
   bus instance.

## Sources

- Phone, read-only:
  - `/mnt/vendor/etc/libnfc-sec-vendor.conf`;
  - `/mnt/system/system/etc/libnfc-nci.conf`;
  - `/mnt/vendor/firmware/nfc/`, `/mnt/vendor/etc/nfc/`;
  - `/mnt/vendor/etc/permissions/*nfc*`, `com.nxp.mifare.xml`;
  - strings of `nfc_nci_sec.so` and `libnfc_nci_jni.so`;
  - `/proc/device-tree`, `/sys/bus/i2c/devices`, `/proc/config.gz`;
  - debugfs `gpio`, `pinctrl`, `clk/clk_summary`.
- Repo: `sensors/stock-state/{gpio,regulator_summary,s2mps18-regs*,pinctrl}.txt`
  (stock TWRP-era dumps), `power/suspend/README.md` (S2MPS18 LDO decode),
  `kernel/make-dts.py` (expander and hsi2c patterns).
- Stock DT: LineageOS `android_kernel_samsung_universal9810`,
  `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts` (node
  `sec-nfc@27`).
- Vendor kernel: LineageOS `universal9810` (lineage-17.1) `drivers/nfc/sec_nfc.{c,h}`,
  `include/dt-bindings/clock/exynos9810.h`.
- Mainline: `drivers/nfc/s3fwrn5/{core,firmware,i2c,nci,phy_common}.c`
  (torvalds/master, fetched 2026-11-12).
