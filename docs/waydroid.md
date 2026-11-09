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
3. JNI, into the native stack (Fluoride/GD), through its C and C++
   interfaces: `bt_interface_t`, `btgatt_interface_t` (client and server),
   `BleScannerInterface`, `BleAdvertiserInterface` and `btsock_interface_t`.
   Below that is the HCI HAL.

The shim implements layer 3's interfaces and keeps the Java Bluetooth app,
permissions, scan filters and callbacks unchanged, so apps see stock Android
behaviour.

**What the pinned image has** (system.img of 20260927, `TQ3A.230901.001`,
read with debugfs on 8 Nov):

- `com.android.btservices` is a *flattened* APEX directory,
  `/system/apex/com.android.btservices/`, so `strip-image.sh` can change it
  like any other file.
- There is **no `libbluetooth.so`**. In Android 13 the stack is linked
  statically into `lib64/libbluetooth_jni.so` (13.8 MB). The JNI code reaches
  it only through `hal_util_load_bt_library()`, which returns
  `&bluetoothInterface` (no `dlopen`; `DYNAMIC_LOAD_BLUETOOTH` is off).
  `bluetoothInterface` (328 bytes) and `JNI_OnLoad` are exported dynamic
  symbols. Every profile interface, GATT, scanner, advertiser and sockets
  included, is fetched through `bluetoothInterface.get_profile_interface`.
- There is **no Bluetooth app** (`com.android.bluetooth`, `Bluetooth.apk`):
  no `app/` in the APEX and none in any app directory. The LineageOS waydroid
  target leaves it out. `framework-bluetooth.jar`, `service-bluetooth.jar`
  (with its odex) and the app's privapp allowlist are there. Without the app,
  `BluetoothManagerService` has nothing to bind, and without the
  `android.hardware.bluetooth*` features system_server doesn't start it.

