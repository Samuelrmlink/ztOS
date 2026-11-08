# x86_64 apps on ztOS (optional)

ztOS is aarch64. The optional **ztos-x86-compat** package runs x86_64 Linux
programs through [Box64](https://github.com/ptitSeb/box64). It is not part of
the base system: no image and no device package list pulls it in. You install it
when you want it and remove it as one unit, and it leaves nothing behind.

## Install, remove

The PKGBUILDs are in `packages/box64` and `packages/ztos-x86-compat`. On the
phone, or on any aarch64 Arch Linux ARM system:

```sh
packages/ztos-x86-compat/build.sh --install     # build box64 (~40 min on the S9), install both
sudo pacman -Rns ztos-x86-compat                # remove: takes box64 and squashfuse with it
```

`--install` does what the App Store would do: `box64` goes in as a dependency
and `ztos-x86-compat` as the package you asked for. cmake is only needed to
build; `pacman -Rns cmake` removes it afterwards.

The package installs:

| Path | What |
|---|---|
| `/usr/bin/ztos-x86` | the command: run, install, list, remove, status |
| `/usr/bin/ztos-x86-apps`, `/usr/share/applications/ztos-x86-apps.desktop`, its icon | **x86 Apps**, the frontend, in the app list while the package is installed |
| `/usr/lib/binfmt.d/ztos-x86_64.conf` | the binfmt rule that lets `./program` work |
| `/usr/share/doc/ztos-x86-compat/README.md` | this file |
| (box64) `/usr/bin/box64`, `/usr/bin/box64-bash`, `/usr/lib/box64-x86_64-linux-gnu/`, `/etc/box64.box64rc` | the emulator and its few x86_64 libraries |

The install script registers the binfmt rule, and systemd-binfmt registers it
again at every boot. Removing the package unregisters it first
(`/proc/sys/fs/binfmt_misc/ztos-x86_64` disappears). Box64's own
`/etc/binfmt.d/box64.conf` is left out of the box64 package, so the rule
exists only while ztos-x86-compat is installed.

Apps you add live in your home directory (`~/.local/share/ztos-x86/`, with menu
entries `~/.local/share/applications/ztos-x86-*.desktop`). Their entries carry
`TryExec=/usr/bin/ztos-x86`, so the app list hides them once the package is
removed and shows them again if you reinstall it. `ztos-x86 remove-all` deletes
them for good. Run it before removing the package if you want nothing left in
your home.

## Running things

```sh
ztos-x86 run ./program [args]      # an x86_64 ELF, a shell script, or an AppImage
./program                          # the same, once the kernel has binfmt_misc (see Kernel)
ztos-x86 install App.AppImage      # add to the app list; also .deb, .tar.*/.zip, folder, program
ztos-x86 install electron.zip --name "Electron" --exec electron
ztos-x86 list | remove ID | remove-all | status
```

Or open **x86 Apps**: it lists what you installed, starts and removes apps, and
**Add an x86_64 app…** takes an AppImage, a .deb, an archive or a program.

What `run` does:

- **x86_64 ELF**: `box64 program`. Box64 loads x86_64 code but uses native
  aarch64 builds of the libraries it knows: glibc, libstdc++, GTK 2/3, SDL,
  OpenGL/EGL (Mesa/Panfrost), Vulkan, Wayland, X11 client libs, PulseAudio/ALSA,
  OpenSSL, freetype/fontconfig, and more. Other libraries must come with the
  app (AppImages and Electron apps bring theirs) or the program won't start.
- **AppImage**: its squashfs is mounted natively with squashfuse, which is
  faster than emulating the AppImage's own FUSE runtime, and `AppRun` is
  started. If squashfuse fails, it falls back on `--appimage-extract-and-run`.
- **Shell scripts** run in Box64's x86_64 bash (`box64-bash`). Without binfmt,
  the x86_64 programs a script starts then stay inside Box64. `BOX64_BASH` is
  set, so programs started by emulated programs are caught too.
- **Electron/Chromium** (a `chrome-sandbox`, `libffmpeg.so` or `resources/app.asar`
  beside the program) also get `--no-sandbox --ozone-platform=wayland`: the
  setuid sandbox can't work under emulation, and ztMETRO has no X server.
- The environment asks toolkits for Wayland (`GDK_BACKEND`, `QT_QPA_PLATFORM`,
  `SDL_VIDEODRIVER`, `ELECTRON_OZONE_PLATFORM_HINT`). ztOS has no Xwayland, so
  X11-only programs can't show a window.

Box64 settings (`BOX64_DYNAREC_*`, `BOX64_LOG=1` to debug) go in the environment
or in `~/.box64rc`. See the [Box64 usage docs](https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md).

## What works (Galaxy S9, ztOS, box64 0.4.4)

RESULTS

## Why Box64 (and not FEX-Emu or qemu-user)

CHOICE

## Kernel

KERNEL

## Limits

LIMITS
