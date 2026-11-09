# Waydroid on ztOS

Android apps run in Waydroid, a LineageOS 20 (Android 13) container, as an
**optional application**: the `ztos-waydroid` package. The base system has no
Waydroid, no Waydroid services and no Waydroid noise. Installing the package
adds everything; removing it takes everything away again. ATL (the Android
translation layer) stays the base system's way to run Android apps.

The Android side (images, patches, freeze policy, microG, audio, keys,
location, push) was built and measured on an earlier S9 install. It is
documented in `android/waydroid/README.md`, `PHASE2.md`, `AUDIO.md`,
`RAM-BOOT.md`, `MICROG-TASK.md` and `android/push/README.md`. This page
covers packaging, the kernel, what to test, and the Bluetooth design.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Kernel options | `kernel/waydroid.config`, `scripts/build-boot.sh --waydroid` | binder, binderfs, PSI, bridge, veth, nftables NAT, policy routing, netd's xtables |
| Package | `packages/ztos-waydroid/` (PKGBUILD, install scriptlets, sudoers template, container drop-in) | depends on `waydroid lxc python-gbinder nftables dnsmasq ...` |
| Helpers the shell calls | `android/waydroid/power.sh` → `/usr/local/bin/ztos-waydroid-power`; `recover.sh` → `/usr/local/libexec/ztos-waydroid-recover` | paths fixed by ztmetro.c, zrn-pushd, ztos-android |
| Launcher | `launch.sh` → `/usr/local/bin/ztos-waydroid-launch` | used by `ztos-android launch` |
| Services | `ztos-waydroid-images` (system, first set-up), `ztos-waydroid-network` (system), `ztos-waydroid-session`, `-boot`, `-audio-reconnect` (user) | all shipped in `/usr/lib/systemd`, none in `/etc` |
| Image recipe | `/usr/share/ztos-waydroid/` (`install.sh`, `strip-image.sh`, `patch-*.py`, `waydroid.cfg`, microG scripts) | `PACKAGED` there tells `install.sh` the package owns the files |
| Base, outside the package | `zrn-pushd` and `zrn-pushd-waydroid` (push for ATL and Waydroid apps), the GNSS bridge `ztos-waydroid-location` (device layer), ztMETRO's Android window handling, `ztos-android` | each one checks whether Waydroid is set up |

"Set up" means `/var/lib/waydroid/waydroid.cfg` exists, which only
`waydroid init` writes. Everything that would start or freeze Android checks
that file (and, where it calls the root helper, that the helper is installed).

## Kernel

Without `--waydroid` the boot image has no binder (`dev-binderfs.mount`
failed on every boot, ROADMAP 3.3), no nftables, no NAT, no bridge, and
`VETH`, `NF_CONNTRACK`, `IP_NF_IPTABLES` and `OVERLAY_FS` only as `=m`. The
image builds no modules, so those `=m` options are missing on the phone too.

`kernel/waydroid.config` enables everything built in (`=y`):

- `ANDROID_BINDER_IPC`, `ANDROID_BINDERFS`, `ANDROID_BINDER_DEVICES=""`.
  Waydroid mounts `/dev/binderfs` itself and allocates its binder, hwbinder and
  vndbinder there. No static devices.
- `COMPAT`, for 32-bit processes. The vendor image's audio HAL service is
  32-bit ARM only, and so are media.omx, cas, the external camera provider
  and system's mediaserver. Without `COMPAT` they fail with "Exec format
  error", audioserver waits forever for `IDevicesFactory@4.0`, and Android
  never finishes booting (chg9). This adds 192 KiB to the image. After the
  flash, check that `dmesg | grep -i '32-bit EL0'` shows support on all CPUs
  (Exynos M3 and Cortex-A55).
- `PSI`, for Android's lmkd (`ro.lmk.use_psi`) and ztos-memory-policy. It is
  also listed in ztOSIAL's kernel contract.
