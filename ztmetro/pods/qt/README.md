# ztPods on a PC: ztpods-qt and ztos-podsd

The AirPods app has two front ends over one daemon. On the phone it is `ztpods` (ztMETRO). On a Linux desktop (KDE Plasma, or any Qt desktop) it is `ztpods-qt`, a Qt 6 Widgets app. Both read the same `ztos-podsd` (`../ztos-podsd.c`) and use the same rows, modes and texts (`../pods-ui.h`); only the window differs. `../README.md` says what the daemon does and where its facts come from.

| File | What |
|---|---|
| `main.cpp` | Arguments: `ztpods-qt [--tray] [MAC]`, `--render` |
| `mainwindow.cpp` | The four tabs: Status, Listening, Controls, About |
| `podsclient.cpp` | The state file and the command socket. No Bluetooth. |
| `test.cpp`, `fixtures/` | Every control against a pretend daemon (`ctest`) |
| `CMakeLists.txt` | The app, the daemon if it can, the install |
| `ztos-podsd.service.in` | The desktop's user unit |
| `Containerfile` | Builds the daemon on a Rocky 10 host with no `systemd-devel` |

This isn't the phone's build. `ztmetro/Makefile` is written for building on the phone (aarch64 Arch Linux ARM), and its `all` and `install` targets build and install the whole shell into `/usr/local`. Don't run them on a PC. The PC has its own build: CMake for the app, and either CMake or the container for the daemon. The Makefile has two targets for it, `pods-qt` and `podsd-container`, and neither is part of `all`.

## Before anything: pair them with the PC

The daemon never pairs, powers or configures Bluetooth. Before it can do anything:

1. Pair the AirPods with the PC (System Settings, then Bluetooth, with the case open and its button held), and **connect them once**. On that first connection the daemon opens the AAP channel and stores the pair's keys in `~/.local/share/ztos/pods/keys`.
2. After that, their batteries can be read from their broadcasts even when they aren't connected, for as long as the daemon keeps the keys. Before then, any AirPods nearby show only as "Nearby", in steps of 10%, and with no proof of whose they are.
3. Settings (listening mode, controls, name) are read and changed only while they are connected to this PC. When they aren't, the last values are shown read-only, with a **Connect Here** button. Only that button ever asks BlueZ to connect them, and it may take them from your phone.

AirPods talk to one host at a time. If they are playing on your iPhone, "Connect Here" takes them.

## Packages

| | App (`ztpods-qt`) | Daemon (`ztos-podsd`) | `make check-podsd` (optional) |
|---|---|---|---|
| Rocky Linux 10 / RHEL 10 / Fedora | `cmake gcc-c++ qt6-qtbase-devel` (`qt6-qtsvg` draws the in-app icon) | `gcc make pkgconf-pkg-config systemd-devel` | `python3-dbus python3-gobject dbus-daemon` |
| Arch | `cmake gcc qt6-base` (`qt6-svg`) | `gcc make pkgconf systemd-libs` (the headers are in `systemd-libs`) | `python-dbus python-gobject dbus` |

```sh
# Rocky / RHEL / Fedora
sudo dnf install cmake gcc-c++ make qt6-qtbase-devel qt6-qtsvg pkgconf-pkg-config systemd-devel
# Arch
sudo pacman -S --needed cmake gcc make qt6-base qt6-svg pkgconf systemd-libs
```

On Rocky 10, `qt6-qtbase-devel` and `systemd-devel` are in the default repositories (BaseOS/AppStream); no EPEL or CRB is needed. Nothing needs root to run. The daemon is a **user** service, and BlueZ's D-Bus policy lets the logged-in user talk to it.

## Build

From `ztmetro/pods/qt/`:

```sh
cmake -S . -B build                 # installs to ~/.local by default
cmake --build build -j
ctest --test-dir build              # optional: the controls against a pretend daemon
```

Or from `ztmetro/`, run `make pods-qt`, which does the same in `pods/qt/build`. Pass CMake options in `PODS_QT_CMAKE="..."`.

CMake options:

