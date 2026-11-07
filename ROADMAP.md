# ztOS fork roadmap

Working list for development on an SM-G960F (Exynos) from a Rocky Linux 10 PC.
Last updated 2026-11-07.

**Evidence:** **Confirmed** = seen in logs or code. **Likely** = strong reading of the evidence, not yet
proven. **Unknown** = symptom seen, cause not found. **Review** = found by reading code, not seen failing.
**Size** is a rough guess: S = under a day, M = a few days, L = a week or more.

## 1. Fix first

| # | Item | Size | Evidence |
|---|---|---|---|
| 1.1 | **Update the `zrn-bbd` driver (source fixed; not on the phone yet).** The new `reset` file and logging work. The escalating holds are gone: `MCU_RESP` reads high on good boots too, so it is now only logged, with one parked standby of `standby_ms` (1000 ms) at each probe and reset, and no "the chip did not reset" warning. Saves about 3 s on a good boot. Compiled against `out-target`; needs a warm reboot and a cold boot on the phone. | S | Confirmed; change untested |
| 1.1a | **Hidden Reboot button after a hub failure (fixed).** The full-screen warning's flag stayed set when the shell continued by itself (`boot_check_sensorhub`), so its unseen buttons took every touch, and one went to TWRP. Fixed in `cb60176`, and `make check-reload-hub` covers it. A real failed hub hasn't exercised it on the phone yet. | S | Confirmed; fixed |
| 1.1b | **Make the panel self-mask-off reliable.** Black lines appeared in rows 0-149 and 2810-2959, clean in screen captures. They persisted across clock updates and one resend of the mask-off commands, then cleared after a second resend followed by redraws. `ztos-panel-selfmask` sends its commands once, unchecked, without holding off frames, and packets are known to get dropped. Seen twice on this phone. | S | Likely; not proven |
| 1.2 | **Sensor hub after a full power-off (fixed).** LDO9 on the S2MPB02 was off, and the chip doesn't answer without it. Samsung's kernel turns it on, which is why a TWRP pass cured it. `zrn-flash` now turns it on at load. Passed one full power-off on the G960F; see `sensors/README.md`. Not run on a G960N. | L | Confirmed; fixed on this phone |
| 1.3 | **Kernel and module build (partly done).** `build/linux-7.1` and the toolchain are in place, and `scripts/build-bt-modules.sh` built the running Bluetooth, sound and other modules. On a fresh clone nothing fetches them, and a boot image hasn't been built here yet. | M | Confirmed |
| 1.4 | **Rebuild prebuilts together and enforce it.** Prebuilts date from 14 and 26 Oct, while the boot image is from 14 Oct and kernel files changed through 30 Oct. All carry the vermagic `7.1.0-zboot`, so a mismatch loads silently. Add a CI check. | M | Confirmed (dates); harm Likely |
| 1.5 | **`btmgmt` kernel crash (fixed on this phone).** Rebuilding `bluetooth.ko` against the running kernel stopped the oopses (3.0a): none in the four boots from 2026-11-06. The install still needs 4.0 and 1.4, or a new install could bring it back. | M | Confirmed; fixed on this phone |
| 1.6 | **Answer DHCP on USB.** `build.py:335` disables `systemd-networkd`, the only thing serving DHCP on `usb0`, and NetworkManager is told to ignore `usb0`. Hosts retry every 45 s and lose the connection. Check that enabling it doesn't conflict with NetworkManager on `wlan`/`eth`. Workaround in use: a static host profile (`ztos-usb`), which stopped the DHCP loop on this PC. The phone side is unchanged: `systemd-networkd` is still disabled. | S | Confirmed |
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
| 3.0a6 | **Sync the speaker's volume buttons with the phone**, and return to the other output's volume on disconnect. Bluetooth takes over when it connects, unless another Bluetooth or USB output is present. Works while audio is playing: the UE MINI BOOM reports nothing while the link is idle. Disconnect and reconnect still to confirm. | M | Mostly confirmed |
| 3.0a5 | **Bluetooth list order (fixed).** Rows keep their places while the page is open, and a tap acts on the device drawn in that row. Consider also asking before pairing with unnamed devices, since tapping one pairs at once. The "Not in range" row hasn't been seen on the phone yet. | S | Confirmed; host test and 5 scans |
| 3.0a7 | **PIN entry and pairing questions for Bluetooth.** `ztos-bt-agent` answers BlueZ through a sheet in the shell: confirm a number, type a PIN or passkey, show a PIN or passkey, allow a pairing or a service. It asks only while the phone is discoverable. Every sheet, the lock behaviour and the timeout tested on the phone against a pretend bluetoothd, and registration against the real one. No real device has paired through it yet, and the UE MINI BOOM flow is still to confirm. | M | Partly confirmed |
| 3.0a2 | Pairing started from another device: `ztos-bt-agent` now answers it (3.0a7), untried with a real device. `Pairable` starting as `no` still needs checking. Settings also says "ztOS Bluetooth Device", but the phone's name is `ztos`. | S | Confirmed (code) |
| 3.0a3 | No Bluetooth address found in EFS on this G960F, so it uses the chip's default. Check whether the F stores it elsewhere. Read EFS only. | S | Unknown |
| 3.0a1 | Bluetooth acceptance tests on the G960F: scan finds devices; the phone is discoverable while the Bluetooth page is open; a speaker pairs, connects and plays audio. AirPods Pro 2 follow in a later phase. | M | New |
| 3.0b | **Fully power down the modem.** Airplane mode now turns the radio off (`AT+CFUN=4`), but `cbd` and the modem processor keep running. Add a "modem off" state that doesn't start them, and handle `ztoscsf`, which restarts stopped units. `modem/README.md` warns a restarted modem may stop answering. | M | Confirmed (gap) |
| 3.0c | Add an option for airplane mode to also turn off Wi-Fi and Bluetooth. Off by default. | S | New |
| 3.0d | The modem reports `sim=error` and `bars=4` with no SIM fitted. Report "no SIM" and no signal. | S | Confirmed |
| 3.0e | `zrn-pushd` calls `ztos-waydroid-recover` at a path that doesn't exist on this phone. `snd_zrn_abox` also ignores `capture` and `mic_gain`, more signs of out-of-date modules (1.4). | S | Confirmed |
| 3.1 | `ztmetro` aborted twice (21:09 and 21:10 UTC) and the shell restarted. No core file was kept. Keep cores so we can get a backtrace (`coredumpctl` shows `Storage: none`). The abort at 07:46 on 2026-11-07 is explained and fixed: changing a running app's scale (7.21). Those at 00:47 and 01:22 that day are not. | M | Confirmed; cause Unknown |
| 3.2 | Three DRM warnings from `drm_atomic_helper_wait_for_vblanks`. Two came just after the shell crashes, so they may be linked. | M | Confirmed; cause Unknown |
| 3.3 | `/dev/binderfs` fails with `unknown filesystem type 'binder'`, which suggests the kernel lacks binder. Check the effect on Waydroid. | M | Confirmed; effect Likely |
| 3.4 | Read the last kernel crash log. `ztoscsf` reports the phone restarted after the kernel stopped, and `/sys/fs/pstore` needs root. | S | Confirmed |
| 3.5 | `zrn-cpuidle` won't load because a mark file says the last load never finished. Decide whether a fresh install should have that file. | S | Confirmed |
| 3.6 | `ztos-update-check` and `ztstore-install@samuel` fail. Possibly from 1.7; needs checking. | S | Confirmed; cause Unknown |
| 3.7 | Wi-Fi P2P interface creation times out, with other `brcmf` `-52` errors. May share a cause with 1.7. | M | Confirmed; cause Unknown |
| 3.8 | `zrn-bluetooth-addr` fails with `hci0 not registered`, though `hci0` exists now. Probably a boot-order issue or a result of 1.5. | S | Confirmed; cause Likely |
| 3.9 | Test real sleep on the F. `zrn-sleepd` reports `switched off` here, so it isn't sleeping. Find out why, and whether that's intended. | L | Confirmed; cause Unknown |
| 3.10 | `ztmetro` sent `reboot_recovery` to `ztoscsf`, which ignores it on purpose (`sensors/README.md`), then raced three reboot commands, one of them a plain reboot. Now the shell does it, as one ordered command, only when a person taps the button; `ztoscsf` is unchanged. Nothing sends `dialog_sensorhub` today, so only the boot warning reaches this. Built; the button not pressed on the phone. Both hub warnings still tell people to go through TWRP, which the LDO9 fix (1.2) should have made unnecessary: wording left for the owner. | S | Review; fixed |
| 3.11 | `zrn-android-root` had unchecked mounts, so it could report success with a broken root. Now any refused mount prints what failed, takes the half-made root down and exits 1, which its three callers already check. The apex bind is skipped only when SYSTEM has no such directory, as on an Android 9 layout. `sensors/test-android-root.py` (mocks); runs at the next boot on the phone. | S | Review; fixed, host test only |
| 3.12 | `ztoscsf` runs the hub check on devices without that hub, and its retries block the main loop. Fixed in the source: no check where `ztoscsf-sensorhub.service` is not installed, and a recovery now runs a step a second, so the control pipe and other checks keep going. `make check-csf`; not yet on the phone. | S | Review; fixed, host test only |
| 3.13 | `/run/ztoscsf.ctl` was mode 0666, so any local user could stop the hub. Now 0620, owned by root and the group of the phone's user (`/etc/ztos/user.conf`). Host test only; not yet on the phone. `/run/ztoscorenetworking.ctl`, `/run/zrnperfd.ctl` and `/run/ztoscsf-sensord.ctl` are still 0666 and have the same weakness. | S | Review; fixed, host test only |

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
| 6.1 | **Live screen and touch control, same on phone and QEMU.** A `ztmirror` program on the device streams the screen and takes input over a direct TCP socket (USB-NCM in development). `scripts/ztctl.py` already does still captures and multi-touch over SSH, as a stopgap and for scripted tests. Built: `ztmirror`, `ztos mirror` and `ztos touch`; the window client still needs a try. Design notes below. | L | New |
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

