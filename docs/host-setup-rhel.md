# Building ztOS on Fedora, Rocky, Alma and RHEL

`installer/README.md` lists the packages for Arch Linux. This page gives the
same setup for the Red Hat family, tested from cloud images in KVM guests
(6 vCPU, 16 GB RAM, SELinux enforcing, unprivileged user) on 2026-11-06, at
commit `aeb4749`.

The build never needs root. These steps do, because they install packages.

## What was tested

The build tests below used a symlink at `/usr/lib/qt6/uic`. The build now finds
`/usr/lib64/qt6/libexec/uic` by itself, so the commands here no longer make one. That
`uic` was run by hand on `apps/music/form.ui`; no full build was repeated without the
symlink.

| Distribution | What ran | Result |
| --- | --- | --- |
| Rocky Linux 10.2 | Full cold build for `starlte`, no EPEL, compilers removed first | Exit 0, `root.img` passes `e2fsck -fn`, listing looks right |
| Rocky Linux 10.2 | Full build for `starlte` with CRB + EPEL `pyside6-tools` | Exit 0, same checks |
| Fedora 45 | Full cold build for `starlte` | Exit 0, same checks |
| AlmaLinux 10.2 | Package install, `ztos doctor`, reboot. **No build** | Doctor passes; binfmt survives the reboot |
| RHEL 10 | **Not tested.** The commands below are mapped from Rocky and Alma | Unknown |

The build command in each full build was:

```
installer/ztos doctor
installer/ztos build --target starlte --user ztest --password ztest --hostname ztos --timezone UTC
```

These were build tests only:

- No test image was flashed or booted.
- The `qemu` target was not built, so `mkfs.fat`, `mcopy`, `mmd` and `sfdisk`
  passed `doctor` but were not exercised.
- Anything about flashing is marked below as coming from one real install on
  Rocky 10, or from reading the code.

## Rocky Linux 10 and AlmaLinux 10

No EPEL or CRB is needed to build.

```sh
sudo dnf -y --setopt=install_weak_deps=False install \
    git-core bsdtar dosfstools mtools e2fsprogs python3 util-linux shadow-utils \
    curl tar xz qt6-qtbase-devel

# qemu-user is not in EL10 (see below): take this one package from Fedora, signature checked
sudo dnf -y --setopt=install_weak_deps=False \
    --repofrompath=f45u,https://dl.fedoraproject.org/pub/fedora/linux/updates/45/Everything/x86_64/ \
    --repo=f45u --setopt=f45u.gpgcheck=1 \
    --setopt=f45u.gpgkey=https://src.fedoraproject.org/rpms/fedora-repos/raw/f45/f/RPM-GPG-KEY-fedora-45-primary \
    install qemu-user-static-aarch64
```

Check the key's fingerprint when dnf shows it:
`4F50 A611 4CD5 C697 6A7F 1179 655A 4B02 F577 861E`.

This exact list was installed on Alma. The Rocky cold build added only
`git-core bsdtar mtools qt6-qtbase-devel`, because the cloud image already had
the rest. Name them all, since a smaller install may not have them.

The Fedora package is a static binary and needs only `/bin/sh` to install. The
`--repofrompath` option applies to that one command, so no Fedora repository
stays enabled and `dnf update` will not update this package. Repeat the command
to update it. When Fedora 45 reaches end of life, change the release in both URLs.

**Using EPEL for `uic`.** `qt6-qtbase-devel` is enough. `pyside6-tools` is the
other way, with a real `pyside6-uic`:

```sh
sudo dnf -y install epel-release
sudo dnf -y --setopt=install_weak_deps=False install pyside6-tools
```

A full Rocky build passed this way, with CRB enabled. On Alma the install also
resolves with CRB disabled (dry run only). It is larger: 246 packages and 1.2 GB
on Rocky, against 97 packages and 617 MB for `qt6-qtbase-devel`. Both output the
same Python from `form.ui`.

**Enabling CRB**, which only the optional tools below need:
`sudo dnf config-manager --set-enabled crb`. The Alma cloud image already had
it on; the Rocky one did not.

### RHEL 10 (not tested)

Use the Rocky list above. The differences are in how repositories are enabled:

```sh
sudo subscription-manager repos --enable codeready-builder-for-rhel-10-x86_64-rpms
sudo dnf install https://dl.fedoraproject.org/pub/epel/epel-release-latest-10.noarch.rpm
```

Rocky and Alma get `qt6-qtbase-devel`, `bsdtar` and `mtools` from AppStream. Whether
RHEL ships all three in the same place was not checked. If one is not found, try
it again with CodeReady Builder enabled.

## Fedora

```sh
sudo dnf -y --setopt=install_weak_deps=False install \
    git-core bsdtar dosfstools mtools e2fsprogs python3 util-linux shadow-utils \
    curl tar xz qemu-user-static-aarch64 qt6-qtbase-devel
```