So the shim does not replace a `libbluetooth.so`. It goes in as a **wrapper
`libbluetooth_jni.so`**: the stock library is renamed
`libbluetooth_jni_stock.so` in the same APEX directory, and the wrapper's
`JNI_OnLoad` `dlopen`s it, makes the page of its `bluetoothInterface`
writable, copies the shim's `bt_interface_t` over it, and calls the stock
`JNI_OnLoad`, which registers the stock JNI methods. From then on the JNI
code calls the shim. The stock stack stays in memory but is never
initialised (`init` and `enable` are the shim's). This keeps the JNI layer
the exact one the image was built with. The only ABI the shim must match is
that of the interface structs and classes of `packages/modules/Bluetooth` at
`lineage-20.0`. See "Shim build" below.

Below the interfaces, it speaks a small framed protocol over a Unix socket.
On the other end is a host daemon, **`ztos-btbridge`**, a user of BlueZ's
D-Bus API. It runs on the host (BlueZ and `AF_BLUETOOTH` are there), as the
user's systemd service, and is part of the ztos-waydroid package. It is not
a stack: BlueZ does SMP, SSP, encryption and key storage.

Rejected alternatives at other layers:

- **The HCI HAL.** That would mean emulating a controller over BlueZ's API
  while BlueZ also drives the real one. It can't honour BlueZ's ownership.
- **Replacing the Java Bluetooth app.** That would mean reimplementing about
  ten AIDL interfaces, and it breaks with every Android release.
- **Interposing `bluetoothInterface` by symbol** (loading a library first
  that defines it). It doesn't work: the JNI code's reference is resolved at
  link time, and no dynamic relocation names it.

### Socket and trust

- **Where.** The host's `~/.local/share/waydroid/data` is Android's `/data`
  (Waydroid's session bind mount). The host user is uid 1000, which is
  Android's `system` and owns `/data/misc`. The bridge makes
  `/data/misc/ztos-btbridge/` (mode 0711) and listens on `bridge.sock` in
  it. This needs no Waydroid change, and the path survives a bridge restart.
- **Who.** The socket is `SOCK_SEQPACKET`. The bridge checks `SO_PEERCRED`
  of every connection: the uid must be 1002 (`AID_BLUETOOTH`, the Bluetooth
  app; the container has no user namespace, so uids are the host's), and the
  pid must be in the `lxc.payload.waydroid` cgroup. Anything else is closed
  at once. Apps can't reach the socket. The bridge doesn't trust the shim
  anyway: all policy is on the host.
- **What Android never gets.** No HCI: the container's network namespace
  has no `AF_BLUETOOTH`, and the bridge has no op that sends raw HCI or mgmt
  commands. No link keys or LTKs: no op returns them, and BlueZ's D-Bus API
  doesn't expose them either. No agent role: the shim never raises Android's
  pairing dialog. RFCOMM connections (stage 3) are handed over as an
  already-connected socket fd (`SCM_RIGHTS`), which carries data only.

### Protocol

Each message is one `SOCK_SEQPACKET` packet of at most 64 KiB. It starts
with a header `op:u16, flags:u16, id:u32` (little endian), followed by the
arguments, each a type byte and its value (`y` u8, `q` u16, `u` u32, `i`
i32, `B` bool, `a` address in 6 bytes, `U` 128-bit UUID, `s` UTF-8 string,
`b` bytes, `L` list, `T` tuple). Requests carry an id. The answer is a
`REPLY` with that id and a status first. Events carry id 0. The table of ops
and their argument signatures is in
`android/waydroid/bluetooth/ztbt/proto.py`, and a test checks the shim's
C++ header against it. The bridge refuses a packet whose arguments don't
match its op's signature.

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
| `setCharacteristicNotification` (local only), then the app's CCCD (0x2902) write → `register_for_notification`, `write_descriptor` | the CCCD write becomes `StartNotify()`/`StopNotify()` on its characteristic (BlueZ refuses direct CCCD writes); `PropertiesChanged(Value)` → `notify_cb`; a CCCD read answers the last value written |
| `requestMtu` → `configure_mtu` | BlueZ exchanges the MTU itself (it asks for 517 on every LE connection); the shim answers with GattCharacteristic1 `MTU` |
| `readRemoteRssi`, `requestConnectionPriority`, `setPreferredPhy` | Device1 `RSSI`; no-op with success; `Adapter1`/mgmt PHY configuration, read-only at first |
| `BluetoothDevice.createBond` → `bt_interface_t::create_bond` | Device1 `Pair()`; **BlueZ's agent** (ztOS's `ztos-bt-agent`, the shell's sheet) asks the user; the result → `bond_state_changed_cb`. The Android pairing dialog is never shown: the shim never calls `ssp_request_cb`/`pin_request_cb` |
| encrypted characteristic (insufficient authentication) | BlueZ itself pairs on the ATT error (its agent again), retries, and returns the value; Android only sees a slower read |
| `removeBond` | refused for bonds BlueZ made outside Android; allowed only for a bond an Android app created, and only after the shell's confirmation (Device1 `RemoveDevice` is never called silently) |
| `BluetoothLeAdvertiser.startAdvertisingSet` → `BleAdvertiserInterface::StartAdvertisingSet` | LEAdvertisingManager1 `RegisterAdvertisement` of an `org.bluez.LEAdvertisement1` object exported by `ztos-btbridge` (Type, ServiceUUIDs, ManufacturerData, ServiceData, LocalName, Includes `tx-power`, MinInterval/MaxInterval, `SecondaryChannel` for extended); 5 instances shared with Linux |
| `openGattServer`, `addService` → `btgatt_server_interface_t::add_service` | GattManager1 `RegisterApplication` of an ObjectManager tree (GattService1/Characteristic1/Descriptor1) that `ztos-btbridge` exports per Android server; `ReadValue`/`WriteValue` calls from BlueZ → `request_read_*_cb`/`request_write_*_cb`, Android's `sendResponse` → the D-Bus reply; `notifyCharacteristicChanged` → `PropertiesChanged(Value)` or the `AcquireNotify` fd |
| `createRfcommSocketToServiceRecord(uuid)`, the hidden `createRfcommSocket(channel)`, `connect()` → `btsock_interface_t::connect` | by UUID: Profile1 (client role) for that UUID + Device1 `ConnectProfile(uuid)`, BlueZ does SDP and security, `Profile1.NewConnection(fd)`; by channel: an RFCOMM socket from the bridge. The RFCOMM socket stays in the bridge, which relays it to Android's stream (see "Classic RFCOMM") |
| `listenUsingRfcommWithServiceRecord` → `btsock_interface_t::listen` | not yet: ProfileManager1 `RegisterProfile(uuid, {Role: server})`, `NewConnection` per incoming connection |
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
  rule: ask (with the usual sheet) while the bridge has a `Pair()`
  outstanding for that device. The bridge writes the address to
  `$XDG_RUNTIME_DIR/ztos-btbridge/pairing` for as long as the `Pair()` call
  runs (at most 60 s). The agent asks about that device while the file is
  younger than 90 s and names it. The agent still never says yes by itself.
- **Bonded list.** The list Android gets at start leaves out bonded devices
  that Linux is connected to. If Linux connects one later, Android keeps it
  as bonded (a bond state change would make apps think it was unpaired), but
  its connects fail with "busy" until Linux lets go.
- **Frozen Android.** The bridge watches the container's cgroup
  (`/sys/fs/cgroup/lxc.payload.waydroid/cgroup.events`, `frozen 1`). On a
  freeze it ends Android's discovery and advertising and disconnects every
  connection it made for Android. A `keep_awake` app keeps the container
  thawed, so its connections stay. While Android can't read, scan results
  are dropped and the other events are queued (bounded) until it thaws. A
  stopped container closes the socket, which releases everything too.
- **Removing bonds.** `removeBond` from Android is refused, except for a
  bond that an Android app created through the bridge. The bridge records
  those addresses in `~/.local/state/ztos-btbridge/android-bonds`. A
  confirmation sheet in the shell for that case is still to do.

### Container side

- Add `android.hardware.bluetooth.xml` and `android.hardware.bluetooth_le.xml`
  to the vendor permissions (`patch-vendor.sh`). Without them, apps see no
  Bluetooth and system_server doesn't start `BluetoothManagerService`.
- Add the Bluetooth app as `app/Bluetooth/Bluetooth.apk` in the flattened
  APEX directory, where `lineage-20.0` installs it. It must come from a build
  of the same release (see "Shim build").
- Rename the stock `libbluetooth_jni.so` to `libbluetooth_jni_stock.so` and
  install the wrapper under the old name. Use `strip-image.sh` and checksum
  pins, as for init and the composer.
- Switch off the profiles the bridge doesn't offer (A2DP, HFP, AVRCP, HID,
  PAN, LE Audio, hearing aids and so on) with the app's `profile_supported`
  config (a runtime resource overlay) and the `bluetooth.profile.*.enabled`
  properties. Then the Java side never asks for their native interfaces.
- Grant the app its Nearby-devices runtime permissions (`BLUETOOTH_CONNECT`,
  `_SCAN`, `_ADVERTISE`) with a default-permissions file in
  `product/etc/default-permissions`. Without them its ProfileService refuses
  to start ("Permission denied!"), GattService never runs, and Android times
  out turning Bluetooth on (`BLE_START_TIMEOUT`) without ever asking the
  stack. Android applies default grants only at a set-up's first boot or
  after a fingerprint change. So an installation whose Android booted before
  the app was added needs them once with `pm grant com.android.bluetooth
  android.permission.BLUETOOTH_CONNECT` (and `_SCAN`, `_ADVERTISE`), as on
  the test phone on 8 Nov.
- No HCI HAL service is needed: the shim never opens one.

### Shim build

The wrapper and shim are C++ for aarch64 bionic. They are built on the PC
with NDK r25c (API 33) and:

- the interface headers of LineageOS `android_packages_modules_Bluetooth`
  at `lineage-20.0` (`system/include/hardware/bluetooth.h`, `bt_gatt*.h`,
  `ble_scanner.h`, `ble_advertiser.h`, `bt_sock.h`, `types/`), fetched by a
  script at a pinned commit;
- AOSP `external/libchrome` (r576279) headers at `android-13.0.0_r75`.
  These are needed because `BleScannerInterface`, `BleAdvertiserInterface`
  and the GATT client take `base::Callback`s. The shim only runs and copies
  callbacks the JNI code made. The few out-of-line functions that takes
  (libchrome's `callback_internal.cc` and `memory/ref_counted.cc`) are
  compiled into the shim. It can't link against the stock library, because
  both have the soname `libbluetooth_jni.so`;
- the NDK's libc++ headers compiled with `_LIBCPP_ABI_NAMESPACE=__1`, and
  linked against the image's own `libc++.so`, not the NDK's
  `libc++_shared` (`std::__ndk1`). `std::vector` and `std::string` cross the
  interface, so both sides must use the same libc++. NDK r25c's clang is
  r450784d1, the compiler Android 13 itself is built with, so its libc++
  headers are the platform's.
- the flags AOSP builds arm64 platform code with: `-fsanitize=shadow-call-stack`,
  `-fno-exceptions`, `-fno-rtti`, `NDEBUG` (libchrome's
  `RefCountedThreadSafeBase` has extra members when DCHECKs are on).

`shim/fetch.sh` gets the pinned sources (`shim/sources.conf`), the NDK
(sha256-checked) and, from the system image with debugfs, `libc++.so`,
`liblog.so` and the stock `libbluetooth_jni.so`. `shim/build.sh` builds
`out/libbluetooth_jni.so` (125 KB; the build is reproducible) and fails
unless:

- it exports only `JNI_OnLoad` and needs only `libc++`, `liblog`, `libdl`,
  `libc` and `libm`;
- it links with no undefined symbol against the image's libraries;
- `sizeof(bt_interface_t)` is the size of the stock `bluetoothInterface`
  symbol (328). The wrapper also checks the stock `size` field at run time,
  and refuses to patch on a mismatch;
- the libchrome callback functions built into the shim use the same field
  offsets as the stock library's (`shim/abi-check.sh` compares the
  disassembly).

The built library is kept in `prebuilts/waydroid-bt/`, with its sha256.

**The Bluetooth app** comes from AndyYan's LineageOS 20 GSI (lineage-20-td,
20251021, `TQ3A.230901.001`), the one `Bluetooth.apk` from its btservices
APEX. Source URL and hashes are in `prebuilts/waydroid-bt/SOURCES`.
`bluetooth/tools/check-bt-app.py` reads the app's dex files and the stock
library's `JNINativeMethod` tables (through their RELR relocations). All 23
classes with native methods match the Waydroid library's tables. As in stock
AOSP 13, AdapterService declares three natives that no library registers.
The GSI's `framework-bluetooth.jar` is byte-identical to the Waydroid
image's, and its JNI library looks up the same Java callbacks.

**Testing without Android.** `tests/test_shim_host.py` builds the shim for
the PC (g++, the same sources and headers, `shim/tests/host-build.sh`) and
drives it the way Android's JNI does (`shim/tests/host_test.cc`). It runs
against the bridge's real core and socket, with a fake BlueZ that answers at
once and plays the radio. The test covers:

- enable, and the bonded list without the Linux-connected device;
- LE scan results and Classic discovery;
- GATT connect, database, reads (including an ATT error), the CCCD write and
  the notification it brings, writes, MTU and PHY;
- the Linux-connected device refused (133);
- pairing through `Pair()`, removing a Linux bond refused, removing the
  Android-made one allowed;
- disable;
- that no PIN or SSP request ever reaches Android, and that every callback
  comes on the one thread that announced itself with `ASSOCIATE_JVM`.

### Classic RFCOMM (stage 3, ahead of stage 2)

The user's thermal printers (the Nelko app) use BLE or Classic SPP
(`00001101-...`, RFCOMM). Classic comes before the peripheral stage.

**What Android expects.** `BluetoothSocket.connect()` gets an fd from
`btsock_interface_t::connect` at once: Fluoride's end of a Unix stream
socketpair. It then reads a channel (`int`), then `sock_connect_signal_t`
(packed, 20 bytes: size 20, address, channel, status, max tx and rx packet
sizes; native order, which is little endian), and a non-zero status or the
end of the stream fails with `IOException`. After that, the stream is the
data.

**How the bridge does it.**

1. The shim makes the socketpair and returns one end to Android as the
   socket. It sends the other end to the bridge in `RFCOMM_CONNECT`
   (address, UUID or zero, channel or -1, Android's `SEC_FLAG_*`), as
   `SCM_RIGHTS` with the packet (header flag `FLAG_FD`; the bridge closes the
   connection on any packet whose fds don't fit its op). The shim keeps no
   copy.
2. The bridge applies the same policy as for GATT: Linux connected → busy,
   adapter off or container frozen → refused. Otherwise it holds the device
   for Android. If the device isn't bonded yet, it writes the agent's pairing
   note: BlueZ pairs it (SSP) with its own agent and the shell's sheet, and
   the bond is BlueZ's.
3. **By UUID** (`createRfcommSocketToServiceRecord`, what printer apps use):
   the bridge registers an `org.bluez.Profile1` for that UUID, once, client
   role (no SDP record of ours), and calls `Device1.ConnectProfile(uuid)`.
   BlueZ does the SDP lookup, connects with its security and keys, and calls
   `NewConnection(device, fd)` (accepted only from bluetoothd's bus name).
   The bridge reads the channel from the socket.
   **By channel** (the hidden `createRfcommSocket(int)`, a common fallback in
   printer apps): the bridge opens the RFCOMM socket itself
   (`BT_SECURITY` medium when Android asked for a secure socket). The kernel
   uses BlueZ's link keys for that link.
4. Up: the bridge writes the channel and the connect signal into Android's
   stream and relays both ways (`ztbt/relay.py`: GLib, non-blocking, at most
   256 KiB waiting each way, so a slow printer slows the app instead of
   filling memory). Failed: it closes Android's end (`IOException`).
5. Down: when either side closes, what is still owed to the other side is
   written out, both are closed, and the device is let go as for GATT (the
   bridge disconnects only what it connected). A freeze closes every stream.

**Why not pass the RFCOMM socket into Android.** fd passing would work
across the namespaces, but then Android would hold an `AF_BLUETOOTH` socket
(socket options, its security level). With the relay, Android holds only a
Unix stream. For printers' rates (kB/s) the extra copy costs nothing.

**Not yet:** listening sockets (`listenUsingRfcommWithServiceRecord`, a
server-role Profile1), L2CAP and SCO sockets (refused), and `control_req`.
Insecure sockets are connected with BlueZ's default security: SSP makes
encryption mandatory for RFCOMM anyway.

**Tested without Android or a printer:**

- the core against a fake BlueZ (`tests/test_rfcomm.py`): the signal bytes,
  the relay set-up, failures, busy, freeze, remote disconnect, a connect
  finishing after its stream ended, the fd rules;
- the relay with socketpairs (`tests/test_relay.py`), including a 1 MiB job to
  a slow reader;
- the Profile1 flow in both orders of reply and `NewConnection`
  (`tests/test_bluez.py`);
- the shim end to end (`test_shim_host.py`): `btsock connect` to an echoing
  fake SPP printer by UUID, the signal read as Java reads it, a job written
  and answered, the AirPods refused (end of stream), L2CAP refused.

### LE advertising (stage 2, first half)

`BluetoothLeAdvertiser.startAdvertisingSet` reaches the shim's
`BleAdvertiserInterface::StartAdvertisingSet`. The shim sends `ADV_START`
(connectable, scannable, intervals, tx power, the AD bytes, scan response,
duration). The bridge reads Android's AD structures back into the properties
of an `org.bluez.LEAdvertisement1` that it exports and registers with
`LEAdvertisingManager1`:

- Type: `peripheral` for a connectable set, `broadcast` otherwise.
- Fields: `ServiceUUIDs` (16, 32 and 128 bit), `ManufacturerData`,
  `ServiceData`, `LocalName`, `Appearance`, `Includes: tx-power`.
- Other AD types go into `Data`.
- `Discoverable` comes from the flags, but only for a connectable set: BlueZ
  refuses flags on a broadcast ("Broadcast cannot set flags", found on the
  phone).
- `Timeout` comes from the duration.
- Advertising data and scan response are merged, and BlueZ lays them out.

New data (`SetData`) and `Enable` register the set again. When a duration
runs out, BlueZ calls `Release`, and the shim calls the set's timeout
callback.

Policy:

- Android gets at most 4 of BlueZ's 5 instances; the shim reports 4 as
  `max_adv_instance`.
- A freeze, the adapter going off, Android's Bluetooth going off, or the
  shim going away stops Android's sets.
- Legacy advertising only. Periodic advertising and the old single
  `StartAdvertising` path are refused (unsupported). `SetParameters` is
  accepted, but BlueZ keeps choosing intervals and power.
- Sets advertise with BlueZ's address, not one Android picks.

On the phone, `ztbt-cli SOCKET adv` registered a "ztOS-btbridge-test"
broadcast through BlueZ 5.87 (`ActiveInstances` 0 → 1 → 0). The shim's path
is covered by `test_shim_host.py`. Not tested inside Android yet: the image
on the phone still has the RFCOMM build of the wrapper.

### GATT server (stage 2, second half)

`BluetoothGattServer.addService` reaches `btgatt_server_interface_t::add_service`.

**Services.** The shim sends `SERVER_ADD` with the service's elements (type,
UUID, properties, permissions). The bridge (`ztbt/gatts.py`) exports them as
one BlueZ GATT application (`ztbt/gattapp.py`: an ObjectManager root, a
`GattService1`, its `GattCharacteristic1`s and `GattDescriptor1`s) and
registers it with `GattManager1.RegisterApplication`.

- Properties become BlueZ flags. The permissions add `encrypt-read`,
  `encrypt-write` and the authenticated variants: BlueZ enforces them, and
  pairs through its own agent if it must.
- The handles Android sees are the bridge's. The ATT handles on the air are
  BlueZ's.
- One service per application; deleting it unregisters it. Included
  services are refused.

**Requests.** bluetoothd's `ReadValue`/`WriteValue` calls (only taken from
bluetoothd's bus name) become `request_read_*`/`request_write_*`, carrying a
request id that the app answers with `sendResponse` (`SERVER_RESPONSE`). An
ATT status from the app becomes the matching BlueZ error. Unanswered after
25 s, BlueZ gets "unlikely error". Write-without-response is acknowledged at
once.

**Notifications.** BlueZ owns every CCCD (it makes one for each
characteristic that notifies or indicates), so the 0x2902 descriptors Android
declares aren't exported. BlueZ's `StartNotify`/`StopNotify` (first central
subscribed, last one left) become the CCCD writes (`01 00`, `02 00`, `00 00`)
Android apps wait for, attributed to the connected central. With several
centrals that is a guess: BlueZ doesn't say which one subscribed.
`notifyCharacteristicChanged` becomes a `Value` change, and BlueZ notifies or
indicates each subscribed central as it subscribed. The indication-sent
callback comes when the value is handed to BlueZ, not on each central's
confirmation.

**Connections.** A remote central becomes a connection of Android's servers
only when it first touches one of their attributes (`EV_SERVER_CONN`, with
the MTU). Devices that only use Linux's services, such as the AirPods, never
show up in Android. Disconnection comes from BlueZ's `Connected`. A server
can't connect out (`connect` refused); `cancelConnection` does nothing.

**Lifetime.** A freeze or the adapter going off unregisters Android's
services (centrals see Service Changed), and a thaw registers them again.
Android's Bluetooth going off or the shim going away removes them.

**Tested on the PC:**

- the core against the fake BlueZ (`tests/test_gatts.py`);
- the D-Bus objects on a private bus (`tests/test_gattapp.py`): what
  bluetoothd reads with `GetManagedObjects`, async reads and writes, only
  bluetoothd's calls taken, the `Value` signal;
- the shim end to end (`test_shim_host.py`): a heart-rate service added,
  a central's read answered by the app, its subscription seen as a CCCD
  write, a notification sent, the service deleted.

Not on the phone yet.

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

### The bridge (built)

`android/waydroid/bluetooth/`:

| File | What |
|---|---|
| `ztos-btbridge` | the daemon (Python, dbus-python, GLib) |
| `ztbt/proto.py` | the wire format and the table of ops |
| `ztbt/core.py` | the policy and the ops, with no D-Bus or socket in it |
| `ztbt/bluez.py` | BlueZ: Adapter1, Device1, the GATT objects |
| `ztbt/server.py` | the socket, the `SO_PEERCRED` and cgroup checks, the freezer watch, the store |
| `ztos-btbridge.socket`, `.service` | user units. The socket is enabled by the package; the daemon starts on the first connection and exits 5 minutes after the last one |
| `ztbt-cli` | the shim's side of the protocol, for tests on the host |
| `tests/` | `python3 -m unittest` in that directory: the wire format, the policy and BLE central ops against a fake BlueZ, the backend's bookkeeping, the socket |

The package installs it in `/usr/lib/ztos-waydroid/btbridge/`.
`ztos-bt-agent` (base system) reads the bridge's pairing note.

The bridge carries stage 1's ops now: adapter, bonded list, LE scan and
Classic discovery, GATT client (connect, database, read, write,
notifications through the CCCD), MTU, RSSI, and bonds through BlueZ. On the
phone it was run against BlueZ 5.87 with `ztbt-cli`
(`ztos-btbridge --socket /tmp/... --allow-uid $(id -u) --no-container-check`):
`HELLO`, the bonded list, and LE and auto scans found 15 advertisers, with
the AD structures rebuilt. Connecting and pairing were not tried on the
phone, because of the testing rules below.

Not done yet: reliable (prepared) writes, `AcquireNotify` (a notification
that arrives while the same characteristic is being read is lost, because
BlueZ's `Value` change can't be told apart from the read's), background
(`autoConnect`) connections (the bridge always connects directly), PHY and
connection priority, advertising and the GATT server (stage 2), and RFCOMM
(stage 3).

### Testing rules

Scanning, and advertising a test service, are fine. Don't pair with, connect
to or disconnect any of the user's existing devices (AirPods, Galaxy Fold6),
and don't remove bonds. Ask the coordinator before any pairing test. The user
pairs IoT devices themselves with nRF Connect.
