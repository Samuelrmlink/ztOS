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

## Run log: stage 0 on 2026-11-12 (stopped at LDO38)

The owner approved stages 0 and 1 on 2026-11-12 ("Approve version read"). The
tools are in [`driver/`](driver/):

- `zrn-nfc.c`: the stage-1 module, built against `linux-7.1-dp/out-target-bt`,
  vermagic `7.1.0-zboot-wd-dp6`;
- `stage1.sh`: pre-checks, stage 0, insmod, readback, rmmod;
- `build.sh`.

Run at 07:02 CST inside `ztlock run nfc`. Kernel #62, battery 31%, no
`zrn_hrm` loaded, gpp5-0/1 unclaimed, no `i2c-103`. Raw output:

```
== stage 0: LDO38 (S2MPS18 0x6f) via ACPM
read type=1 reg=0x6f: ret=0 val=0x2c words=['0x2b0000', '0x2c00', '0x0', '0x0']
== LDO38 = 0x2c (mode 0)
== STOP: LDO38 off
```

**Result: LDO38 (`VDD_NFC_1P8`) is off** (observed): bits 7:6 = 0, voltage
code 0x2C = 1.8 V. The older mainline dump in `sensors/stock-state/` showed
`0xAC`, so whatever booted then had it on. Today's kernel #62 does not. The
script stopped there, as the approval required.

Not done: the expander reads, the module load, every GPIO or I2C access, and
every NCI command. So there are **no NCI responses, no NCI version, no firmware
version, and no VEN/FIRM reading**. With its 1.8 V rail off, the chip is
unpowered whatever VEN says, and so are the pull-ups on gpp5-0/1 (inferred).

### Proposal: stage 1b (LDO38 on), for owner approval

Everything in stage 1 stays the same, plus one PMIC write and one restore:

- **ACPM update** of S2MPS18 `0x6F`, mask `0xC0`, value `0x80`. That gives
  `0xAC`: mode 2, "on while awake", the same value stock and the earlier
  mainline both had. The command is `s9p-acpm update 1 0x6f 0x80 0xc0`. Read it
  back once. Then wait 5 ms for the ramp (`regulator-ramp-delay` 12000 µV/µs).
- At the end: **restore** `0x6F` to the value read before, `0x2C` (off), with
  `s9p-acpm write 1 0x6f 0x2c`, and read it back. Do this unless the owner
  prefers to leave the rail on.
- Side effect: the same rail feeds the HRM sensor's I2C pull-ups. While it is
  on, the `feature/hrm` agent must not run, which the gpp5 check already
  enforces.

### Proposal: how the NFC and HRM drivers share gpp5-0/1

Both chips sit on the stock "hsi2c20" bus: NFC at `0x27`, HRM at `0x57`. Both
depend on `VDD_NFC_1P8` for power or pull-ups. For the long term:

1. **One bus-owner module, `zrn-i2c20`.** It owns the bus, which should become
   a fixed number, for example `i2c-103`. Start with `i2c-gpio` on gpp5-0/1;
   later, the real hsi2c20 controller once its USI setup is known. It also owns
   **LDO38**, reference-counted: `zrn_i2c20_get()` / `zrn_i2c20_put()`, which
   enable or disable the rail through ACPM.
2. `zrn-nfc` and `zrn-hrm` become ordinary I2C client drivers on that bus
   (`i2c_new_client_device` for 0x27 and 0x57). Neither registers its own bus
   or touches the pins. The I2C core then serialises their transfers. This is
   the same pattern as zrn-bt's expander bus.
3. Neither client may switch LDO38 off while the other holds it.
4. Later, the same goes into the device tree: one `i2c` node with both children
   and a regulator, once S2MPS18 regulators exist in mainline.

Until then, the rule stands: only one of `zrn_nfc` / `zrn_hrm*` is loaded at a
time, and each checks for the other before loading.

### Draft: stage 2, reading a card (for owner approval; not to be run before stage 1 succeeds)

This assumes stage 1/1b worked and the chip answers NCI. New accesses in
**bold**.

- **Power and bus:** as in stage 1/1b (LDO38 on, i2c-gpio, gpa1-2 IRQ, VEN/FIRM
  on the expander).