| Option | Default | What |
|---|---|---|
| `CMAKE_INSTALL_PREFIX` | `~/.local` | Where the binaries, `.desktop` file and icon go |
| `ZTPODS_DAEMON` | `AUTO` | `BUILD`: compile `ztos-podsd` (needs libsystemd's headers). `PREBUILT`: install `ZTPODS_DAEMON_BINARY`. `OFF`: app only. `AUTO`: `BUILD` if libsystemd's headers are found, else `PREBUILT` if `../ztos-podsd` exists, else `OFF`. The configure step prints which it chose (`-- ztos-podsd: ...`). |
| `ZTPODS_DAEMON_BINARY` | `../ztos-podsd` | The binary for `PREBUILT` |
| `ZTPODS_SYSTEMD_USER_DIR` | `~/.config/systemd/user` | Where the user unit goes |
| `ZTPODS_TESTS` | `ON` | Build `ztpods-qt-test` |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | |

### The daemon, two ways

**With `systemd-devel` installed** (the plain route): `sudo dnf install systemd-devel`, then the CMake commands above. CMake builds `ztos-podsd` alongside the app. You can also build just the daemon with `make pods/ztos-podsd` in `ztmetro/`.

**Without it, in a container** (for a host you'd rather not add `-devel` packages to). This needs podman, and works rootless:

```sh
cd ztmetro
make podsd-container      # builds pods/ztos-podsd in quay.io/rockylinux/rockylinux:10
cd pods/qt
cmake -S . -B build && cmake --build build -j   # AUTO finds ../ztos-podsd: "ztos-podsd: PREBUILT"
```

The container is Rocky Linux 10, so its glibc and libsystemd match a Rocky 10 / RHEL 10 host, and the binary runs there. For another distribution, build the daemon natively instead. A binary built on an older glibc runs on a newer one, but not the other way round. The container mounts the source with `--security-opt label=disable` rather than relabelling it for SELinux.

## Install, run and uninstall (KDE Plasma)

```sh
cmake --install build                               # ~/.local/bin, ~/.local/share/{applications,icons}, ~/.config/systemd/user
systemctl --user daemon-reload
systemctl --user enable --now ztos-podsd            # the daemon, now and at each login
kbuildsycoca6 2>/dev/null || true                   # the launcher sees the new .desktop file at once
```

Then open **AirPods** from the launcher, or run `~/.local/bin/ztpods-qt`. Other ways to start it:

- `ztpods-qt 40:DA:5C:xx:xx:xx` opens on that pair, as `ztpods MAC` does on the phone.
- `ztpods-qt --tray` puts an icon in the system tray, with the batteries in its tooltip. Closing the window hides it there, and the icon's menu has Quit. For it at login, copy `~/.local/share/applications/ztpods-qt.desktop` to `~/.config/autostart/` and add `--tray` to its `Exec=` line.

Logs: `journalctl --user -u ztos-podsd -f`. To uninstall:

```sh
systemctl --user disable --now ztos-podsd
cmake --build build --target uninstall              # removes exactly what `cmake --install` put down
systemctl --user daemon-reload
rm -rf ~/.local/share/ztos/pods ~/.local/state/ztos/pods   # optional: the pairs' keys and last readings
```

## What is different on a desktop

- **When it listens.** On the phone, the daemon listens for broadcasts only while ztMETRO says the screen is on (`$XDG_RUNTIME_DIR/ztmetro.state`). A desktop has no such file, so the desktop unit sets `ZTOS_PODSD_DESKTOP=1`, and then a missing file counts as "on". The phone's policy then applies unchanged. While the window is open (not minimised), it listens for 4 s of every 10 s. Otherwise, with a pair whose keys it has, it listens for 3 s of every 60 s. With neither, it doesn't listen at all. Whether audio is playing makes no difference (`pods_scan_policy()` in `../pods-model.h`). A locked desktop still counts as "on". If you run `ztos-podsd` by hand, set `ZTOS_PODSD_DESKTOP=1` yourself, or it never scans.
- **The unit** differs from the phone's (`rootfs/overlay/etc/systemd/user/ztos-podsd.service`). Its binary is in `~/.local/bin`, and it has no `CPUAffinity=0-3`, which pinned the daemon to the phone's efficiency cores.
- **The adapter.** It uses the first `org.bluez.Adapter1` that BlueZ reports. With two adapters (a USB dongle and a built-in one), that may not be the one the AirPods are paired with.
- **Other Bluetooth clients.** KDE's bluedevil and the daemon share BlueZ. Discovery is per client, so the daemon's short LE bursts and bluedevil's own scans don't stop each other. While a burst runs, BlueZ reports the adapter as "Discovering", and the Bluetooth settings page may show that. Only one program can hold a pair's AAP channel. Don't run LibrePods or similar alongside it.
- **Paths** are the same as on the phone, and all per user: `$XDG_RUNTIME_DIR/ztos-pods-state.json`, `ztos-pods.sock`, `ztos-pods` (the status bar's short file, unused here); `~/.local/share/ztos/pods/keys` (mode 0600); and `~/.local/state/ztos/pods/readings`.

## Tested, and not

Tested on Rocky Linux 10.2 (KDE Plasma 6.6, Qt 6.10.1, BlueZ 5.85) and in a clean Rocky 10 container:

- `ztpods-qt` builds without warnings. `ctest` passes. It uses every control against fixture states (`fixtures/`: linked, not linked, two pairs with a nearby one) and a pretend daemon socket, and checks each line sent: listening mode, every switch, slider and picker, the press-and-hold cycle (not sent with fewer than two modes), rename, connect, and forget (only after the confirmation), plus `scan-hint on` while the window shows and `off` when it hides.
- Offscreen pictures of every tab in each state, with the Breeze style (`--render`).
- `ztos-podsd` builds both ways (the container, and CMake with `systemd-devel`). `make check-podsd` passes on the PC. That test runs the whole daemon against a pretend bluetoothd on a private bus and touches no radio, and it covers both the phone rule (no `ztmetro.state`: no scanning) and the desktop rule (`ZTOS_PODSD_DESKTOP=1`: scanning). `make check-pods` passes.
- The `.desktop` file passes `desktop-file-validate`, and the install and uninstall targets were run into a scratch prefix.

Not tested yet:

- The daemon against the real adapter and real AirPods on a PC. In particular: the AAP L2CAP channel (PSM 0x1001) next to the PC's A2DP, discovery bursts with bluedevil running, and BlueZ 5.85's ManufacturerData for the broadcasts. All of this has only run on the phone so far.
- The tray icon on a real Plasma panel, Wayland window activation, and the "Open Bluetooth Settings" button (`systemsettings kcm_bluetooth`).
- GNOME and other desktops, Arch builds (the package names above are Arch's current ones), and AirPods models other than the Pro 2 USB-C.

`--render TAB OUT.png [MAC]` (TAB is `status`, `listening`, `controls` or `about`) draws a tab offscreen, with no display needed. `ZTPODS_RENDER_NOW=SECONDS` sets the time the ages are drawn from:

```sh
mkdir -p /tmp/pods-rt && cp fixtures/connected.json /tmp/pods-rt/ztos-pods-state.json
XDG_RUNTIME_DIR=/tmp/pods-rt ZTPODS_RENDER_NOW=1794093677 build/ztpods-qt --render listening /tmp/listening.png
```
