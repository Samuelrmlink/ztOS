# Flip covers on the Galaxy S9: hall sensors, Clear View, LED View (ROADMAP 8.34)

Research of 12 November 2026, host only. No phone access and no NFC was used. Part A's "What ztOS has" was added with the hall work on `feature/rotation-cover`.

**Evidence.** Each claim comes from one or more of these:

- **Stock BOOT.img** (`stock/BOOT.img`, kernel 4.9.118-23733677, July 2022). It carries 12 device trees (revs 16 to 26) and an embedded kernel config (IKCONFIG). I parsed both with a small Python script.
- **Samsung's starlte kernel** as published in the community tree `xxmustafacooTR/exynos-linux-stable`, branch `noble`. Links below are to that tree. `[K]` = `https://github.com/xxmustafacooTR/exynos-linux-stable/blob/noble/`.
- **The S9's own Android 10 system** (G960FXXUHFVG4, the same build as our factory zip), from the public dump `Exynos7870-labs/samsung_starlte_dump`. I read strings out of `NfcNci.apk`, `libnfc_nci_jni.so`, `LedCoverService.apk` and `floating_feature.xml`.
- **Decompiled One UI framework** (`488315/samsung_framework`, `services/sources/com/android/server/sepunion/cover/`). It is newer than the S9's, so it is a guide, not proof for the S9.
- **Galaxy S8 framework smali** (`GrifoDev/SuperMan-ModdedFiles`, `samsung-services.jar.out/.../cover/NfcLedCoverController.smali`).

I did not unpack the 5 GB factory zip. The BOOT.img and the dump of the same build covered the questions.

**Marks:** **confirmed** = read in stock binaries or source. **inferred** = follows from strings or code but not seen running. **guess** = my opinion, to be tested.

## A. The two hall sensors

### What the stock device tree says (confirmed, all 12 DTBs agree)

| | Flip hall | Certify hall |
|---|---|---|
| DT node | `/hall`, `compatible = "hall"` | `/certify_hall`, `compatible = "certify_hall"` |
| GPIO property | `hall,gpio_flip_cover = <&gpa3 6 0>` | `certify_hall,gpio_certify_cover = <&gpa0 2 0>` |
| Pinctrl node | `pinctrl@14050000/hall-irq` (the alive block) | `pinctrl@14050000/certify-hall-irq` |
| Function | `0xf` = external wake-up interrupt (EINT) | `0xf` = EINT |
| Pull (`pin-pud`) | `1` = **pull-down** on Exynos | `0` = none |
| Drive | 0 | 0 |
| Line level | **low = cover closed**, high = open | **low = cover attached**, high = none |

Source: `[K]arch/arm64/boot/dts/exynos/exynos9810-starlte_common.dtsi:74-103`, and the same nodes in every DTB inside `stock/BOOT.img`.

**Correction to HARDWARE-AUDIT.md:** the audit says "pull-up" for `gpa3-6`. On Exynos, `pin-pud = 1` is a pull-down (`EXYNOS_PIN_PULL_DOWN`, `include/dt-bindings/pinctrl/samsung.h`; pull-up is 3). The hall IC must therefore drive the line both ways (push-pull). That is inferred: an open-drain output with a pull-down could never read high.

### What the stock kernel does (confirmed)

Stock IKCONFIG: `CONFIG_HALL=y`, `CONFIG_HALL_EVENT_REVERSE=y`, `CONFIG_CERTIFY_HALL=y`, no `CONFIG_HALL_NEW_NODE`, no `CONFIG_SEC_FACTORY`.

| | `hall.c` (flip) | `certify_hall.c` |
|---|---|---|
| Input device name | `hall` | `certify_hall` |
| Event | `EV_SW` / `SW_LID` (0x00) | `EV_SW` / `SW_CERTIFYHALL` (0x1b, a Samsung-only code) |
| Value | **1 = closed**, 0 = open | **1 = attached**, 0 = detached |
| IRQ | threaded, rising + falling, oneshot | the same |
| Debounce | none in hardware. On open, a 10 ms delayed read (50 ms wakelock). On close, read at once | the same |
| Wake from suspend | yes. `enable_irq_wake` is called on every suspend, whatever `device_may_wakeup` says | yes, the same |
| First report | 500 ms after the input device is opened | the same |
| sysfs | `/sys/class/sec/sec_key/hall_detect` (`OPEN`/`CLOSE`) | `/sys/class/sec/sec_key/certify_hall_detect` (`CLOSE` means attached) |
| Off-mode charging | driver does not load | the same |