## 7. New features

| # | Item | Size | Starting point |
|---|---|---|---|
| 7.1 | **Bluetooth device battery in the status bar**, next to the Bluetooth icon, when the connected device reports it through the standard Bluetooth battery mechanism. | S | Built and installed: one thin bar beside the icon. |
| 7.2 | **Audio tile in the pull-down menu.** Label is the output type: Phone Speaker, A2DP, 3.5mm AUX (TRS), 3.5mm Headset (TRRS, with mic and play/pause), USB AUX, USB Headset (Apple adapter to TRS or TRRS). The current device name goes below it. Tapping opens a modal to switch device and set that device's volume. Stage 1 built: the tile, modal, speaker and A2DP. | L | Phone speaker and A2DP work. The jack needs codec work (`audio/README.md` lists headphones under the CS47L93). USB audio needs `SND_USB`, which `audio/build.sh` disables, and USB host mode. Needs 3.0a6 for per-device volume. |
| 7.3 | **Investigate force press.** A harder press near the bottom centre acts as the button there, with a short vibration, even when the button is hidden. | M, research | No support found in the repo. First find out how the stock kernel reads the pressure sensor (`stock/BOOT.img`), and whether the mainline touch driver can reach it. |
| 7.4 | **Investigate Always-On Display.** White on black at low brightness, with elements shifted every couple of hours to avoid burn-in. May be its own app. | L, research | No AOD found in the shell or Settings. Needs the panel's low-power mode without full sleep, and a way to update it while the SoC sleeps. Related: panel self mask (1.1b), sleep (3.9). |
| 7.5 | **Edit quick tiles in place.** A pencil button next to ALL SETTINGS reveals an unused-tiles section. Drag to rearrange, or drag into unused to hide. Any number of tiles, from none upward; the panel scrolls or grows to fit. Replaces the Settings page. | M | Tiles are a `quick` config list capped at eight (`PAGE_QUICK`, `QUICK_IDS`), so that cap has to go. |
| 7.6 | **Per-application audio.** Volume and output device per app through PipeWire, with Default following the global output. Must persist across shell restarts, app restarts and reboots. | L | `ztos-soundd` and `sound/ztsound.c` handle output-wide sound profiles. Check whether WirePlumber's stream restore can do the persistence. Shares device-switching with 7.2. |
| 7.7 | **Reorganize Settings into a tree.** Today it is flat under "system" and "applications". Sections such as Connections (Wi-Fi, Bluetooth, ...) and Customization, with sub-pages. Starts as a written plan to approve. | M, plan first | Built, merged and installed 2026-11-07, with `make check-settings-tree` and `check-settings-config` passing. Deep links such as `ztsettings bluetooth` still work. |
| 7.8 | **Wallpaper switcher on the home screen.** Hold a blank area of the tiles screen to enter it. Swipe left/right between the wallpapers added so far. Buttons: settings (jumps to the Tiles Shell page under Customization), add (built-in images now, gallery once 7.9 exists; the choice joins the swipe list), edit (zoom/pan the current image), remove. | L | Settings has a wallpaper picker (`PAGE_WALLS`) with built-in categories and a single `start_wallpaper` value. Needs a saved list of wallpapers with crop data, and a hold gesture that doesn't conflict with tile hold or scrolling. |
| 7.9 | **Photo gallery app.** Also the source for 7.8's "from gallery" option, which waits for this. | L | Built, merged and installed 2026-11-07 (`ztgallery`; picker `ztgallery --pick`). See `ztmetro/gallery/README.md`. |
| 7.10 | **Customization section of Settings** (part of 7.7). Customization: animation speeds (in a submenu), accent color (now under theme), and shell style (warn before applying, since it restarts the whole shell). **Tiles Shell** submenu under Customization: number of start screen columns, and a background shortcut that jumps to the home screen with the 7.8 switcher open. Lock screen settings are in 7.12. | M | These settings already exist (`menu_speed_idx`, accent, `columns`, `style`), so this is mostly moving them. Start and lock wallpapers are already separate keys. |
| 7.12 | **Lock Screen submenu under Customization**, with its own wallpaper picker. As on the tiles screen, chosen wallpapers join a user list before use, and the two lists are separate. No limit on how many wallpapers either list holds. | M | Lock wallpaper is a single `lock_wallpaper` value today. Should share the list storage and picker with 7.8. |
| 7.13 | **Automatic wallpaper rotation.** A submenu inside both Tiles Shell and Lock Screen to rotate through that list at a chosen interval. | M | Depends on 7.8 and 7.12. Must not wake a sleeping phone just to change wallpaper: apply at the next wake. |
| 7.11 | **Preview for the background and shell style overhaul.** Show a preview demo of how the shell will look, then require a second confirmation to apply. Replaces the plain warning and direct shortcut in 7.10 later. | M | Depends on 7.10. `ztsettings --render` may help with previews. |
| 7.13a | **Home opens the task switcher** when tapped on the tiles screen at rest. From the app list, edit mode or a scrolled list, it still returns to the top of the tiles first. | S | Built. With no apps open, nothing happens. DotMatrix style unchanged. |
| 7.13b | **Dismiss notifications by swiping** one left or right off the pull-down menu. | S-M | CLEAR ALL exists; per-notification swipe doesn't. Schedule with the other pull-down work. |
| 7.13c | **Remove the Rotation Lock quick tile for now**, until rotation exists. | S | Nothing is behind it. Starts after the audio tile merges. |
| 7.13d | **Working flashlight tile, with brightness.** | M | Built: tile on/off, level from its modal (7.13e). |
| 7.13e | **Long-hold modals on quick tiles**, in the style of the audio selector: flashlight brightness slider with a percentage, screen brightness, others to be listed. | M | Flashlight and brightness built: sliders can have visible detents (flashlight: off plus 9 levels), and brightness has the auto toggle. Other tiles still to do. |
| 7.13f | **Audio tile tap option.** A setting for whether a single tap opens the modal or just switches the default device. Goes in the quick tile settings (7.5). | S | Depends on 7.2 and 7.5. |
| 7.13g | **Numeric on-screen keyboard**, chosen automatically for numeric fields and Bluetooth PINs. Browser, PWA and Android fields where the app tells the shell what the field is. | M | Built, merged and installed 2026-11-07. Apps must say what the field is; `ztmetro/KEYBOARD.md` lists what each kind sends. |
| 7.13h | **Dots and trails for network and agent input**, so a person holding the phone can see it: a mostly opaque circle per finger, a thin trail of its last second of travel, both gone 750 ms after it lifts. | S | Built. |
| 7.14 | **AirPods support, in its own app for now.** Battery levels (left, right, case), listening modes and other radio options, toggle switches and name editing, similar to LibrePods and LinuxPods. **Battery must show even when not connected, from BLE broadcasts.** Support features across AirPods generations, including how each reports sensor and battery data. Testing is on AirPods Pro 2. Spatial audio is out of scope. A button on the device's row in the Bluetooth page opens these settings once AirPods have connected. | L | Research running. Depends on 3.0a. The chip-default Bluetooth address (3.0a3) may matter for pairing. |
| 7.14a | **AirPods status bar icon**, separate from the plain Bluetooth meter (7.1), which leaves out Apple devices. Fed by the AirPods protocol, not BlueZ's battery. | S | Depends on 7.14. |
| 7.15 | **AirPods "High quality two-way audio".** Use the microphone without dropping to the low-quality headset profile, as LibrePods lists it. | L, research | Feasibility unknown until research reports. |
| 7.16 | **Move the AirPods UI into a modal** reached from the same area as the audio sources modal (7.2). | M | Depends on 7.2 and 7.14. |
| 7.17 | **Notification LED settings page.** Configure the front LED (`red/green/blue:status`, driver `leds/zrn-led.ko`): on/off and colour for charging, full and low battery (now fixed in `zrn-charge`), notifications, and whether the RapidBoost indicator runs (`zrnperfd` blinks it white at the top boost rung, which looks like a fault if you don't know). The two writers need one owner so they stop overwriting each other. | M | TODO |
| 7.18 | **Auto-rotate.** Turn the screen to follow the phone, so fullscreen video can play in landscape (today it stays portrait and letterboxed). Needs an orientation source from the accelerometer, shell layout and touch mapping for each rotation, and apps resized to match. Possible first step: rotate only fullscreen apps, leaving the tiles and lock screen portrait. Bring back the Rotation Lock tile (see 7.13c), with a Settings switch. | L | TODO. The shell has no rotation today. Accelerometer data comes through the sensor hub. |
| 7.19 | **Fingerprint sensor: unlock, and a swipe to pull down the action centre**, as Android does. | L | Research under way: can Samsung's secure-world fingerprint code be used as a black box from Linux? Not started until that answers. |
| 7.20 | **Apps can keep the screen on (idle-inhibit).** A playing video asks the screen to stay awake through Wayland's idle-inhibit protocol, for the app in front only and not behind the lock screen. | S | Built. Tested on the phone: Grayjay keeps the screen on while playing, and it sleeps once paused. |
| 7.21 | **Per-app scaling.** Settings > Applications > app scaling sets each app to 75-150%, kept in `~/.config/ztmetro/app-scale`. The app gets a window sized to match and renders at the screen's pixels. Android runtimes keep 100%. | M | Tried on Brave: changing the scale of a running app aborted the shell (2026-11-07). Fixed in the source, with a test (`make check-reload-hub`); not yet installed. |
| 7.22 | **Better notifications.** Swipe away or dismiss one notification (not just Clear All), show live progress bars, and show app download progress, starting with Grayjay. | M | TODO. `ztnotifyd` already reads a progress hint. Grayjay's own exposure is being checked. |
| 7.23 | **Hardware video decoding.** `media/` loads mainline `s5p-mfc` as modules with no new boot image. Install it by default, make `mpv` use it, and fix Superscan's error 34 stall. Brave and Grayjay's CEF only support VA-API, so they can't use it without a shim or rebuild. | L | Works on this phone with its own firmware: H.264 2536x1080 confirmed in `mpv` and FFmpeg. Not persistent across reboots yet. |

### Building these in parallel

Items that edit the same code can't run in parallel without conflicts. Two files are the bottleneck: `ztmetro/ztmetro.c` (status bar, pull-down menu) and `settings/ztsettings.c`.

| Track | Items | Main files | Notes |
|---|---|---|---|
| A: research only | 7.3, 7.4 | Read only | Can run at any time. Nothing to merge. |
| B: audio backend | 3.0a6, then 7.6 | `ztos-soundd`, WirePlumber config | Mostly outside the shell. Settles the API that 7.2 uses. |
| C: pull-down menu | 7.1, then 7.5, then 7.2 UI | `ztmetro.c` | One after another: all three change the same drawing and touch code. 7.2 UI waits for B. |
| D: Settings | 7.7, 7.10 | `ztsettings.c` | Plan first. Hold other Settings changes while pages move. 7.5 removes `PAGE_QUICK`, so agree the order with C. |
| G: AirPods | 7.14, 7.15, then 7.16 | New app and daemon | Mostly independent. The Bluetooth page button touches `ztsettings.c`, and battery may feed 7.1. |
| F: wallpapers and gallery | 7.8, 7.9, 7.12, 7.13 | `ztmetro.c`, new app | The gallery app is independent. The 7.8 gesture touches the home screen's touch code, so schedule it around C. |
| E: audio hardware | 3.5mm and USB output detection for 7.2 | Audio driver, kernel config | Independent. Rebuilt with `scripts/build-bt-modules.sh`. The kernel side may need a new boot image, which is now allowed; see below. |

A, B, C, D and E can start together, each in its own worktree. Only one track can test on the phone at a time, so phone testing has to take turns.

### Decisions (2026-11-06)

- **Settings tree (7.7):** plain "all settings" root with no tabs. Remove the Battery Saver switch, which does nothing, keeping the real battery info. Lock-on-wake and slow motion leave the developer page only if they work. Developer stays visible under system. No one-tap "add to the other wallpaper list" for now.
- **A new boot image is allowed** when a change needs one, because USB has been stable since the port was cleaned and the DHCP loop fixed. Check the partition table on the G960F and confirm `stock/` backups before any flash (2.6).
- **Quick tiles:** any number, including none (7.5).
