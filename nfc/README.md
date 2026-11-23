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

### Stage 2a result, 11:58 CST: CLKOUT1 is gated

The owner approved stage 2 (2026-11-12), clock write excluded. Its first step
was run inside `ztlock run nfc` (battery 22%, charging), with
[`driver/clkout1-read.py`](driver/clkout1-read.py): one aligned 32-bit read
through `/dev/mem`, no write. Observed:

```
CLKOUT1 @0x14060a0c = 0x00000001: SEL=0x0 DISABLE=1 -> GATED
```

**The NFC reference clock is off.** SEL is already TCXO (0). Only the disable
bit is set. That fits CLK_REQ never rising, and a chip configured for an AP
clock (`FW_CFG_CLK_SPEED=0x12`, `sec-nfc,nfc_ap_clk`). As instructed, **stage 2
stopped here**: no LDO, no expander access, no NCI command and no RF field.

**Proposal: stage 2b, for owner approval.** One write before the stage-2
session, and one write after it:

- **Before:** write `0x14060A0C` = `0x00000000` (clear bit 0; SEL stays 0 =
  TCXO), then read it back. This is what the vendor's `ra_enable_clkout()`
  does for `OSC_NFC`.
- **After:** write `0x14060A0C` = `0x00000001` (the value read today), then
  read it back.
- Same `/dev/mem` method, inside the same `ztlock run nfc`. Stock gates the
  clock dynamically from CLK_REQ. Leaving it running only for the session is
  the simple equivalent.
- Side effects: a 26 MHz output on the clock pad for the session. CLKOUT0
  (audio) is a different register and is not touched.

**Second request (also needs approval):** `RF_DISCOVER_MAP_CMD` (21 00),
poll-mode mapping of protocol ISO-DEP (0x04) to the ISO-DEP RF interface
(0x02).

- This is volatile NCI configuration.
- Without it, NCI 1.0 controllers activate ISO-DEP cards on the Frame
  interface. The host would then have to send RATS and do the ISO-DEP block
  framing itself, and neither is in the approved list.
- Type 2 (NTAG/Ultralight) and FeliCa reads work without it.

## Stage 2 runs: the open-path NACK, then DISCOVER_MAP rejected

**Run 1 (2026-11-22 17:38 CST), card: MIFARE Classic 1K.** The first
`os.write` of CORE_RESET returned `ENXIO`, an address NACK. Cause: `open()`
powered the chip ~7 ms after LDO38 came on, while stage 1c (which answered)
had the supply up for hundreds of ms first. Fixed: `open()` now holds VEN off
for `SUPPLY_SETTLE_MS` = 1000 ms after LDO38, checks SCL/SDA read high
(`zrn_i2c20_lines`), and enables CLKOUT1 only just before the first
`RF_DISCOVER`. No field, everything restored.

**Run 2 (2026-11-22 17:51 CST), same card.** The open path now works. The chip
answered CORE_RESET, CORE_INIT and CORE_SET_CONFIG. It then **rejected
`RF_DISCOVER_MAP`**:

```
TX 21 00 07 02 04 01 02 81 01 01     map: ISO-DEP->ISO-DEP, MFC(0x81)->Frame, poll
RX 41 00 01 01                        RF_DISCOVER_MAP_RSP status 0x01 = REJECTED
```

ztnfc stopped on the error. **No RF field was ever enabled** (we never reached
`RF_DISCOVER`). The kernel restored everything: LDO38 `0xac`->`0x2c`, CLKOUT1
back to gated on close, chip off, modules unloaded, gpp5 released. No
`/var/lib/zt-nfc` file was created. The supply-settle fix is confirmed: the
earlier NACK is gone.

### Diagnosis (inferred)

NCI status `0x01` is `STATUS_REJECTED`. The map carries two entries; the new
one is **protocol 0x81 (the proprietary MIFARE number from
`NFA_PROPRIETARY_CFG`) mapped to the Frame RF interface (0x01)**. The most
likely cause is that the S3NRN82 firmware does not accept 0x81 as a mappable
protocol in `RF_DISCOVER_MAP`, or does not allow the Frame interface as a map
target. The single-entry ISO-DEP map (`21 00 04 01 04 01 02`), approved
earlier, was never sent on the phone, so we don't yet know whether the chip
accepts *any* `RF_DISCOVER_MAP`, or only rejects the 0x81 entry.