- **Clock (CLKOUT for `oscclk_nfc`):**
  - Stage 2a, read-only: **one 32-bit read of the PMU CLKOUT control register**
    (address still to be found from the 9810 PMU map, probably near PMU base
    `0x14060000` + `0x0A00`; we will propose the exact address before any read).
  - Stage 2b, only if CLKOUT is off and RF discovery fails without it: **one
    write** selecting OSC (26 MHz) on CLKOUT and enabling it. Exact register
    and value to be proposed separately. The audio work also found `xclkout`
    unused on mainline (`audio/MIC-REA.md`).
- **NCI commands** (the CORE_RESET/CORE_INIT of stage 1, then):
  - **`CORE_SET_CONFIG` (20 02)**, volatile RF parameters only:
    - `TOTAL_DURATION` (0x00) = 500 ms;
    - `PA_BAIL_OUT` (0x08) = 1;
    - no `LA_*`/`LF_*` listen parameters;
    - nothing persistent.
  - **`RF_SET_LISTEN_MODE_ROUTING`: not sent.** No NFCEE is enabled and no
    listen mode is configured.
  - **`RF_DISCOVER_CMD` (21 03), poll only:** NFC-A, NFC-B, NFC-F and
    ISO 15693 (`00 01 / 01 01 / 02 01 / 06 01`, or the proprietary 0x80 form if
    NCI 1.0). **This turns the RF field on.** Each run lasts at most 30 s, then
    `RF_DEACTIVATE_CMD` (21 06 00) for idle.
  - `RF_DISCOVER_SELECT` (21 04), only if several cards are found:
    - protocols T2T (0x02), ISO-DEP (0x04), T3T (0x03), 15693;
    - **MIFARE Classic (0x81)** only in a separate, later sub-step.
  - **Data exchange on the static RF connection (conn id 0)**:
    - T2T `READ` (0x30) and `GET_VERSION` (0x60);
    - ISO-DEP `SELECT` of the NDEF app, the EMV `2PAY.SYS.DDF01` and DESFire
      `GetVersion` (0x60);
    - T3T `Polling` / Read Without Encryption.
    - **No write commands to cards.**
  - `RF_DEACTIVATE` (21 06 00), then power down as in stage 1.
- **Still excluded:** NFCEE commands, `0x2F` proprietary commands, firmware or
  flash, and listen/emulation mode. MIFARE Classic gets its own approval later
  (it needs the proprietary MFC wrapper, protocol 0x81).
- **Cards:** only the owner's own test tags/cards, placed on the back of the
  phone by the owner.

## Run log: stage 1b on 2026-11-12 (chip did not ACK)

Owner-approved (LDO38 on/restore + stage 1). Run at 07:28 CST inside `ztlock
run nfc`, kernel #62, battery 70%, no `zrn_hrm`, gpp5 free. Observed:

```
LDO38 read 0x2c (off) -> update 0x80/0xc0 -> reads 0xac
PCAL6524 before: in 00..02 = 92 00 20   out 04..06 = ff ff ff   cfg 0c..0e = 6d ff ff
zrn-nfc: [start]        VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: [off]          VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: [ven-on]       VEN=0 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: [wake]         VEN=0 FIRM=1 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: TX 20 00 01 00 -> -6
zrn-nfc: [powered-down] VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
PCAL6524 after:  in 00..02 = 92 08 20   out 04..06 = ff fd ff   cfg 0c..0e = 6d f5 ff
rmmod ok, gpp5 unclaimed again, i2c-103 gone
LDO38 restored: write 0x2c -> reads 0x2c
```

What this shows:

- **The chip did not acknowledge address 0x27.** `-6` is `-ENXIO`, which
  i2c-algo-bit returns on an address NACK. So no NCI response, no NCI version
  and no firmware version. CORE_INIT was not sent, because the module stops
  after a failed write.
- No interrupt from the chip, and CLK_REQ stayed low throughout. `irqs=1` was
  already 1 at the start: one spurious edge when the EINT was set up.
- **VEN/FIRM before the run:**
  - Port 1 of the expander was all inputs (`0x0D = 0xff`), with input `0x01 =
    0x00`. So VEN and FIRM were not driven: they floated low, with the 1.8 V
    rail off until just before.
  - After the run, VEN is an output driven high (chip off), and FIRM is an
    output driven low (`0x05 = 0xfd`, `0x0D = 0xf5`, input `0x01 = 0x08`). They
    were left that way, as designed.
- The other expander lines were unchanged: 0x0C stays 0x6d, 0x04/0x06 stay
  0xff, and port 0/2 inputs stay the same.
- Nothing hung, and the phone did not reset. Everything was restored.

Likely causes, most likely first (all inferred):