The Fedora 45 cloud image already had `qemu-user-static` with binfmt active, and
`mtools`. The test added only `git-core bsdtar dosfstools qt6-qtbase-devel`. The full list
above was not installed as one command on Fedora. `pyside6-tools` is also available
(299 packages against 159, dry run only).

## Your account: subuid and subgid

The build runs in `unshare --map-auto --map-root-user`, which needs a range in
**both** files. `useradd` normally adds them. An account made before that, or
one from a directory service, may have none.

```sh
grep "^$USER:" /etc/subuid /etc/subgid
sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 "$USER"   # if either is missing
```

`newuidmap` and `newgidmap` come from `shadow-utils`, with file capabilities, on
EL10.

## binfmt

The build runs ARM programs in a chroot, so the kernel must have a `qemu-aarch64`
entry with the **F** flag. Check it:

```sh
cat /proc/sys/fs/binfmt_misc/qemu-aarch64    # want: enabled, flags: F
```

`qemu-user-static-aarch64` ships `/usr/lib/binfmt.d/qemu-aarch64-static.conf` with
that flag. It registers on install and comes back on boot; a reboot was tested
on Rocky and Alma. No `/etc/binfmt.d` file is needed.

**Where to get it on EL10.** AppStream, BaseOS, Extras, CRB and EPEL have no
`qemu-user-static` and nothing that provides `/usr/bin/qemu-aarch64-static`
(checked on Alma 10). The Fedora package above is the tested source. Avoid
third-party EL10 rebuilds installed with `--nogpgcheck`.

## What the build needs, and how we know

| Need | Package | Evidence |
| --- | --- | --- |
| `git` | `git-core` | Clone failed without it on Rocky and Fedora; also absent from the Alma image |
| `bsdtar` | `bsdtar` | `doctor` failed without it on Rocky and Fedora; also absent from the Alma image |
| `mkfs.fat` | `dosfstools` | `doctor` failed without it on Fedora. Only the `qemu` target uses it (`build.py:476`) |
| `mcopy`, `mmd` | `mtools` | `doctor` failed without them on Rocky. Only the `qemu` target uses them |
| `sfdisk` | `util-linux` | Checked by `doctor`. Only the `qemu` target uses it (`build.py:500`) |
| `unshare --map-auto` | `util-linux` | Used by the build; works with 2.40 (EL10) and 2.42 (Fedora) |
| `newuidmap`, `newgidmap` | `shadow-utils` | Used through `--map-auto` |
| subuid and subgid | account | Removing either made `unshare` fail |
| qemu binfmt with F | `qemu-user-static-aarch64` | Without F the build fails at "setting up package signing keys" |
| `mke2fs` 1.47 or later | `e2fsprogs` | `-O ^orphan_file` at `build.py:374` needs it (read from code). 1.47.1 and 1.47.4 work; older was not tried |
| Python 3.11 or later | `python3` | The SDK imports `tomllib` (read from code). 3.12 and 3.15 work; older was not tried |
| `uic` | `qt6-qtbase-devel`, or `pyside6-tools` | Build failed without one on Rocky and Fedora |
| `curl` | `curl` | Downloads the Arch Linux ARM tarball (`build.py:110`) |
| `tar`, `xz` | `tar`, `xz` | Unpack `ztos-media` (`build.py:238`) |

**A compiler is not needed on the host.** `qt6-qtbase-devel` pulls in `gcc`, `make`
and `cmake`, but the Rocky cold build removed them all first (`rpm -e --nodeps`) and
still passed. Programs are compiled inside the ARM chroot.

## Time and disk

| Run | Wall clock | CPU (user) | Disk |
| --- | --- | --- | --- |
| Rocky 10 cold, no EPEL | 59 min 49 s | 94 min | `root.img` 5.3 GB; total not measured |
| Rocky 10 with EPEL | 73 min 9 s | 114 min | total not measured |
| Fedora 45 cold | 58 min 15 s | 94 min | Clone and build 13 GB (`du`) |

- **Disk:** `root.img` is 10 GiB but sparse, 5.3 GB allocated. Peak use on the
  Fedora guest was 12.3 GB including the OS, on btrfs with zstd compression, so
  a filesystem without compression will use more. The packages add about 1 GB.
  15 GB free is tight; plan for 20 GB.
- **CPU:** a load average near 1 for most of the run. More cores help little.
- **RAM:** peak memory was not measured. 16 GB was enough.
- **Network:** a 790 MB tarball plus about 2 GB of packages.

## Firewall

Firewalld's default zone blocks what flashing needs, and **no script touches the
firewall**. The host listens and the phone connects to it:

| Script | Port |
| --- | --- |
| `scripts/flash-root.sh` (line 69) | 5557 |
| `scripts/flash-from-rescue.sh` | 5558 |
| `scripts/flash-boot-from-arch.sh` | 5559 |
| Development tools | 5561, 5576, 5581, 5583, 5596, 8080 |