This is **not** a 0x2F command and **not** an NVM write; `RF_DISCOVER_MAP` is
volatile config. Nothing in the stop was outside the approved set.

### Proposal: one diagnostic run (needs approval)

Decide which, then one `ztlock run nfc`, same battery and restore rules:

1. **ISO-DEP-only map, then poll.** Send `21 00 04 01 04 01 02` (the
   originally approved single entry). If it is accepted, `RF_DISCOVER` poll
   A/B/F/V, present the Classic card, and **read the `RF_INTF_ACTIVATED_NTF`
   only** (type, ATQA/SAK, UID length, redacted; protocol and interface
   numbers). This answers the real question: on this chip, does a MIFARE
   Classic card activate as protocol 0x81 on the Frame interface by itself? If
   it does, auth/read can follow without mapping 0x81 at all.
2. If even the ISO-DEP-only map is rejected, send **no `RF_DISCOVER_MAP`** and
   rely on the chip's default mapping, then poll and read the activation the
   same way.

Either way, the follow-up only identifies the card (no auth, no read) until we
know the activation interface; the field stays under the 30 s watchdog. If the
chip reports Classic on an interface we can reach, the existing allowlist and
`ztnfc` reader handle auth and READ unchanged. ztnfc is updated to fall back
automatically: if the two-entry map is rejected, it retries the ISO-DEP-only
map, and if that is rejected it sends no map (all three are already in the
approved command set except they would run in one session; this needs the
owner's ok for the retry-in-one-session).

## Stage 2 run 3 (identify only, 2026-11-22 17:55 CST): field on, no card seen

Identification only, no authentication (owner-approved). Card: the MIFARE
Classic 1K, flat on the back; wireless pad online = 0; battery 24%. Observed:

```
CORE_RESET/INIT/SET_CONFIG: ok
TX 21 00 07 02 04 01 02 81 01 01   RX 41 00 01 01   two-entry map REJECTED
TX 21 00 04 01 04 01 02            RX 41 00 01 00   ISO-DEP-only map ACCEPTED
TX 21 03 09 04 00 01 01 01 02 01 06 01  RX 41 03 01 00   RF_DISCOVER poll A/B/F/V ok
(field on, CLKOUT1 enabled)  -> polled 20 s -> no card found
(close-down RF_DEACTIVATE NACKed; kernel powered the chip down and restored all)
```

Everything restored: LDO38 `0x2c`, CLKOUT1 gated again, chip off, modules
unloaded, gpp5 released, phone clean.

Findings (observed, then inferred):

- **The chip accepts an ISO-DEP-only `RF_DISCOVER_MAP` and `RF_DISCOVER` in
  poll mode; the field turns on.** So the earlier rejection was specifically
  the protocol-0x81 / Frame-interface map entry, not DISCOVER_MAP in general.
- **No card was detected in 20 s**, although a Classic card that reads on
  other phones was in place. The chip reported no `RF_DISCOVER_NTF` and no
  activation.
- The close-down `RF_DEACTIVATE` was NACKed. The chip likely went to sleep
  (`SLEEP_TIMEOUT=1000` in the vendor config) during the idle poll and a plain
  I2C write no longer reached it; the kernel then powered it down regardless,
  so nothing was left on. ztnfc now treats that last write as best-effort.

### Why no card was seen (inferred), and the 0x2F stop

The most likely cause is that **the chip's RF/analog configuration was never
loaded.** Stock loads `sec_s3nrn82_rfreg.bin` (the per-board antenna / front-end
register block seen in `/vendor/etc/nfc/`) through the **proprietary 0x2F
commands** `START_RFREG (0x2F 0x26)`, `SET_RFREG (0x2F 0x22)` and
`STOP_RFREG (0x2F 0x27)` — the same sequence mainline's
`s3fwrn5_nci_rf_configure()` uses. Without it, the receiver/antenna tuning is
at power-on defaults and a card in the field is not detected. The HAL strings
(`Set RF register version`, `hal_vs_rfreg_update`, `Do not need to update RF
registers`) show the RF registers are **versioned and written only when they
differ**, like the firmware, so `SET_RFREG` is very likely an **NVM write**.

That is exactly the **stop-and-report** condition: stock's read path needs a
0x2F proprietary command, and that command probably writes the chip's
non-volatile memory. **I did not send it.** No 0x2F command and no RF-register
write was issued in any run.

A second possibility is antenna coupling: on the SM-G960 the NFC loop and the
WPC coil overlap but the best read spot is not exactly the coil centre. That is
cheap to retest (reposition the card) and needs no new command.

### What this leaves

- **Confirmed reachable without any 0x2F:** power-up, NCI version/firmware
  read, `CORE_SET_CONFIG`, an ISO-DEP `RF_DISCOVER_MAP`, and `RF_DISCOVER`
  polling with the field on.
- **Blocked on the RF-register load:** actually detecting and reading a card,
  Classic or otherwise, if the no-detect is the RF config and not positioning.
  Resolving it needs either (a) an owner decision on the one-time RF-register
  load (a 0x2F sequence that likely writes NVM — its own approval, and it would
  change the chip's stored RF block), or (b) a reposition-and-retry to rule out
  coupling first.

The reader, the allowlist and the restores all work; the open question is
purely the analog RF bring-up, which the approvals deliberately fence off.

## Stage 2 run 4 (closed loop with a Chameleon Ultra, 2026-11-22 19:45 CST)

The owner put a Chameleon Ultra (emulator, firmware 2.3.0) flat on the phone's
back, bridged to the phone over BLE by `cubridge`. Sleep was held off with a
`/run/zrn-sleepd/inhibit/nfc-test` file (mtime = expiry), removed on every exit
path. Two identify-only scans, no authentication.

What actually happened (observed):

- **`cubridge` only permits slot 8** (`writable-slots: [8]`); it refused
  `set-active-slot 4` ("denied"). So the active slot was **8 (MIFARE
  Ultralight MF0ICU1)** for both scans, and it is **still 8** — I could not set
  it back to 4. **The coordinator/owner must restore slot 4;** cubridge as
  configured won't let this agent do it.
- **Scan 1** hit the intermittent open-path NACK (`CORE_RESET 20 00 01 00` →
  `-ENXIO`, retry failed). No poll. The 1 s supply settle is not always enough;
  run 2 (stage 1c) and stage-2 run 2 both answered, so it is marginal, not
  dead. ztnfc now needs its `CORE_RESET` send wrapped too (it raised instead of
  logging); the kernel still restored everything.
- **Scan 2** reached polling with the field on, and in 20 s **detected no
  card**, but the chip sent a **proprietary notification**:

  ```
  TX 21 03 09 04 00 01 01 01 02 01 06 01   RF_DISCOVER poll A/B/F/V, accepted
  NTF 6f 3f 01 80                          proprietary NTF: GID 0x0F, OID 0x3F, data 0x80
  no card found
  ```

Everything was restored (LDO38, CLKOUT1, chip off, modules unloaded, gpp5
released, inhibit removed, lock free).

### Reading of run 4

- **The phone's reader still detects nothing**, now against a live emulator
  sitting on the coil, not just a passive card. That rules out the card and
  all-but-rules-out gross positioning: the Chameleon is right on the back.
- The **proprietary NTF `6F 3F 01 80`** during polling is the Samsung
  firmware signalling a vendor condition while the field is on. It is not a
  standard NCI notification; it is most consistent with an RF/front-end state
  the firmware flags when its RF registers have not been programmed.
- Together with the passive-card run, this points hard at the **RF analog
  configuration never being loaded.** The digital/NCI path is fully alive
  (reset, init, config, map, discover, field on); only detection fails.

## RF-register load proposal (0x2F 26/22/27) — for the owner to decide, not sent

Stock programs the chip's RF/analog front-end at startup by streaming
`sec_s3nrn82_rfreg.bin` (observed: `/vendor/etc/nfc/`, **3056 bytes**, header
`c0 9e 00 80 c0 07 4c 00 ...`) through three **proprietary (GID 0x0F) NCI
commands**, exactly as mainline `s3fwrn5_nci_rf_configure()` does (from source,
`drivers/nfc/s3fwrn5/nci.c`):

| Step | Command | Bytes | Meaning |
|---|---|---|---|
| (opt) FW clock cfg | `NCI_PROP_FW_CFG` | `2F 28 03 <clk_type> <clk_speed> <clk_req>` | clock source/speed. The board's values must come from the HAL config (`FW_CFG_CLK_SPEED=0x12`), **not** mainline's crystal defaults (`01 ff ff`), which are wrong here. |
| start | `NCI_PROP_START_RFREG` | `2F 26 00` | begin the RF-register update |
| data ×N | `NCI_PROP_SET_RFREG` | `2F 22 <len> <index> <≤252 bytes>` | the blob in ≤252-byte chunks; 3056 bytes ≈ 13 writes, index 0,1,2… |
| stop | `NCI_PROP_STOP_RFREG` | `2F 27 02 <checksum16>` | checksum = sum of the blob's 32-bit words, low 16 bits |

**What it changes, and the risk:**

- It **writes the chip's RF register block**. The HAL treats these registers
  as **versioned** (`Set RF register version`, `hal_vs_rfreg_update`, `Do not
  need to update RF registers`), i.e. stored and rewritten only when the
  version differs — the same pattern as the firmware. So `SET_RFREG` is very
  likely a **non-volatile write**, and `START_RFREG` probably prepares/erases
  that area.
- This is **doubly gated** under the phone rules: it is a **0x2F proprietary
  command** *and* a probable **NVM write**. It must not run without the owner's
  explicit approval. A failure mid-stream could leave the RF block
  inconsistent (the firmware likely keeps a version/backup, but that is not
  confirmed).
- It does **not** touch the main firmware image, the secure element, MST, or
  EFS/BOOT. It is confined to the NFC chip's own RF configuration.

**If approved**, the safe shape would be: read back the current RF-register
version first (a read-only vendor command, if one exists), stream the vendor
blob unchanged, verify the checksum RSP, then rerun the identify scan to see
whether detection starts working. I would write the exact byte stream and the
chunk boundaries for review before any such run, and treat the whole thing as
one owner-approved NVM operation.

### Version, rollback and a better first suspect (added after run 4)

- **The file's version (observed).** `sec_s3nrn82_rfreg.bin` ends with a
  trailer: `45 4b 41 00` ("EKA"), `01 4b 1d 0e 0e 21 01 11`, `44 45 46 00`
  ("DEF"). The HAL prints the RF version as six numbers
  (`RF version: %d/%d/%d/%d.%d.%d`) and compares "RF Version (BIN)" against
  "RF Version (F/W)", so this trailer is most likely that version record
  (inferred). There is a second, **different** file under `BRI/` (md5
  `58695bf8…` vs `d8c4edf9…` for the main one); which one this board uses is
  decided by the HAL and is not known for certain (the main one is the likely
  default).
- **The chip's RF version** cannot be read without a vendor (0x2F) query; it
  is not in `CORE_INIT` (that carries only the firmware version
  `45 00 4b 02`). So "blob vs chip" is **unknown**.
- **Probably already loaded.** Stock Android ran on this phone, and its HAL
  rewrites the RF registers only when the versions differ. So the chip very
  likely **already holds this exact RF block** in NVM. If so, the rfreg load
  would change nothing — and would not explain the missed detection.
- **Rollback.** There is no read-back of the current RF block, so the only
  rollback is to stream a known blob again (the same file, or the `BRI/`
  one). If the chip already holds this blob, re-streaming it is a no-op in
  effect, but it still costs an NVM write cycle and a power-loss window.
- **Risk.** A 0x2F command plus a probable NVM write of the RF block. An
  interruption mid-stream could leave it inconsistent until it is re-streamed.
  Main firmware, eSE, MST and EFS/BOOT are not touched.

**A better first suspect: the clock configuration (`FW_CFG`, 0x2F 28).**
In every run, CLK_REQ stayed low — even with the field on. The Samsung HAL
calls `hal_nci_send_prop_fw_cfg` during init and checks for
`NCI_PROP_FW_CFG_RSP`, with `FW_CFG_CLK_SPEED=0x12` from the vendor config.
That command tells the chip which reference clock to use (type, speed, and
whether to request it through CLK_REQ). Without it, the chip likely runs its
RF PLL on a default it cannot lock to this board's 26 MHz AP clock, so the
field is wrong or absent and no card answers — and the chip never asks for
the clock. This fits all observations better than missing RF registers.

- Bytes: `2F 28 03 <clk_type> <clk_speed> <clk_req>`, with `clk_speed = 0x12`
  (vendor config). `clk_type` and `clk_req` are **not** in the vendor config;
  their HAL defaults would have to be recovered from `nfc_nci_sec.so` before
  this can be proposed with exact bytes. Mainline's `01 ff ff` (external
  crystal) is wrong for this board.
- Whether `FW_CFG` is volatile or stored is **unknown**. The HAL sending it on
  each init suggests volatile, but this is not confirmed. It is a 0x2F command
  either way and needs explicit approval.

Suggested order, each a separate owner decision: (1) the position sweep
below; (2) recover `FW_CFG`'s `clk_type`/`clk_req` from the HAL (read-only
analysis of a VENDOR file), then propose `FW_CFG` alone; (3) only then the
rfreg stream.