1. **The chip was still starting up and NACKed the first write.** Mainline
   `s3fwrn5_i2c_write()` retries once after 110 ms for exactly this case:
   "chip was in standby". The vendor driver also has a 150 ms
   `CONFIG_SEC_NFC_MARGINTIME` variant of the 20 ms VEN wait.
2. **No reference clock.** The chip asserts CLK_REQ when it wants the AP's
   CLKOUT, and it never did. If it cannot boot without XI, it would neither
   ask nor answer. CLKOUT is not touched by anything on mainline.
3. **The bus itself.** The pull-ups may not be on LDO38, or the bus may be
   wired differently. The HRM agent's ID read at 0x57 on the same pins will
   show whether this bus works at all.

### Proposal: stage 1c (for owner approval)

The same accesses as stage 1b, with only timing and retry changes. There are
no new registers and no new commands.

- VEN on, then wait **150 ms** (vendor margin time) before WAKE.
- On a NACK, retry the same CORE_RESET write **once after 110 ms**, as
  mainline does. Retry at most once.
- Additionally, before powering the chip, sample SDA and SCL levels as GPIO
  inputs. This is a read, and shows whether the pull-ups are powered.

If that also NACKs, the next proposal is the CLKOUT read (stage 2a in the
stage 2 draft), moved ahead of stage 2.

### zrn-i2c20 (code, host-tested only)

[`driver/zrn-i2c20.c`](driver/zrn-i2c20.c) and `zrn-i2c20.h` implement the
shared bus owner described above:

- **Bus:** `i2c-103` on gpp5-0/1.
- **`zrn_i2c20_get()`:**
  - the first caller reads 0x6F;
  - if bits 7:6 are 0, it updates to mode 2, reads back, and waits 5 ms;
  - it returns the adapter.
- **`zrn_i2c20_put()`:** the last caller restores the exact value read before.
  `leave_on=1` skips the restore.
- **Unload:** also restores.
- **Load:** fails with `-EBUSY` if gpp5 is already taken.

It is built against `linux-7.1-dp/out-target-bt` with no warnings. It has not
been loaded on the phone. `zrn-nfc` (stage 1) is still standalone; stage 1c
can move it onto `zrn_i2c20_get()`.

## Stage 1c (approved 2026-11-12)

The HRM agent then showed that the bus itself works. With LDO38 on, SCL and
SDA idled high, and the MAX86915 at 0x57 answered PART_ID 0x2B (08:07 CST).
So the NACK at 0x27 is on the NFC chip's side.

Changes for 1c, all in `driver/`:

- **`zrn-nfc.c` timing:**
  - waits 150 ms after VEN on;
  - on a NACK, retries CORE_RESET once after 110 ms.
- **`zrn-nfc.c` bus:**
  - It now builds its own `i2c-algo-bit` adapter with **class 0** (no
    `.detect` probing; i2c-gpio would set `I2C_CLASS_HWMON`) and no address
    retries.
  - It samples SCL and SDA with the lines released, before power-up.
- **`zrn-i2c20.c`:** uses the same class-0 bit-banged adapter, and fails with
  `-EBUSY` if gpp5 is taken.
- **`stage1.sh`:** checks that `i2c_algo_bit` is loaded, and captures the
  kernel log from a `/dev/kmsg` marker. The line-count method printed nothing
  in 1b, because the ring buffer is full.

**Not yet run.** At 08:39 and again until 09:10 CST, `ztlock run nfc` timed out
after 30 min. `/run/ztbench/lock` is held by **`ztnotifyd` (pid 17519, started
08:24:59)**, which inherited the locked fd 9 from a `ztlock run`. gnss and
thermal are queued behind it too. This lock leak is not ours to fix.

### Stage 1c result, 09:19 CST: the chip answers

The lock was freed (ztnotifyd restarted without fd 9; ztlock now closes fd 9
for its commands). Run inside `ztlock run nfc`, kernel #62, battery 33%, no
`zrn_hrm`. Raw output (observed):