- `VETH`, `BRIDGE`. The container's eth0 is attached to the `waydroid0` bridge.
- nftables NAT for `waydroid-net.sh` (`NF_TABLES`, `NF_TABLES_INET/IPV4/IPV6`,
  `NFT_NAT`, `NFT_MASQ`, `NFT_CT`, `NFT_REJECT`, `NFT_COMPAT`, `NFT_FIB_*`,
  `NF_NAT`, `NF_CONNTRACK`).
- Policy routing (`IP_ADVANCED_ROUTER`, `IP_MULTIPLE_TABLES`,
  `IPV6_MULTIPLE_TABLES`). Without it Android's netd cannot add its `ip rule`s,
  and `ztos-waydroid-network` has to add a default route by hand.
- Legacy iptables (`NETFILTER_XTABLES_LEGACY`, `IP(6)_NF_IPTABLES_LEGACY`,
  filter, mangle, raw, nat), plus the xt matches and targets Android 13's netd
  sets up: owner, mark, connmark, bpf, socket, quota, idletimer, nflog and
  others. Android uses iptables-legacy, not iptables-nft.
- `INET_DIAG_DESTROY`, so netd can close the sockets of a lost network.

`build-boot.sh --waydroid` applies the file after the base config, runs
`olddefconfig`, then **fails** if any `=y` option of the file did not stick
(an option whose dependency is off is otherwise dropped silently).

Cost, measured on 8 Nov 2026 by compile-testing the running image's config
(`out/ztos-chg6.config`) with the file applied, using the same toolchain and
`-j4`: the build was clean, with no new warnings.

- `Image`: 23,310,344 → 24,127,496 bytes, **+798 KiB**.
- vmlinux text+data: +814 KB.
- bss: +26 KB.

That is under 1 MB of RAM for the kernel's own copy. There is no runtime effect while it is unused:
binder exists only once binderfs is mounted, netfilter hooks and conntrack
register only when a table or rule asks for them, and the bridge exists only
once Waydroid creates `waydroid0`. PSI is the exception. It adds a small cost
to every task state change (a few ns per context switch). If that matters,
`psi=0` on the command line turns it off, or `PSI_DEFAULT_DISABLED` with
`psi=1` makes it opt-in.

Recommendation: keep it a flag, `--waydroid`, but pass it on the images the
phones run. It is cheap and harmless when unused, and a flag keeps
"Waydroid-ready kernel" an explicit choice for other devices (ztConflo).

### Build

The coordinator builds, in the container, exactly the current image's
options plus the flag:

```sh
scripts/build-boot.sh <name> <the flags used for ztos-chg6> --waydroid
```

The build prints `waydroid: kernel/waydroid.config applied` after the config
step. A `--waydroid: not enabled: ...` error means an option did not survive;
nothing was built in that case.

## Installing and removing

On the phone, from a checkout (or a copy of `android/waydroid`,
`packages/ztos-waydroid` and `docs/waydroid.md` in the same layout):

```sh
cd packages/ztos-waydroid && makepkg -f && sudo pacman -U ztos-waydroid-*.pkg.tar.*
```

`post_install` does the following:

1. Renders the sudoers rule for the phone's user (`ztos-user-dropins`).
2. Enables the two global user units.
3. Enables and starts `ztos-waydroid-images` in the background.

That unit fetches the pinned images (about 1 GB of zips, 2.4 GB unpacked)
from Waydroid's OTA channel and checks their SHA-256. **The download does
not need binder**: on a kernel without it, the images are put in place and
the unit stops there. On a kernel with binder, it continues:

1. `waydroid init`.
2. `install.sh --strip`: strip the image, add the 64-bit zygote and the
   wake-alarm init patch, patch the vendor composer, merge `waydroid.cfg`,
   then `waydroid upgrade -o`.
3. Enable the container manager and `ztos-waydroid-network`.
4. Write `/var/lib/ztos/waydroid-ready`.