Sources: `[K]drivers/input/hall.c:99-147,171-174,262,347`, `[K]drivers/input/certify_hall.c:95-139,255,338`, `[K]drivers/input/Kconfig:218-256`.

**`CONFIG_HALL_EVENT_REVERSE`.** The GPIO reads 1 when open. Without the option, the driver reports `SW_LID = 1` when open, which is backwards for Linux. With it, the driver reports `!gpio`, so `SW_LID = 1` means closed, as Linux defines it (`hall.c:112-116`). The Kconfig text says "This was required from input FW" and ties it to Android Q or later. In short, with the option on (as stock is), the event matches mainline `SW_LID` exactly.

**What `certify_hall` is for (inferred).** It is not a check that the cover is genuine on its own. A Samsung smart cover has two magnets: one in the front flap and one in the back shell (a Samsung Community user reports this for Clear View covers). The flip hall sees the front magnet when the flap closes. The certify hall sees the back magnet as soon as the phone sits in the cover. The S9's NFC service then authenticates the cover over NFC and learns its type (part C). Kconfig agrees: "send a event when cover is detected".

**Sensor hub (`MSG2SSP_HALL_IC_ON_OFF`, 0x96).** In the fork, writing `SSP:HALL_IC=1` or `=0` to the hub's `ssp_control` sysfs file sends opcode 0x96 with one byte (1 = closed) (`[K]drivers/sensorhub/brcm/ssp_sysfs.c:598-608`, `ssp_dev.c:631-643`). It is re-sent after a hub reset (`ssp_dev.c:285`). The magnets probably skew the magnetometer, which is likely why (guess). I could **not** find the `SSP:HALL_IC` string in the stock BOOT.img, only `hall thr:` (the DT `ssp-hall-threshold`, which starlte does not set). So this message may be missing from the stock kernel. It is low priority for ztOS.

**The touch screen also has a cover mode.** The S9's `sec_ts` driver takes a factory command `clear_cover_mode,<2|3>,<type>`. It writes cover type to register 0x38 and sets bit 2 of the function register (`[K]drivers/input/touchscreen/sec_ts/sec_ts.h:214,386,514-527`, `sec_ts_fn.c:4770-4820`). Android sends this when a Clear View or LED cover closes. The touch IC then tunes itself for touch through the flap and limits touch to the window. Mainline `s6sy761` has no such command. For ztOS it is a later extra.

### What ztOS has for the flip hall (built 12 Nov, not yet loaded on the phone)

