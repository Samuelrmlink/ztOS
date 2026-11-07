# ztOS fork roadmap

Working list for development on an SM-G960F (Exynos) from a Rocky Linux 10 PC.
Last updated 2026-11-06.

**Evidence:** **Confirmed** = seen in logs or code. **Likely** = strong reading of the evidence, not yet
proven. **Unknown** = symptom seen, cause not found. **Review** = found by reading code, not seen failing.
**Size** is a rough guess: S = under a day, M = a few days, L = a week or more.

## 1. Fix first

| # | Item | Size | Evidence |
|---|---|---|---|
| 1.1 | **Update the `zrn-bbd` driver (done on this phone, not a fix for 1.2).** The new `reset` file and logging work. Its escalating holds don't help: `MCU_RESP` reads high on good boots too, so "the chip did not reset" misleads and about 13 s of boot is wasted. Rethink the check. | S | Confirmed |
| 1.1a | **Fix the hidden Reboot button after a hub failure.** The full-screen warning's flag is never cleared when the shell continues by itself (`boot_check_sensorhub`), so its unseen buttons keep taking every touch. A touch in the Reboot area sends the phone to TWRP, and the lock screen and dialog can't be used. A one-line fix is written but not yet on the phone. | S | Confirmed |
| 1.1b | **Make the panel self-mask-off reliable.** Black lines appeared in rows 0-149 and 2810-2959, clean in screen captures. They persisted across clock updates and one resend of the mask-off commands, then cleared after a second resend followed by redraws. `ztos-panel-selfmask` sends its commands once, unchecked, without holding off frames, and packets are known to get dropped. Seen twice on this phone. | S | Likely; not proven |
| 1.2 | **Get the sensor hub starting after a full power-off.** Reproduced on the G960F: power-off then on fails (`ready=0`, chip never answers `lhd`). A pass through TWRP fixes it, and it then survives plain reboots. Ruled out: LDO37 cut over 30 s, and L30, L31, L38, L39, L42, L43 at their cold values. Upstream's snapshots show the same failure signature on its phone, so it is probably not model-specific. Cause not found. Next: try in the failed state, including holding standby with AP_SLEEP high as TWRP may. | L | Confirmed; cause Unknown |
| 1.3 | **Set up a kernel and module build.** `build/linux-7.1` and the toolchain are missing, and no script fetches them. 1.1, 1.4 and 1.5 depend on this. | L | Confirmed |
| 1.4 | **Rebuild prebuilts together and enforce it.** Prebuilts date from 14 and 26 Oct, while the boot image is from 14 Oct and kernel files changed through 30 Oct. All carry the vermagic `7.1.0-zboot`, so a mismatch loads silently. Add a CI check. | M | Confirmed (dates); harm Likely |
| 1.5 | **Fix the `btmgmt` kernel crash.** 21 NULL pointer oopses in about 20 s at boot, in `lsm_blob_alloc` via `hci_sock_create`. `bluetooth.ko` (26 Oct) is newer than the kernel (14 Oct), so an ABI mismatch is the first suspect. | M | Confirmed (crash); cause Likely |
| 1.6 | **Answer DHCP on USB.** `build.py:332` disables `systemd-networkd`, the only thing serving DHCP on `usb0`, and NetworkManager is told to ignore `usb0`. Hosts retry every 45 s and lose the connection. Check that enabling it doesn't conflict with NetworkManager on `wlan`/`eth`. Workaround: a static host profile (`ztos-usb`). | S | Confirmed |
| 1.7 | **Fix Wi-Fi going silent while connected.** The phone stays associated (5280 MHz) but ARP to the gateway and the PC fails, though DHCP worked at 21:48 UTC. Power save is on, and the driver logs `-52` errors. This blocks SSH over the LAN. | M | Unknown |

## 2. G960F support

| # | Item | Size | Evidence |
|---|---|---|---|
| 2.1 | Add variant detection (F vs. N). Nothing in the code distinguishes them. | M | Review |
| 2.2 | Report the real model. The device tree hardcodes `SM-G960N`, so the F shows the wrong name. | S | Confirmed |
| 2.3 | Use F-appropriate Wi-Fi data. The NVRAM is `STAR1 KR` with `ccode=KR`, built into the kernel. Check for an F calibration file and set the regulatory domain by region. | M | Review; effect Unknown |
| 2.4 | Check modem values on the F: board revision `0xae`, `system_rev`, `hw_rev`. A wrong revision caused LTE drops to 2G on the N. | M | Review; effect Unknown |
| 2.5 | Check the hub patch `G960NKSU5FVA2` against the F's vendor `lhd`. Lower priority for now, because the hub fails before a patch is sent. | S | Unknown |
| 2.6 | Confirm partition numbers `sda10`, `sda11`, `sda25` on the F. Flash scripts should also check the partition label before writing. | S | Review |
| 2.7 | Record dual-SIM and band differences between models. | M | Unknown |

## 3. Other phone bugs