```
LDO38 0x2c -> 0xac (restored to 0x2c at the end, reads 0x2c)
PCAL6524 before: in 90 08 20  out fd fd ff  cfg 6d f5 ff   (VEN output high = off, left from 1b)
zrn-nfc: bus idle SCL=1 SDA=1
zrn-nfc: [start]  VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: [off]    VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: [ven-on] VEN=0 FIRM=0 IRQ=0 CLK_REQ=0 irqs=1      (150 ms)
zrn-nfc: [wake]   VEN=0 FIRM=1 IRQ=0 CLK_REQ=0 irqs=1
zrn-nfc: TX 20 00 01 00 -> 0
zrn-nfc: RX 40 00 03 00 10 01
zrn-nfc: TX 20 01 00 -> 0
zrn-nfc: RX 40 01 14 00 02 0e 02 00 03 01 02 03 02 2e 04 ff 10 00 0e 45 00 4b 02
zrn-nfc: RX timeout after 50 ms (irq line 0, irqs 3)        (no notifications)
zrn-nfc: [powered-down] VEN=1 FIRM=0 IRQ=0 CLK_REQ=0 irqs=3
PCAL6524 after: unchanged; rmmod ok; gpp5/gpa1-2/gpa2-6 unclaimed again
```

The first CORE_RESET was ACKed, so the retry was not needed. **The 1b NACK
was the 20 ms start-up wait being too short. 150 ms works** (inferred from
the difference between the two runs).

Decoded (from source: NCI 1.0 spec layout, the same as `nci_core_init_rsp`):

| Field | Value | Meaning |
|---|---|---|
| CORE_RESET_RSP | status 0x00, **NCI version 0x10 = NCI 1.0**, config status 0x01 | The chip reports its configuration as reset, although we asked to keep it: a cold start, so nothing was stored (inferred). |
| NFCC features | `02 0e 02 00` | Octet 1 `0x0e`: technology-, protocol- and **AID-based routing** supported, which HCE needs. Octet 0 bit 1: configuration by DH and NFCEEs. Octet 2: a power-state option. |
| RF interfaces | `01 02 03` | **Frame**, **ISO-DEP**, **NFC-DEP**. No proprietary interface (NXP lists 0x80 for MIFARE Classic). |
| Max logical connections | 2 | — |
| Max routing table | 0x042E = 1070 bytes | — |
| Max control payload | 255 | — |
| Max large parameters | 16 | — |
| Manufacturer ID | **0x0E = Samsung** | ISO/IEC 7816-6 IC manufacturer code |
| **Firmware version** | **`45 00 4b 02`** | **Identical to bytes 0x10–0x13 of VENDOR's `sec_s3nrn82_firmware.bin` (dated 2018-10-15).** The chip runs exactly the image Android shipped, so no download is ever needed. Mainline's version check would also see "equal" (major 0x45, build 0x00/0x4b). |

Other results:

- **IRQ path works.** `irqs` went from 1 to 3: one rising edge per response
  on gpa1-2.
- **CLK_REQ stayed low throughout.** The host interface runs without the AP
  clock. Whether the RF side needs CLKOUT1 is still open (stage 2a).
- The chip sent no unsolicited notifications.
- **Revised MIFARE Classic outlook:**
  - The chip lists no proprietary RF interface, so Classic would have to go
    through the Frame interface with protocol 0x81.
  - The stock JNI's NXP `EXTNS_Mfc` layer suggests the Samsung HAL maps it,
    but this lowers confidence.
  - The "MIFARE Classic with dictionary keys" row stays
    **unknown-needs-test**, now leaning "no" (inferred).

Next is stage 2 (RF field, card reads). Its draft is above and is not approved
yet. The CLKOUT1 read below (stage 2a) is its first step.

### The read-only clock-output check (proposal, stage 2a)

From the vendor CAL (LineageOS universal9810,
`cal-if/exynos9810/clkout_exynos9810.c`, `clk-exynos9810.c`, `cal-if/ra.c`)
(from source):

- `OSC_NFC` = **`VCLK_CLKOUT1`**. (`OSC_AUD`, the codec's `xclkout`, is
  CLKOUT0.)
- CLKOUT1 is the **PMU register at offset 0xA0C**, which is physical
  **`0x14060A0C`** (PMU = `system-controller@14060000`). Its fields:
  - SEL = bits 13:8, where 0x0 = TCXO;
  - **bit 0 is a disable bit**: `ra_enable_clkout()` writes 0 to enable and 1
    to disable.
- **Proposed access:** **one 32-bit read** of `0x14060A0C` (devmem read, no
  write).
  - If bit 0 = 1, CLKOUT1 is gated, which explains why the chip never raised
    CLK_REQ.
  - The follow-up, needing separate approval, would be one read-modify-write:
    bits 13:8 = 0, bit 0 = 0. Then repeat stage 1c, and set bit 0 back to 1
    afterwards.

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