**A blocked port stops the install with a message.** `flash-root.sh` tries the link
before it writes anything, and says what to do if the phone cannot connect. Before that
check it waited forever and printed nothing (in a test with a stub phone over a veth pair
it was still waiting when killed after 120 s).

The better fix is to put only the USB interface in the `trusted` zone, through its
NetworkManager connection (next section), and open nothing else:

```sh
sudo nmcli con mod ztos-usb connection.zone trusted
sudo nmcli con up ztos-usb
```

The other way is to open the ports on the zone the interface is in. Interface names
vary, so look it up first:

```sh
sudo firewall-cmd --get-active-zones
sudo firewall-cmd --zone=public --add-port=5557/tcp --add-port=5558/tcp --add-port=5559/tcp
```

Without `--permanent` this lasts until the next reload (tested). Both methods let the
connection through in the veth test on Rocky.

## The USB network

**Confirmed on a real phone:** installer builds do not serve DHCP on USB
(`build.py:332` disables `systemd-networkd`). NetworkManager on the host keeps
trying DHCP and wipes the interface about every 45 seconds. Give it a static address
instead:

```sh
sudo nmcli con add type ethernet con-name ztos-usb \
    802-3-ethernet.mac-address 02:5a:42:00:00:02 \
    ipv4.method manual ipv4.addresses 172.16.42.2/24 ipv4.never-default yes \
    ipv6.method link-local
```

A rescue boot does serve DHCP, but the static address works there too.

The command, the zone change and the address were checked on Alma against a veth
with that MAC address. The DHCP loop itself needs the phone and was not reproduced in
a VM.

## Optional tools

### Installing on a phone (from one real install on Rocky 10)

None of this can be fully tested in a VM.

| Tool | EL10 | Fedora |
| --- | --- | --- |
| `adb` | Not in EL10 repos or EPEL. Use Google's `platform-tools` zip (runs on Alma 10) | `android-tools` (installs and runs) |
| `socat` | `socat` (AppStream) | `socat` |
| `gzip`, `ping`, `python3` | Already in the cloud images | Already in the cloud images |
| Download mode, to put TWRP on | `odin4` built from source | `heimdall` 2.2.2 installs; not tried on a phone |

Heimdall is not in EL10 or EPEL. The real install used
[odin4](https://github.com/Llucs/odin4), whose build uses `cmake`, `gcc-c++`,
`libusb1-devel`, `cryptopp-devel` (EPEL) and `libstdc++-static` (CRB). That list comes
from its `CMakeLists.txt` and the host's dnf history; it was not rebuilt in a guest.
The USB permissions `adb` and `odin4` need were not tested.

`scripts/flash.sh` refuses to run unless `stock/BOOT.img`, `stock/RECOVERY.img` and
`stock/EFS.img` exist. The installer offers to save them first; see
[the backups](../installer/README.md#the-backups-in-stock).

### The emulator

**Fedora:**

```sh
sudo dnf -y --setopt=install_weak_deps=False install qemu-system-aarch64-core edk2-aarch64 \
    qemu-device-display-virtio-gpu-pci-gl qemu-device-display-virtio-gpu-gl \
    qemu-ui-gtk qemu-ui-opengl qemu-ui-egl-headless
```

These install, and QEMU reaches its UEFI firmware. ztOS was not booted in it.
`doctor` asks QEMU itself which graphics and windows it has.

**EL10:** `qemu-system-aarch64` is not in AppStream, CRB or EPEL. `qemu-kvm` is there
but is x86-only, so there is no packaged way to run the emulator. Only `edk2-aarch64`
exists.

### Rebuilding kernel modules (not tested)

`scripts/build-bt-modules.sh`, on the `feature/power-menu` branch, needs the following.
The packages resolve on Alma 10 and Fedora 45 (dry run only); nothing was compiled in
these tests.

```sh
sudo dnf install flex bison bc openssl openssl-devel perl elfutils-libelf-devel make gcc
```

It also needs the Arm GNU 14.2 toolchain (`arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu`)
under `build/toolchain`, and `build/linux-7.1`. A Podman container has been reported to work
for this on a Rocky host; that was not repeated here.

## Known traps

- **Removing qemu leaves it working until a reboot.** With the F flag the kernel keeps
  the binary open, so the entry stays after `dnf remove` and ARM programs still run. With
  the binary gone, restarting `systemd-binfmt` fails and the entry disappears (tested on
  Rocky), and a reboot should do the same (not tested). `doctor` warns about this: "the
  qemu interpreter is installed: warning".
- **The tarball is not verified.** It comes over plain HTTP with no checksum
  (`build.py:27`, `build.py:110`).
- **Images differ slightly by host.** Fedora's `mke2fs` adds `metadata_csum_seed`, which
  Rocky's does not. Inode counts match and block counts nearly do, but neither image
  was booted, so this difference is untested on the phone.
