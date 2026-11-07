# AirPods: ztos-podsd and ztPods

`ztos-podsd` (user service) holds an AAP channel to each connected pair and listens to BLE broadcasts. `ztpods` shows it. The status bar's icon and the button on Settings' Bluetooth page read `$XDG_RUNTIME_DIR/ztos-pods`. No root, no `main.conf` change, no vendor ID spoofing.

| File | What |
|---|---|
| `pods-proto.h`, `pods-aes.h` | Bytes: AAP packets, the advert. No I/O. |
| `pods-model.h` | State, commands, scan policy, the files. No I/O, no clock. |
| `ztos-podsd.c` | sd-bus, the L2CAP socket, timers, files |
| `pods-adv-capture.c` | A standalone capture of every advert in Apple's manufacturer data, for working out what the air carries |
| `ztpods.c` | The app. JSON and a socket; no Bluetooth. |
| `pods-bar-state.h`, `pods-bar.h` | The short file's reader; the status bar's drawing |

Tests: `make check-pods` (any machine, no libraries), `make check-podsd` (on the phone: the whole daemon against a pretend bluetoothd on a private bus and pretend pods on a unix socket; touches no radio).

`ZTOS_PODSD_TRACE=FILE` records every packet in hex, leaving out the key packet and device information (serials). Use it to turn real pods into test data.

## Where the facts come from

Seen on this phone, off a Pro 2 USB-C (A3047), 2026-11-07: AAP session, keys, a pod's advert resolving with the stored IRK and opening under the ENC_KEY, and the case's own advert. Other protocol facts are read from other projects, whose licences are GPL-3.0-or-later (LibrePods, LinuxPods) and GPLv3 (apple-wireshark). No code was copied; this is a C reimplementation from the facts they document, and these files say `GPL-3.0-or-later`, as ztOS's README does for `ztmetro/`. Thanks to all three.

| Fact | Where it was seen, by them |
|---|---|
| Session, handshake wait, 0x04, 0x06, 0x08, 0x0D, 0x31, 0x1D, advert decrypt, IRK reversal, gate before Off | Pro 2 USB-C and Pro 3 |
| Rename 0x1A | Pro 3 (Pro 2 assumed) |
| 0x1F, 0x25, 0x26, 0x28, 0x2E, 0x1B... values in the dump | Pro 3, AirPods 4 |
| Meaning of the other settings ids (0x0A, 0x01, 0x16, 0x17, 0x18, 0x23, 0x20, 0x31, 0x40, 0x35) | LibrePods' table, extracted from an Apple beta. **Nobody cited has checked them on hardware.** |
| Model capability table for anything but Pro 2/Pro 3 | LibrePods' table, not hardware. Hardware runs exist only for Pro 2 USB-C, Pro 3, AirPods 4 ANC. |
| Battery component 0x01 (Max) | LibrePods' enum; LinuxPods notes other counts are untried |
| Firmware field unpacking | One value, seen by LinuxPods |

Settings left out on purpose: spatial and head tracking (out of scope), 0x06 owns connection, 0x36 allow auto connect, 0x39 raw gestures, 0x3A temporary pairing, 0x41 disable button input (they change how the link or the stems behave, and nothing here would handle the result); 0x24, 0x2C, 0x2F, 0x33, 0x37, 0x38 (formats not documented, or needing vendor ID spoofing or hardware Pro 2 lacks). `0x02` capabilities is not decoded: rows are gated by the model table and by what the dump reports.

## Things the design relies on

- **Battery from broadcasts needs one connection here first**, for the keys. Before that, adverts are only "nearby": 10% steps, no proof of whose.
- **A closed case is not silent, on a Pro 2 USB-C (A3047).** Measured here: with the left pod out and the right in the closed case, the case itself sent `12 02 ac 00 07 11 06 …` every 2-4 s from an address of its own, opening under the pair's ENC_KEY to the case's level and the right pod's in 1% steps (case 21% as AAP had it; right 95 to 98 charging). Lid position is in byte 2's bit 3, on one opening seen so far, and in the pods' adverts when they are out. What LinuxPods measured, on a Pro 3, is the *pods* going quiet about 4 s after the lid shuts. **Not yet captured: a charger going in or out**, and a closed empty case.
- **Settings need a link.** No broadcast carries them, and nothing sets one without AAP. Without a link the app shows the last values, read-only, with their age, and a button that asks BlueZ to connect; only that tap ever connects.
- **Bursts, not advert monitors**: monitors need `Experimental = true`, and with no controller offload here they would scan continuously on the host. Discovery holds up LE devices reconnecting by directed advertising, hence short bursts: with the screen on, 4 s of 10 s with the app open and 3 s of 60 s otherwise, linked or not, for a pair we have keys for. With A2DP active: none if a pair is linked, 2 s of 20 s if nothing is and the app is open. With the screen off: none. A linked pair's music is never scanned beside until that has been measured on this chip.
- `mpris-proxy`'s AVRCP handling isn't touched: AAP's stem packets aren't read, and nothing here sends media keys.