- **`sensors/hall/zrn-hall.ko`**, with no boot image change. Our device tree has no `__symbols__`, so a compiled overlay cannot name `&gpa3`. The module instead finds the bank's node by name (`gpa3-gpio-bank`), takes its phandle, and adds a `gpio-keys` node at run time (an `of_changeset`): `gpios = <gpa3 6 GPIO_ACTIVE_LOW>`, `EV_SW`/`SW_LID`, debounce 15 ms, `wakeup-source`. Mainline's `gpio-keys` reports it as the input device `zrn-hall`. The board values are module parameters in `sensors/hall/zrn-hall.conf` (for `/etc/modprobe.d/`), not in the code. Unloading removes the device and the node.
- **`kernel/make-dts.py --hall`** adds the same switch as a child of the board's `gpio-keys` node, for a later boot image. The module is not needed after that.
- **The shell** takes the switch as a wlroots switch device. Closed: the screen goes off, dark with no AOD (`aod_why`: "the flip cover is closed"). Opened: the screen comes on at the lock screen. Settings > display > "Flip cover turns the screen off and on" (`flip_cover`, on by default). Host-tested with the control pipe's `cover closed|open`.
- **Pin change on load (needs the owner's approval).** `gpio-keys` requests gpa3-6 as an input and its interrupt on both edges. The Exynos pin controller then sets the pin's function to EINT (0xf). The pull is not touched: no pinctrl state is given, so it stays as the bootloader left it. Stock sets pull-down (`pud 1`).
- `GPIO_ACTIVE_LOW` gives `SW_LID = 1` when closed, the same as stock.
- The certify hall could be a second switch on `gpa0-2`, active low, no pull. Mainline has no `SW_CERTIFYHALL`; `SW_DOCK` is the closest meaning. Not built: ztOS has no use for it until it knows cover types.

### Where the sensors sit in the S9 (not found)

I found **no** public board view, teardown note or service-manual page that places either hall IC. The SM-G960F service manual has a "Parts Layout" page, but the copy at manualmachine.com refused to load (HTTP 403). Owners report a Hall IC test in the `*#0*#` menu, without positions. Do not trust any position I could give. Find it by hand with the method in part D (one session, about 15 minutes).

## B. Clear View cover

**What stock shows (confirmed from Samsung retail and support pages).** The S9 Clear View Standing Cover (EF-ZG960) has a semi-transparent front, not a cut-out window. Closed, the phone shows the time, date, battery and notification icons. You can swipe through the flap to answer or reject calls and to control music. It also folds back into a stand.

**Window size (confirmed in newer framework code).** For a Clear View cover (type 8), the framework sets the "window" to the **full screen size**: `CoverVerifier.getScreenSizeForClearCover()` uses the display's real size. So there is no smaller region. On the S9 that is 1440 x 2960. One UI simply draws its clock in the top part.

**How the phone knows it is a Clear View (inferred).** Cover type numbers (`include/linux/input/sec_input.h` in newer Samsung kernels; the S9's `sec_ts.h` has the older list):

| Type | Cover | Type | Cover |
|---|---|---|---|
| 0 | Flip (plain) | 8 | Clear View |
| 1 | S View (window) | 11 | Neon |
| 2 | None | 14 | LED back cover |
| 7 | LED View | 255 | NFC smart cover |

The S9 ships with `com.sec.feature.nfc_authentication_cover`. When it is set, the framework does not read a cover ID pin. Older phones read the ID over a 1-Wire chip (`/sys/devices/w1_bus_master1/w1_master_check_id`); the S9 has no such path. Instead the S9's NFC service (`NfcCoverManager`) receives the attach event, runs `startCoverAuth`, then `transceiveAuthData`, then `CoverAttached(attached, coverId)`. It stores the ID in `nfc.cover.cover_id` (strings in the S9's `libnfc_nci_jni.so` and `NfcNci.apk`). The auth exchange is not public. Its name and a "fake cover" flag in `ScoverState` suggest it is a challenge-response check (guess).

**Proposal for ztOS (needs no NFC):**

1. The user picks the cover in Settings: *None / Plain flip / Clear View* (later *LED View*).
2. On `SW_LID = 1` with *Plain flip*: blank the screen and lock, as the power key does.
3. On `SW_LID = 1` with *Clear View*: lock, then show a "cover page": a large clock, the date, battery and up to 3 notification icons in the top third, on black. Use low brightness. Dim the panel after 10 s. Wake it for 10 s on a new notification or a power-key press, as stock does.
4. On `SW_LID = 0`: show the normal lock screen (or unlock if the lock was very recent).
5. Touch through the flap stays off at first. Without the touch IC's cover mode, the flap makes touch unreliable. Later: an "answer call" swipe in the cover page.

Effort: S for 2 and 4, M for the cover page (shell work). The certify hall is optional: it lets ztOS ask "Cover attached: which kind?" once.

## C. LED View cover and NFC-powered covers

**What it is.** The LED View Cover (S9: EF-NG960) has an LED dot matrix in the front flap and no battery. Samsung's own FAQ says NFC tag reading is restricted while the cover is in use. The cover also has a touch area, used to accept or reject calls and silence alarms.

**How stock detects it (inferred).** The detection works the same way as for Clear View. The certify hall reports attach, NFC authentication reads cover type 7, and the framework starts an LED controller. The S9 has `com.sec.feature.cover.nfcledcover` and `NFC_LED_COVER_LEVEL = 50` (`vendor/etc/floating_feature.xml`). It also ships `LedCoverService.apk` and `LedCoverAppStar.apk` ("Star" is the S9's code name).

**How stock powers and talks to it (confirmed in code, protocol partly):**

1. The NFC service gets `startLedCover()` (JNI `nfcManager_startLedCover`). It leaves normal tag discovery and starts an RF field for the cover. It returns a byte reply, and a reply of length 1 or less means failure. **Wireless charging is switched off** while the LED cover runs (`setWirelessChargeEnabled(false)`), and switched back on at stop.
2. Each command is one `transceiveLedCover(frame)` call. In both the S8 smali and the newer framework, the frame is: `00 A2 00 00 L 10 L CMD <data...> FF FF`, where `L = len(data) + 5`. A good reply contains `10 ?? D1 CMD FF FF` (S8 smali, `mResponsePattern = {0x10, 0x06, 0xD1, 0x00, 0xFF, 0xFF}`). What `A2` and `10` mean is not public. `A2` is the NFC Type 2 tag WRITE opcode, which fits a cover chip that looks like a T2T writable "mailbox" (guess).
3. The command numbers seen in code are: 15 = clock (data is ASCII `HHMM`, or `%2d%02d` for 12-hour), 3 = call duration, 11 = battery low, 12 = charging, 17 = poll touch, 18 = LED off, 6, 9, 13, 14, 19, 20 and 224 (meanings unknown). `LedCoverService` names further commands, such as `CMD_LED_CLOCK`, `CMD_LED_CALL_INCOMING`, `CMD_LED_NEW_MESSAGE`, `CMD_LED_MUSIC` and `CMD_LED_BATTERY_*`. Their byte values and the icon bitmap format would need a full decompile.
4. **Field on only while the LEDs are lit.** Each command arms an off timer: 3.7 s by default, 4.6 s for charging, 9 s for commands 13 and 14, 60 s for command 6. When it expires, the service sends LED off and calls `stopLedCover()`. While waiting for a touch answer, it polls with command 17 every 100 ms. On an error it retries up to 13 times, 500 ms apart. So the cover is lit in short bursts, not all the time.
5. **Power cost:** not published. An NFC reader field typically draws tens of mA while on (guess, not measured). Because stock keeps the field on for only a few seconds per event, the daily cost is small. A ztOS clock always lit on the cover would not be.

**S10 covers vs. S9 covers (partly confirmed).** Each phone has its own "LED cover level": S9 = 50, Note 10 = 70, S20 = 80, newer = 100 (floating features in public ROM trees). In the framework, levels 30 to 100 all hand commands to `LedCoverService`. The S9's copy of that app contains separate controllers per family: `Dreamy` (S8), `StarNfc` (S9), `BeyondLEDView` (S10), `HubbleLEDView` (S20). The S10 got its own controller, so its icon set or command set differ at least in part. **I could not confirm** whether an S10 cover answers S9 frames. The S10 Clear View is a type 8 cover authenticated over NFC like the S9's (inferred, same feature flags). The hall magnets work on any phone, because they are only magnets.

**Feasibility for ztOS:**

| Step | Needs | Effort | Risk |
|---|---|---|---|
| Detect "an LED cover is attached" | Certify hall only; type would come from NFC auth, which we cannot replay | S | none |
| Light the S10 LED cover held to the S9 | NFC stage 2 (approved RF), a raw NCI path to the S3NRN82, and the vendor `startLedCover` RF setup, which is inside Samsung's NFC firmware or JNI and not public | L | unknown proprietary NCI commands; RF writes to a chip whose firmware we must never change |
| Clock and icons | Frame format above plus the command and icon table, from a full decompile of `LedCoverService.apk` | M | low once step 2 works |
| Touch to answer | Command 17 polling | S | battery drain while polling |

**Verdict:** possible but expensive. It is a research project for after NFC stage 2. Even then the `startLedCover` RF setup may need vendor NCI commands that only Samsung's JNI knows. The JNI would have to be disassembled; I did not do that. The S10 cover is a poor first target. It does not fit the S9, so the NFC antennas will not line up, and its command set may differ. An S9 LED cover (EF-NG960, second-hand) would be the sensible target. A Clear View cover gives most of the value with no NFC at all.

## D. Owner's hands

### D1. Find the S9's hall sensor and test an S10 cover's magnet (no NFC)

Before you start: this needs `zrn-hall.ko` loaded (part A), which changes the pin's configuration and so needs your approval. Then `evtest` on the `zrn-hall` device shows `SW_LID`. (Reading the pin's level through debugfs needs no change, but only a snapshot.)

1. **Find the magnets in the S10 cover.** Hold a steel paperclip near the cover, inside and out, and slide it slowly over the front flap, then the back shell. Where it jumps or clings, put a small piece of tape. Expect one magnet in the front flap and one in the back shell.
2. **Find the S9's flip sensor.** Run `evtest` on the `zrn-hall` device. Take the cover's front-flap magnet (or any small fridge magnet) and move it slowly over the S9's screen. Hold it about 5 mm above the glass and go edge by edge: top edge, both long edges, bottom edge, then the middle. Note where `SW_LID` goes to 1, and mark that spot on the glass with a tape dot.
3. **Do the cover test.** Lay the S10 cover's front flap on the S9's screen so that its taped magnet sits over your tape dot. The S10 is taller and wider, so the cover will overhang. That does not matter. Open and close the flap 10 times. Every close should show `SW_LID 1` and every open `SW_LID 0`, with no bounces.
4. **Find the certify sensor (optional, later).** It would need a second switch on `gpa0-2`, which is not built. It is likely near the middle of the back (guess).
5. Do this for both covers (LED View and Clear View). Report the positions as "x mm from the top edge, y mm from the left edge, screen facing you".

### D2. Hold the LED View cover to the NFC antenna (only once NFC stage 2 is approved)

- **The S9's NFC antenna** shares a flex with the wireless-charging coil. It sits in the plastic cover on the midframe under the back glass, in the middle of the back, below the camera and fingerprint sensor. iFixit's guide "Samsung Galaxy S9 NFC Antenna and Charging Coil Replacement" (ifixit.com/Guide/116657) shows it.
- **The cover's antenna** is in its back shell, because the S9 pairs with it through the phone's back. A flex cable ("FPCB") runs through the spine to the LEDs in the flap. The `NFC_LED_COVER_FPCB_DISCONNECT` message in stock code refers to that cable.
- **What to do:** lay the S9 face down. Put the S10 LED cover's back shell flat on the S9's back, centred on the middle of the phone. Keep the flap folded so its LEDs face you. Do not use a metal surface underneath. Nothing should happen until ztOS sends a start command. If the cover lights, note the position and keep it still.

## Open questions

- Where both hall ICs sit (D1 will answer it).
- Whether stock really sends `MSG2SSP_HALL_IC_ON_OFF`. The string was not found in the stock kernel.
- The LED cover's start sequence on the RF side and the full command and icon table. These need a disassembly of `libnfc_nci_jni.so` and a decompile of `LedCoverService.apk`.
- Whether an S10 LED cover answers the S9 ("Star") protocol.

## Sources

- Stock: `stock/BOOT.img` (DTBs and IKCONFIG); dump `github.com/Exynos7870-labs/samsung_starlte_dump` (branch `starltexx-user-10-QP1A.190711.020-G960FXXUHFVG4-release-keys`): `system/system/etc/permissions/com.sec.feature.*cover*.xml`, `vendor/etc/floating_feature.xml`, `system/system/lib64/libnfc_nci_jni.so`, `system/system/app/NfcNci/NfcNci.apk`, `system/system/priv-app/LedCoverService/LedCoverService.apk`.
- Kernel: `github.com/xxmustafacooTR/exynos-linux-stable` (branch `noble`), files cited above.
- Framework: `github.com/488315/samsung_framework/tree/main/services/sources/com/android/server/sepunion/cover` (`CoverVerifier.java`, `Feature.java`, `CoverManagerServiceImpl.java:643-657`, `NfcLedCoverController.java:523-562,724-774,918-947`); S8: `github.com/GrifoDev/SuperMan-ModdedFiles` `samsung-services.jar.out/smali/com/android/server/cover/NfcLedCoverController.smali:284-302,413-471`.
- Cover type enum: `github.com/FlopKernel-Series/flop_exynos2100_kernel` `drivers/input/sec_input/sec_input.h:500-527`.
- LED cover levels: `github.com/salvogiangri/UN1CA` `unica/patches/legacy/customize.sh:614-623`; `target/*/sff.sh` in ArtisanROM and ImpulseROM trees.
- Two magnets in a Clear View cover: Samsung Community (S20 FE thread), via search; two hall sensors on S10: forums.androidcentral.com/threads/s10-smart-cover-magnet-sensor-location-s.1004397.
- Samsung: "Will device NFC work if equipped with LED cover?" samsung.com/uk/support/mobile-devices/will-device-nfc-work-if-equipped-with-led-cover; S9 Clear View Standing Cover: samsung.com/hk_en/mobile-accessories/clear-view-standing-cover-galaxy-s9/EF-ZG960CBEGWW/; S9 LED View Cover: samsung.com/za/mobile-accessories/led-view-cover-for-galaxy-s9/EF-NG960PBEGWW/.
- NFC antenna: ifixit.com/Guide/Samsung+Galaxy+S9+NFC+Antenna+and+Charging+Coil+Replacement/116657.