| # | Item | Size | Evidence |
|---|---|---|---|
| 3.0a | **Bluetooth on the G960F (working).** Rebuilding the modules against the running kernel stopped the crash. Scan, discoverable only while the page is open, A2DP over SBC, playback, and pair and delete through Settings all pass with a UE MINI BOOM. The Status switch no longer reads Off while scanning. | M | Confirmed |
| 3.0a0 | **PipeWire not starting (fixed on this phone).** `lsp-plugins-ladspa` was missing from the package list, and the microphone configs required a capture device the old driver lacks. The package is added and the microphone objects are now optional. Not yet proven in a fresh install. | S | Confirmed |
| 3.0a4 | **Microphone (back on this phone).** Rebuilt `snd-zrn-abox` and the sound core against the running kernel; PipeWire waits for the card, and a recording shows signal. Not yet tested in calls, across sleep, or for quality. | M | Partly confirmed |
| 3.0a6 | **Sync the speaker's volume buttons with the phone**, and return to the other output's volume when the Bluetooth device disconnects. | M | New |
| 3.0a5 | **Bluetooth list order (fixed).** Rows keep their places while the page is open, and a tap acts on the device drawn in that row. Consider also asking before pairing with unnamed devices, since tapping one pairs at once. The "Not in range" row hasn't been seen on the phone yet. | S | Confirmed; host test and 5 scans |
| 3.0a2 | Pairing started from another device: Settings has no agent to answer it, and `Pairable` starts as `no`. Settings also says "ztOS Bluetooth Device", but the phone's name is `ztos`. | S | Confirmed (code) |
| 3.0a3 | No Bluetooth address found in EFS on this G960F, so it uses the chip's default. Check whether the F stores it elsewhere. Read EFS only. | S | Unknown |
| 3.0a1 | Bluetooth acceptance tests on the G960F: scan finds devices; the phone is discoverable while the Bluetooth page is open; a speaker pairs, connects and plays audio. AirPods Pro 2 follow in a later phase. | M | New |
| 3.0b | **Fully power down the modem.** Airplane mode now turns the radio off (`AT+CFUN=4`), but `cbd` and the modem processor keep running. Add a "modem off" state that doesn't start them, and handle `ztoscsf`, which restarts stopped units. `modem/README.md` warns a restarted modem may stop answering. | M | Confirmed (gap) |
| 3.0c | Add an option for airplane mode to also turn off Wi-Fi and Bluetooth. Off by default. | S | New |
| 3.0d | The modem reports `sim=error` and `bars=4` with no SIM fitted. Report "no SIM" and no signal. | S | Confirmed |
| 3.0e | `zrn-pushd` calls `ztos-waydroid-recover` at a path that doesn't exist on this phone. `snd_zrn_abox` also ignores `capture` and `mic_gain`, more signs of out-of-date modules (1.4). | S | Confirmed |
| 3.1 | `ztmetro` aborted twice (21:09 and 21:10 UTC) and the shell restarted. No core file was kept. Keep cores so we can get a backtrace. | M | Confirmed; cause Unknown |
| 3.2 | Three DRM warnings from `drm_atomic_helper_wait_for_vblanks`. Two came just after the shell crashes, so they may be linked. | M | Confirmed; cause Unknown |
| 3.3 | `/dev/binderfs` fails with `unknown filesystem type 'binder'`, which suggests the kernel lacks binder. Check the effect on Waydroid. | M | Confirmed; effect Likely |
| 3.4 | Read the last kernel crash log. `ztoscsf` reports the phone restarted after the kernel stopped, and `/sys/fs/pstore` needs root. | S | Confirmed |
| 3.5 | `zrn-cpuidle` won't load because a mark file says the last load never finished. Decide whether a fresh install should have that file. | S | Confirmed |
| 3.6 | `ztos-update-check` and `ztstore-install@samuel` fail. Possibly from 1.7; needs checking. | S | Confirmed; cause Unknown |
| 3.7 | Wi-Fi P2P interface creation times out, with other `brcmf` `-52` errors. May share a cause with 1.7. | M | Confirmed; cause Unknown |
| 3.8 | `zrn-bluetooth-addr` fails with `hci0 not registered`, though `hci0` exists now. Probably a boot-order issue or a result of 1.5. | S | Confirmed; cause Likely |
| 3.9 | Test real sleep on the F. `zrn-sleepd` reports `switched off` here, so it isn't sleeping. Find out why, and whether that's intended. | L | Confirmed; cause Unknown |
| 3.10 | `ztmetro.c:10186` sends `reboot_recovery`, but `ztoscsf` only handles `stop_sensorhub`. | S | Review |
| 3.11 | `zrn-android-root:28-34` has unchecked mounts, so it can report success with a broken root. | S | Review |
| 3.12 | `ztoscsf` runs the hub check on devices without that hub, and its retries block the main loop. | S | Review |
| 3.13 | `/run/ztoscsf.ctl` is mode 0666, so any local user can stop the hub. | S | Review |

## 4. Build and install