### Position sweep (proposal, no new commands)

Three identify-only runs, at most 20 s each, with the owner moving the
Chameleon between runs (the S9's NFC loop is on the back-cover flex around
the wireless-charging coil, so the best spot may be off-centre):

1. centre of the coil;
2. upper back, between the coil and the camera;
3. lower back, below the coil.

Each run uses the already-approved path (fixed open, ISO-DEP-only map, poll
A/B/F/V, the single page-0 READ if T2T activates). The open path gets a
short retry on `CORE_RESET` so a marginal start does not waste a run.

**Cheaper first step (no 0x2F):** the owner can reposition the card/emulator
over the exact NFC loop and rerun the already-approved identify scan, to rule
out coupling before touching the RF registers. The digital path is ready; only
the analog bring-up is in question.

## FW_CFG (clock config) — exact bytes, from the HAL (for owner approval; not sent)

Read-only disassembly of `/vendor/lib64/nfc_nci_sec.so` (copied off the
read-only VENDOR mount; md5 `e3f3ebb0…`), function
`hal_nci_send_prop_fw_cfg(u8)` at `0x17a28`. This is what stock sends, and it
is the prime suspect for the dead RF front-end (CLK_REQ never rose in any run).

### What the HAL builds (from source / disassembly)

- The packet starts `2F 28` (`strh #0x282f` → little-endian `2f 28`): GID 0x0F
  proprietary, OID 0x28 = `NCI_PROP_FW_CFG`.