The unit is skipped from then on. `journalctl -u ztos-waydroid-images` shows
its progress.

microG (zrn-pushd's device identity) is a separate step after Android's
first boot: `android/waydroid/prepare-microg.py` and `install-microg.sh`,
from `MICROG-TASK.md`. Until it is done, zrn-pushd logs
`microG's identity: not available` and retries every 10 minutes. That is
correct while Waydroid is set up but microG is not.

`sudo pacman -R ztos-waydroid` (or `-Rns` to also drop waydroid, lxc and
python-gbinder) does the following:

1. Stops the Android session and the container.
2. Disables every unit it enabled, including `waydroid-container.service`.
3. Deletes the `ip ztos_waydroid` nftables table and waydroid-net's bridge
   and tables.
4. Unmounts binderfs.
5. Removes the sudoers rule, the markers, `/usr/share/waydroid-extra/images`,
   `/var/lib/waydroid` and the download cache.
6. Removes the Waydroid `.desktop` entries in the user's home, so no tile is
   left that cannot open.

The user's Android data (`~/.local/share/waydroid`: installed apps and their
files) is **kept**. The scriptlet prints its size and the command to delete
it.

Image choice: LineageOS 20 **VANILLA** system with the **MAINLINE** vendor
(Mesa), pinned to the 20260927 build that the patches were verified on. The
scripts refuse an unknown binary rather than patch it. GAPPS is not used:
Google Play Services would fight microG and zrn-pushd for the push
connection, costs roughly 300 MB more RAM, and fails Play certification on
Waydroid anyway.

## Runtime

- **GPU**: Panfrost through Mesa in the MAINLINE vendor
  (`ro.hardware.gralloc=gbm`, `ro.hardware.egl=mesa`), GLES only, Vulkan off,
  boot animation off (CMA pressure). The fallback, if Panfrost misbehaves in
  the container, is software rendering: `ro.hardware.gralloc=default`,
  `ro.hardware.egl=swiftshader` in `waydroid.cfg`, then `waydroid upgrade -o`.
  It is slow but stable.
- **Audio**: through PipeWire's Pulse server. The latency rules
  (`90-ztos-waydroid-latency.conf`) are in the base overlay, and they match
  nothing when Waydroid is absent. `ztos-waydroid-audio-reconnect`
  reconnects Android once after PipeWire restarts.
- **Input**: Wayland touch and keys from ztMETRO. The host keyboard is used
  for Android's IME (`KEYS.md`).
- **Network**: `waydroid-net.sh` (bridge, dnsmasq, nftables masquerade).
  `ztos-waydroid-network` keeps forwarding on and repairs its NAT table. On a
  kernel without policy routing, it also adds Android's default route.
- **Shell**: Waydroid's `waydroid.*.desktop` files are synced into ztOS's app
  list by `ztos-android sync` (`network.py` runs it). Apps launch as
  `waydroid.PACKAGE` windows (multi-window). ztMETRO calls
  `ztos-waydroid-power active|idle|screen-off|sync` and
  `sudo ztos-waydroid-recover settings`.

## When the package is not installed

| Caller | Before | Now |
|---|---|---|
| zrn-pushd identity | `sudo ztos-waydroid-recover microg` → "command not found", then "not available ... asking again" every 10 min | said once, no timer; the next Google login checks again (`waydroid_set_up()`) |
| zrn-pushd delivery | each message for a Waydroid app: sudo fails, the message is requeued, then `cannot start ztos-waydroid-power` | said once; the message is dropped unacknowledged (Google sends it again at a later login) |
| ztMETRO screen-off | `system("/usr/local/bin/ztos-waydroid-power screen-off")` → 127 → "Waydroid did not confirm screen-off freeze" each time | **needs the shell change below** |
| ztMETRO volume / ringer | `sudo -n ztos-waydroid-recover settings` on every volume change (sudo logs "command not found") | **needs the shell change below** |
| ztMETRO `run_detached("ztos-waydroid-power", "sync")` | exec fails in the child, silently | unchanged (harmless) |
| `dev-binderfs.mount` | failed on every boot (base fstab) | gone from the fstab; Waydroid mounts it |
| `ztos-waydroid-location` | polled `pgrep zygote64` every 15 s, forever | not started (`ConditionPathExists`) |
| `waydroid-container`, `ztos-waydroid-images` | enabled in the base image (`build.py`) | only with the package |
| installed but not set up (no binder) | | `power.sh`, `recover.sh` and `ztos-waydroid-boot` return at once: success for freeze, display and settings; "not up" (3) for anything that would start Android |