| # | Item | Size | Evidence |
|---|---|---|---|
| 4.0 | **Rebuild the Bluetooth modules during install.** Once the rebuild is proven by hand, add a script the installer runs so they always match the boot image being flashed. Needs the kernel source and compiler, or a prebuilt set checked against the boot image. Depends on 1.3 to 1.5. | M | New |
| 4.1 | RHEL-family host guide, build-tested on Rocky and Fedora. | M | In progress |
| 4.2 | Make `doctor` complete: subgid, `curl`, `xz`, `pyside6-uic`, Python version, `socat`, and a real run of the qemu interpreter. It currently passes with a broken one. | S | Review |
| 4.3 | Add a timeout and a clear error to `flash-root.sh`. A blocked port 5557 should hang with no message (`wait $server`, stderr discarded). | S | Review |
| 4.4 | Document the firewall step, and use the USB interface's zone rather than opening ports on Wi-Fi too. No script handles the firewall now. | S | Confirmed |
| 4.5 | Remove Arch-only paths and hints: `/usr/lib/qemu/*.so`, `/usr/lib/qt6/uic`, `pacman` in error messages. | S | Review |
| 4.6 | Document or relax the `stock/` requirement. `flash.sh` stops without those backups, and no install guide mentions them. | S | Review |
| 4.7 | Verify the Arch Linux ARM tarball. It's fetched over plain HTTP with no checksum check. | S | Review |
| 4.8 | Check why CI passes without installing `uic`, or fix it. | S | Likely |
| 4.9 | Make helper scripts work with any username. Several assume `alarm`. | S | Review |
| 4.11 | Replace `scripts/ztmetro-push.sh`'s hardcoded `alarm` login and `su` with something that works on installer builds. | S | Confirmed |
| 4.10 | Add a log-collection script, with IMEI and EFS data excluded. | S | New |

## 5. Security and housekeeping

| # | Item | Size | Evidence |
|---|---|---|---|
| 5.0 | Tighten sudo: `99-ztos-alarm` grants passwordless `/usr/bin/systemctl`, which is effectively root. | S | Confirmed |
| 5.1 | Review the rescue shell: unauthenticated root telnet on the USB network. Convenient for recovery, but it should be a conscious choice and documented. | S | Review |
| 5.2 | Fix out-of-date docs, for example the `ztoscsf` poll interval in `README.md`. | S | Review |
| 5.3 | Decide how to feed fixes upstream. Many items here affect all installer builds, not only the F. | S | New |
| 5.4 | Test changes on a G960N if possible. None is available here, so N compatibility is by code review only. | ? | Open |

## 6. Developer tools

| # | Item | Size | Evidence |
|---|---|---|---|
| 6.1 | **Live screen and touch control, same on phone and QEMU.** A `ztmirror` program on the device streams the screen and takes input over a direct TCP socket (USB-NCM in development). `scripts/ztctl.py` already does still captures and multi-touch over SSH, as a stopgap and for scripted tests. Design notes below. | L | New |
| 6.2 | Finish input coverage: hardware keys (power, volume, home, back) and a screen-size query. Up to ten overlapping fingers already work through the control pipe, and `cancel ID` is written but not yet on the phone. `ztmirror` must lift any held fingers if its client disconnects. | S | Confirmed (gap) |
| 6.3 | Check that wlr-screencopy captures the whole screen. The shell registers it but it's untested here, `grim` isn't installed, and windows on the hardware overlay plane may be missing from captures. | S | Unknown |
| 6.4 | Fix or retire `scripts/phone-shot.sh`, which only looks for KWin, Hyprland and phoc, not ztMETRO, and `scripts/phone-grab.sh`, which reads `/dev/mem` and needs a file the installer doesn't ship. | S | Review |

### 6.1 design notes

- **Device side:** one `ztmirror` binary for phone and QEMU. It captures with wlr-screencopy, encodes, and serves video and control over TCP. UDP is an option for video later if TCP latency is a problem.
- **Input:** `ztmirror` writes only touch and key commands to `$XDG_RUNTIME_DIR/ztmetro.ctl`, which runs them through the same code as a real finger. The pipe also accepts commands like `launch`, so the socket must never pass raw lines through.
- **Network exposure:** whoever reaches this port can use the phone. It's off by default and binds only to the USB address (`172.16.42.1`), or to the guest side of a QEMU `hostfwd` port. Never on Wi-Fi without authentication.
- **QEMU:** the same binary and protocol. Add a `hostfwd` entry to `qemu_command` (`installer/ztos_installer/targets.py`), so the PC client differs only in host and port.
- **PC side:** `ztos mirror --target phone|qemu` shows a window, maps the mouse to one finger, and adds a second finger for pinch. `ztos touch` scripts `down`/`move`/`up` for tests.
- **Wire format:** scrcpy's framing and touch messages where they fit. The scrcpy client itself probably can't connect, because it expects adb and an Android server.
- **Encoding:** software x264 at reduced resolution first, because `ffmpeg` and `x264` are on the phone and no `/dev/video*` encoder is visible. Check that the viewer's decoder is available on RHEL-family hosts.
- **Limits:** no frames arrive while the screen is off. Capture cost on the phone needs measuring.