- The payload length and fields depend on the chip's firmware major
  (the argument, taken from `nfc_hal_info.version`, compared to `0x70`):
  - **major < 0x70 (ours is `0x45`): payload length 3, fields in order
    `CLK_TYPE, CLK_SPEED, CLK_REQ`** (written to stack offsets 3, 4, 5).
  - major ≥ 0x70: payload length 1, just `CLK_SPEED`.
- Each field is read with `get_config_int` from these keys, **defaulting to 0**
  if the key is absent (`stur wzr` on the failure path):
  - offset 3 ← `FW_CFG_CLK_TYPE`
  - offset 4 ← `FW_CFG_CLK_SPEED`
  - offset 5 ← `FW_CFG_CLK_REQ`
  - The HAL first tries a hardware-rev-suffixed key (`%s_REV%d`, i.e.
    `FW_CFG_CLK_SPEED_REV<hwrev>`), then the plain key.
- It is sent with `__send_to_device`, and the reply checked is
  `NCI_PROP_FW_CFG_RSP` (`4F 28 …`).

### The board's values (observed)

`/vendor/etc/libnfc-sec-vendor.conf` on this phone contains **only**
`FW_CFG_CLK_SPEED=0x12` — no `FW_CFG_CLK_TYPE`, no `FW_CFG_CLK_REQ`, and no
`_REV`-suffixed variants (confirmed by reading the whole file; it is the only
NFC config file the HAL's `get_config` uses). So the two missing fields default
to 0, and for our firmware (major 0x45 < 0x70) the exact packet is:

```
2F 28 03 00 12 00
      │  │  │  └ CLK_REQ   = 0x00 (key absent -> default)
      │  │  └ CLK_SPEED = 0x12 (FW_CFG_CLK_SPEED, vendor conf)
      │  └ CLK_TYPE  = 0x00 (key absent -> default)
      └ payload length 3
```

Expected reply: `4F 28 01 00` (status 0x00) — `does not support FW_CFG!!` is
the HAL's error string if the bootloader rejects it.

### Stored or volatile?

**Most likely volatile** (inferred): the HAL sends FW_CFG from the open/init
and firmware state machines with **no version-compare/skip** around it — unlike
the RF registers, which it rewrites only when their version differs. A clock
setting the chip needs told to it on each power-up fits a volatile,
per-session register. This is **not certain** from static analysis; if the
owner treats it as possibly-NVM, that only raises the approval bar, it does not
change the bytes.

### Risk and rollback

- One proprietary 0x2F command, 6 bytes, the exact bytes stock sends on this
  board. It configures the chip's clock source; it does not touch the firmware
  image, the RF-register block, the eSE, MST, or EFS/BOOT.
- If volatile (likely), it clears at the next power-down; rollback is a
  power-cycle (our driver drops VEN on close). If it turns out to persist,
  there is no read-back, so rollback would be re-sending a known value — but
  `00 12 00` **is** the board's stock value, so re-sending it is the baseline.
- Worst case on a bad value: the chip mis-locks its clock and still doesn't
  detect — i.e. no worse than now, and cleared by the power-cycle on close.

### Proposed use (one run, after approval of these exact bytes)

In the stage-2 open path, right after `CORE_INIT` and before `CORE_SET_CONFIG`:
send `2F 28 03 00 12 00`, require `4F 28 01 00`, then the normal identify-only
poll. The kernel allowlist would gain exactly this one 0x2F packet (that opcode
and body only); nothing else 0x2F stays allowed. The `CORE_RESET` retry is
already added. If the chip then detects the Chameleon, FW_CFG was the missing
step; if not, the rfreg load (already drafted, still on hold) is next.

## FW_CFG run (2026-11-22 20:45 CST): reply 4F 28 02 01 00, session stopped

**LDO38 leak, found and fixed first.** Our sessions did restore LDO38: the
19:46 session logs `restored to 0x2c (reads 0x2c)`. The 0xac seen at 20:32
came from the boot chain after the aod-panel reboots at 20:04 and 20:06: no
Linux code wrote it in those boots. The older mainline register dump shows
the same 0xac after a boot. `zrn-i2c20` kept LDO38 "as found", so it left the
rail on. Now:

- the last `zrn_i2c20_put()` always switches LDO38 off, leaving 0x2c;
- `zrn_i2c20_get_cycled()` holds it off first if it was on;
- the rail was switched off by hand before this run (read back 0x2c).

This run logged `LDO38 0x2c -> 0xac` at the start and
`LDO38 off (reads 0x2c; …)` at the end.

**Open path (timing only).** VEN is held off for 200 ms (with LDO38 off too,
if it was on). Then LDO38 comes on, settles for 1 s, VEN goes on, and the
driver waits 150 ms. ztnfc tries CORE_RESET up to 5 times, 400 ms apart.
This time CORE_RESET was acked on the first try.

Run (Chameleon on slot 8, inhibit held, battery 95%, wireless 0):

```
TX 20 00 01 00             RX 40 00 03 00 10 01          CORE_RESET ok
TX 20 01 00                RX 40 01 14 … 0e 45 00 4b 02  CORE_INIT ok
TX 2f 28 03 00 12 00       RX 4f 28 02 01 00             FW_CFG reply: 2-byte payload 01 00
FW_CFG reply not 4f 28 01 00: stop
```

As required, the session stopped on the unexpected reply. **There was no
RF_DISCOVER and no field.** CLK_REQ stayed 0 for the short session. Slot 4
was restored, the inhibit removed, and the chip powered off. LDO38 is 0x2c.

### Reading the reply (inferred)

`4F 28` is the proprietary FW_CFG response, with a **2-byte** payload `01 00`
where a 1-byte status was expected. There are two readings:

- **Mainline layout** (`struct nci_prop_fw_cfg_rsp { u8 status; }`): the
  first byte is the status. `0x01` is NCI `STATUS_REJECTED`, so the chip
  refused the config.
- **A Samsung 2-byte layout**: one leading byte, then status `0x00`, meaning
  accepted.

The HAL's own evidence leans towards "this chip does not take FW_CFG":

- In `nfc_hal_open_sm`, stock sends FW_CFG only when the bootloader's bootinfo
  version word is greater than 0x6f. Otherwise it sends a plain CORE_RESET.
- `nfc_hal_fw_sm` gates FW_CFG support on further bootinfo bytes
  (`[148] == 0x84`, `[151]`), and logs
  `This version of bootloader does not support FW_CFG!!` otherwise.
- We never read this chip's bootinfo; that needs bootloader mode, which is not
  approved. So we don't know whether stock sends FW_CFG on this phone at all.

### Options (each needs its own approval)

1. **Treat `4F 28 02 01 00` as accepted** and rerun with the poll: the same
   FW_CFG, then the identify scan. That is cheap, but it rests on the second
   reading.
2. **Read the bootinfo**: one GET_BOOTINFO (`00 01 00 00`) in bootloader mode
   (FIRM high at power-up). This is a read-only command, but bootloader mode
   is where flash writes happen. It would tell us whether stock sends FW_CFG
   on this chip, and in which form.
3. **Skip FW_CFG** and go to the rfreg stream (approved, on hold).

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