The ztMETRO change. This is not done here, because the shell is shared. It is
the smallest change that keeps the shell quiet when the package is absent and
picks the package up without restarting:

```c
/* the optional ztos-waydroid package (docs/waydroid.md): one stat */
static bool waydroid_installed(void) {
	return access("/usr/local/bin/ztos-waydroid-power", X_OK) == 0;
}
/* screen-off, in the screen on/off handler: */
	if (!on && !demoloop && waydroid_installed()) {	/* was: if (!on && !demoloop) */
/* waydroid_ringer(): */
	if (!waydroid_installed()) return;
```

With the package installed but not set up, `ztos-waydroid-power screen-off`
now exits 0, so no warning appears in that case either.

## Test plan after the new kernel

1. `grep binder /proc/filesystems`. Also check that `zcat /proc/config.gz`
   (or the image's `.config`) shows `PSI`, `NF_TABLES` and `BRIDGE` as `=y`,
   and that `systemctl --failed` no longer lists `dev-binderfs.mount`.
2. Base without the package: no `waydroid` lines in `journalctl -b`, and no
   "did not confirm" warning after a screen-off (once the shell change is in).
3. With the package: `journalctl -fu ztos-waydroid-images` shows init and
   strip completing, and `/var/lib/ztos/waydroid-ready` exists. Then run
   `ztos-waydroid-launch org.lineageos.jelly` (or Settings,
   `com.android.settings`).
4. Network: inside Android, `curl generate_204` returns 204. `ip rule`
   inside the container works (policy routing), and `nft list ruleset` on the
   host shows waydroid-net's tables.
5. Graphics: `dumpsys SurfaceFlinger | grep GLES` shows Mesa Panfrost, and an
   app scrolls without corruption.
6. Freeze: close every Android window, then after about 20 s
   `lxc-info -sH` shows FROZEN. Screen off also gives FROZEN, and the shell
   logs "Waydroid screen-off policy completed".
7. Audio: a video plays through PipeWire.
8. The app list: an APK installed with `ztos-android install X.apk --runtime
   waydroid` appears as a tile, opens, and closes from recents.
9. microG, then zrn-pushd: `identity` is known, and a test push to an app
   arrives.
10. Removal: `pacman -R ztos-waydroid`. Then nothing in `systemctl list-units
    '*waydroid*'`, no `/etc/sudoers.d/ztos-waydroid`, no `nft list tables`
    entries for it, no `waydroid0` link, and the images are gone.

## Risks

- **Memory**: Android takes about 1.5 to 1.7 GB of the phone's 3.5 GB
  (README, measurements), so with the shell running, about 1.7 GB is
  available. The boot service starts Android at login and then freezes it.
  A user who seldom runs Android apps could disable
  `ztos-waydroid-boot.service` and start Android lazily on first launch
  instead (about 35 s).
- **Battery**: a frozen container uses no CPU (`cpu.stat` unchanged across
  frozen samples). The costs are `keep_awake` apps, push thaws, and
  `ztos-waydroid-network`'s 5 s poll (it skips Android while frozen).
- **Security**: the container runs as root with lxc's default confinement and
  no SELinux. The sudo rules allow only the fixed helper actions. APK paths
  and package names are validated. The build fingerprint is spoofed to a
  Samsung release build (`waydroid.cfg`) so that WhatsApp works. Binder and
  netfilter add kernel attack surface, also while unused.

## Bluetooth for Android apps (design)

**Goal**: Android IoT set-up apps work in Waydroid. That means BLE central
(scan, connect, GATT client), BLE peripheral (advertise, GATT server) and
Classic with Secure Simple Pairing. The user tests with nRF Connect.

**Decided**: Linux owns Bluetooth. BlueZ keeps the controller and every bond
(`/var/lib/bluetooth`). Android is a client of BlueZ through a shim, and it
never holds link keys as the source of truth.

### What the hardware and BlueZ offer (read on the phone, 8 Nov)

BCM4361 at HCI version 9 (Bluetooth 5.0), manufacturer 15 (Broadcom).
`btmgmt info` supports `ssp br/edr le advertising secure-conn privacy
ll-privacy phy-configuration`. BlueZ 5.87 exposes `GattManager1` and
`LEAdvertisingManager1` on `hci0`, with 5 advertising instances and the
`tx-power`, `appearance` and `local-name` includes. So peripheral mode
(advertising and GATT server) is possible through BlueZ's D-Bus API, and so
is central mode and Classic.

### Options considered

- **(a) Give Android the controller** (HCI user channel and AOSP's btlinux
  HAL). Rejected, for two reasons. BlueZ loses `hci0` while Android has it,
  which disconnects the AirPods. And Android would hold its own bonds.
  There is also a technical block: `AF_BLUETOOTH` sockets exist only in the
  host's network namespace (`bt_sock_create` refuses others), so the HAL
  would need a relay from the host anyway.
- **(b) A shim from Android's stack to BlueZ.** Chosen. No existing
  implementation was found. Waydroid has none: issue
  [#1378](https://github.com/waydroid/waydroid/issues/1378) only asks to fake
  the "enabled" state, and Sailfish users report Bluetooth off inside
  Waydroid. BlueZ's old `android/` directory was a HAL that BlueZ itself
  implemented. It targeted the Android 4.4 and 5 HAL, and ran its own daemon
  on the controller rather than sharing the host's.

### Where the shim sits

Android 13's Bluetooth is split into three layers:

1. Framework (`BluetoothAdapter`, `BluetoothLeScanner`, `BluetoothGatt`,
   `BluetoothGattServer`, `BluetoothLeAdvertiser`).
2. Binder (system_server's `BluetoothManagerService`, then the Bluetooth
   app's `AdapterService`, `GattService` and `ScanManager`).
3. JNI, into the native stack `libbluetooth.so` (Fluoride/GD), through its C
   and C++ interfaces: `bt_interface_t`, `btgatt_interface_t` (client and
   server), `BleScannerInterface`, `BleAdvertiserInterface` and
   `btsock_interface_t`. Below that is the HCI HAL.

The shim **replaces `libbluetooth.so`** in the system image. It implements
those interfaces and keeps the Java Bluetooth app, permissions, scan filters
and callbacks unchanged, so apps see stock Android behaviour.

Below the interfaces, it speaks a small framed protocol over a Unix socket.
The socket is bind-mounted into the container the way the location bridge
reaches the host. On the other end is a host daemon, **`ztos-btbridge`**, a
user of BlueZ's D-Bus API. It runs on the host (BlueZ and `AF_BLUETOOTH` are
there), as part of the ztos-waydroid package. It is not a stack: BlueZ does
SMP, SSP, encryption and key storage.

Rejected alternatives at other layers:

- **The HCI HAL.** That would mean emulating a controller over BlueZ's API
  while BlueZ also drives the real one. It can't honour BlueZ's ownership.
- **Replacing the Java Bluetooth app.** That would mean reimplementing about
  ten AIDL interfaces, and it breaks with every Android release.

### Mapping

| Android call (framework → native interface) | BlueZ |
|---|---|
| `BluetoothAdapter.enable/disable`, state | Adapter1 `Powered`. Android's "off" only stops the shim's use; it never powers BlueZ's adapter off |
| `getBondedDevices`, `getBondState` | Device1 objects with `Paired`/`Bonded=true` (InterfacesAdded/PropertiesChanged → `bond_state_changed_cb`) |
| `BluetoothLeScanner.startScan(filters, settings)` → `BleScannerInterface::Scan` / `ScanFilter*` | Adapter1 `SetDiscoveryFilter({Transport:le, RSSI, UUIDs, DuplicateData})` + `StartDiscovery`; each Device1 `ManufacturerData`/`ServiceData`/`RSSI`/`AdvertisingFlags` change → `scan_result_cb` (the shim rebuilds the AD structure; BlueZ ≥ 5.70 `AdvertisingData` property gives the raw bytes where present) |
| `startDiscovery` (Classic) → `bt_interface_t::start_discovery` | Adapter1 `SetDiscoveryFilter({Transport:auto})` + `StartDiscovery` → `device_found_cb` |
| `BluetoothDevice.connectGatt` → `btgatt_client_interface_t::connect` | Device1 `Connect()` (LE) and wait for `ServicesResolved`; the shim's bridge holds the connection while the app has its client open |
| `discoverServices` → `search_service` / `get_gatt_db` | the GattService1 / GattCharacteristic1 / GattDescriptor1 tree under the device (handles from their `Handle` property) |
| `readCharacteristic`, `readDescriptor` | GattCharacteristic1/GattDescriptor1 `ReadValue({offset})` |
| `writeCharacteristic` (with/without response, signed) | `WriteValue(value, {type: request/command/reliable})` (`AcquireWrite` for high-rate write-without-response) |
| `setCharacteristicNotification` + CCCD write → `register_for_notification` | `StartNotify()`/`StopNotify()`, or `AcquireNotify()` for an fd; `PropertiesChanged(Value)` → `notify_cb` |
| `requestMtu` → `configure_mtu` | read-only: BlueZ negotiates the MTU itself; the shim reports `MTU` from GattCharacteristic1 (BlueZ ≥ 5.62) |
| `readRemoteRssi`, `requestConnectionPriority`, `setPreferredPhy` | Device1 `RSSI`; no-op with success; `Adapter1`/mgmt PHY configuration, read-only at first |
| `BluetoothDevice.createBond` → `bt_interface_t::create_bond` | Device1 `Pair()`; **BlueZ's agent** (ztOS's `ztos-bt-agent`, the shell's sheet) asks the user; the result → `bond_state_changed_cb`. The Android pairing dialog is never shown: the shim never calls `ssp_request_cb`/`pin_request_cb` |
| encrypted characteristic (insufficient authentication) | BlueZ itself pairs on the ATT error (its agent again), retries, and returns the value; Android only sees a slower read |
| `removeBond` | refused for bonds BlueZ made outside Android; allowed only for a bond an Android app created, and only after the shell's confirmation (Device1 `RemoveDevice` is never called silently) |
| `BluetoothLeAdvertiser.startAdvertisingSet` → `BleAdvertiserInterface::StartAdvertisingSet` | LEAdvertisingManager1 `RegisterAdvertisement` of an `org.bluez.LEAdvertisement1` object exported by `ztos-btbridge` (Type, ServiceUUIDs, ManufacturerData, ServiceData, LocalName, Includes `tx-power`, MinInterval/MaxInterval, `SecondaryChannel` for extended); 5 instances shared with Linux |
| `openGattServer`, `addService` → `btgatt_server_interface_t::add_service` | GattManager1 `RegisterApplication` of an ObjectManager tree (GattService1/Characteristic1/Descriptor1) that `ztos-btbridge` exports per Android server; `ReadValue`/`WriteValue` calls from BlueZ → `request_read_*_cb`/`request_write_*_cb`, Android's `sendResponse` → the D-Bus reply; `notifyCharacteristicChanged` → `PropertiesChanged(Value)` or the `AcquireNotify` fd |
| `listenUsingRfcommWithServiceRecord`, `createRfcommSocketToServiceRecord` → `btsock_interface_t::listen/connect` | ProfileManager1 `RegisterProfile(uuid, {Role, Channel})`; `Profile1.NewConnection(fd)` → the socket fd passed into Android over the Unix socket (SCM_RIGHTS), which is what `btsock` returns to Java anyway; outgoing: Device1 `ConnectProfile(uuid)` |
| `BluetoothLeScanner` with `PendingIntent`, batch scan, `BluetoothHeadset`/A2DP profiles | not offered (A2DP/HFP stay Linux's: audio goes through PipeWire anyway) |

### Policy (`ztos-btbridge`, on the host)

The policy lives in the host daemon, so Android can't get around it.

- **Hidden while Linux uses it.** A device with `Connected=true` that the
  bridge did not connect for Android (the AirPods, the Fold6) is left out of
  scan and discovery results and the bonded list. Android's `connect`,
  `connectGatt` and `createBond` to it fail with "device busy" (GATT 0x85).
  When Linux disconnects it, a bonded device shows again as bonded and
  disconnected and can be used.
- **Connected on both sides.** The bridge's `Device1.Connect()` makes BlueZ
  show the device as connected. The shell's Bluetooth page marks it "in use
  by Android". The bridge keeps a reference per Android client. When the
  last client closes (`BluetoothGatt.close`, the app stopped, or the
  container frozen or stopped), it calls `Disconnect()`, but only for
  connections it made.
- **Bonds are BlueZ's.** The shim reports bonds from Device1 and never
  passes keys into Android. Android's `bt_config.conf` holds addresses and
  names only, and the shim rebuilds it from BlueZ at every start.
- **Pairing UI is ztOS's.** All pairing goes through BlueZ's registered agent
  (`ztos-bt-agent`). Today the agent accepts only while the phone is
  discoverable, so a request that started from an Android app needs one
  rule: accept it while the bridge has a `Pair()` outstanding for that
  device.
- **Frozen Android.** A freeze or the screen-off policy ends discovery and
  advertising started by Android, and keeps GATT connections only for
  `keep_awake` apps.

### Container side

- Add `android.hardware.bluetooth.xml` and `android.hardware.bluetooth_le.xml`
  to the vendor permissions (`patch-vendor.sh`). Without them, apps see no
  Bluetooth.
- Install the shim as `/system/lib64/libbluetooth.so` (and
  `libbluetooth_jni`'s dependency), with `strip-image.sh` and a checksum
  pin, as for init and the composer.
- No HCI HAL service is needed: the shim never opens one.
- The shim must be built against the LineageOS 20 `packages/modules/Bluetooth`
  headers of the pinned build, because the C++ interface vtables have to
  match. That needs the NDK plus those headers, built off the phone.

### Stages

1. **BLE central**: adapter state, bonded list, LE scan with filters, GATT
   client, and the policy's hide and release rules. That is enough for most
   IoT set-up apps.
2. **Peripheral**: advertising sets and the GATT server.
3. **Classic**: discovery, `createBond` through BlueZ's agent, RFCOMM
   sockets.

Effort is roughly 2 to 3 weeks for stage 1 (shim, bridge and policy),
1 week for stage 2 and 1 week for stage 3. Each stage needs the container
running, which needs the `--waydroid` kernel.

### Testing rules

Scanning, and advertising a test service, are fine. Don't pair with, connect
to or disconnect any of the user's existing devices (AirPods, Galaxy Fold6),
and don't remove bonds. Ask the coordinator before any pairing test. The user
pairs IoT devices themselves with nRF Connect.
