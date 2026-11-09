# App Store (ztstore)

Apps from the repos in `pacman.conf`, from the AUR and from Flathub, in
ztMETRO's look, built on ZrnMetroCore.

**top** lists what is popular and new, **categories** browses AppStream
categories, pacman groups and Flathub's categories, and **installed** shows
updates and your apps. There is also search across all three sources, a
details page, and installing, removing and building from the AUR.

## Nothing is written into it

| What | Where it comes from |
|---|---|
| repos, and their order | `/etc/pacman.conf` |
| packages, versions, sizes, groups | pacman's sync and local databases, read directly |
| app names, summaries, icons, categories | AppStream (`archlinux-appstream-data`), else the app's `.desktop` file |
| the lists on **top** | `/etc/ztstore/sections` (or `~/.config/ztstore/sections`) |
| AUR lists | the AUR's daily metadata dump, cached for a day |
| AUR search and details | the AUR RPC (v5) |
| "popular in your repos" | pkgstats.archlinux.org, cached for a day |
| updates from the repos | `ztos-update`, the system updater, as the Updates app does |
| Flathub's apps, and which are built for this phone | flatpak's own AppStream for aarch64 (below) |
| Flathub's lists (picks, popular, new) | flathub.org's API (`/api/v2/collection/…`, `/app-picks/…`), cached for 12 hours |
| a Flathub app's sizes and permissions | `flatpak remote-info` (and `-m`, its manifest) |
| Flathub apps installed, and their updates | `flatpak list`, `flatpak remote-ls --updates` |

## Flathub

**Which apps.** The catalogue is the one flatpak itself keeps for the
`flathub` remote and this machine's arch:
`/var/lib/flatpak/appstream/flathub/aarch64/active/appstream.xml`. It lists
exactly the apps that have an aarch64 build, so nothing without one is ever
offered. The App Store refreshes it with `flatpak update --appstream flathub`
(as the person: flatpak's polkit policy lets the active session do that
without a password) when it is older than 12 hours. Reading those 45 MB of
XML takes two seconds on the phone, so what the lists need of it is kept in
`~/.cache/ztstore/flathub-index` (1 MB, read in ~25 ms) until the catalogue
changes.

**Search** looks through the catalogue (names, summaries, AppStream
keywords). It also asks flathub.org's search for the same words, and names
the apps Flathub has only for other processors: *"Spotify · x86_64 only"*,
under the results, not offered.

**Top**: `flathub` lists in the sections file (`picks`, `popular`,
`trending`, `recently-added`, `recently-updated`, `verified`, and `mobile`,
the popular ones that fit a phone). The API's lists cover all arches; only
the apps in this phone's catalogue are kept.

**Details**: every screenshot (two across; a tap shows them large, as tabs to
swipe through, with their captions), the whole description (paragraphs and
lists, as text), what is new in the last releases, the permissions from the
app's manifest in plain words (broad ones marked ⚠: the home folder, all
files, every device, X11 without Wayland, the session bus, `org.freedesktop.Flatpak`
and so on), download and installed size, the runtime and whether it is
already on the phone (and its size if not), developer, licence, content
rating (OARS), and the links (website, bug tracker, donations, help,
source).

**Screenshots** are fetched only when a details page shows them, on a
thread, as the ~750 px wide images Flathub offers (not the 1920 px
originals), shrunk to at most 1080 px, and kept as PNGs in
`~/.cache/ztstore/shots/`, at most 40 MB (the least recently seen go first).
Six at most are kept decoded in memory.

**Mobile data**: when NetworkManager says the connection is metered, nothing
is refreshed that is already there (the catalogue, the lists, the AUR dump,
pkgstats), and screenshots wait for a tap on "show screenshots".

**Installing is system-wide** (`flatpak --system`), through the same root
service as the repos (`ztstore-install@USER`, no password). The `flathub`
remote, its runtimes and the apps already on the phone are system-wide, the
shell indexes `/var/lib/flatpak/exports`, and a second, per-user copy of
the runtimes would cost hundreds of MB. Flathub's commits are GPG-signed and
flatpak checks them, as pacman checks the repos' packages. Removing and
updating go the same way (removing also takes away the runtimes no app uses
any more, `flatpak uninstall --unused`). The progress page shows flatpak's
percentage, and "cancel" asks the service to stop flatpak (SIGINT: what was
pulled stays in its repo, nothing half-deployed).

**Adding Flathub**, where it is missing: the top page asks first, then the
service runs `flatpak remote-add --if-not-exists flathub
/usr/local/share/ztstore/flathub.flatpakrepo`, the official file (with
Flathub's GPG key) kept in the system overlay.

**Updates**: the installed page lists Flathub apps with an update beside the
system's; each updates on its own ("update" on its page, or "update them all"),
apart from the whole-system pacman update, which stays as it is.

**In the app list**: `ztapps` reads the exported `.desktop` files from
`/var/lib/flatpak/exports/share/applications` (and the per-user one) and
their icons from the export folders; their `Exec=flatpak run …` lines start
as they are.

## How installing works

- **From the repos**: `ztstore-install@USER.service` runs `pacman -S` as root.
  There is no password, because the packages are signed (`50-ztstore.rules`).
- **From Flathub**: the same service runs `flatpak --system install` (above).
- **From the AUR**: `ztstore-aur` fetches the recipe and checks it is made for
  this machine. Then:
  1. What it needs from the repos goes in first.
  2. It is built as you, with `zrnperfd` letting the build use the big cores,
     and packed with zstd on all cores (not the system's single-core xz).
  3. The built package goes in through `ztstore-admin@USER.service`, which asks
     your password on ztMETRO's password sheet (`ztos-polkit-agent`). If the
     sheet is missed (the phone locked during the build), the package is kept:
     "install it now" asks again without building it again.
- **Removing** packages from the repos or the AUR goes through the admin
  service, and asks for the password.

The system side lives in `os/overlay`, as the updater's does: `ztstore-helper`,
`ztstore-aur` (`/usr/local/libexec/ztstore/`), the two services, the polkit rule
(local, active session, wheel only), and Flathub's `.flatpakrepo`
(`/usr/local/share/ztstore/`). `make install` puts `/etc/ztstore/sections` in place.

The app writes its request to `$XDG_RUNTIME_DIR/ztstore/request` (and, to stop
a flatpak download, the request's id to `$XDG_RUNTIME_DIR/ztstore/cancel`).
The services answer in `/run/ztstore/USER/status` and `log`.

## Building and checking

    make store/ztstore     # on the phone, with the rest of ztmetro
    make check-store       # the parsers, against a made-up pacman layout and
                           # a Flathub catalogue (tests/fixtures)
    ztstore --render top out.png          # a page as a PNG, no display needed
    ztstore --render detail out.png mpv
    ztstore --render detail out.png org.xfce.mousepad   # a Flathub app, screenshots fetched
    ztstore --render shot out.png org.xfce.mousepad 1   # its second screenshot, large
    ZTSTORE_FILTER=3 ztstore --render search out.png calculator   # Flathub's results

`ZTSTORE_SECTIONS`, `ZTSTORE_FLATHUB` (a catalogue folder), `ZTSTORE_PACMAN_CONF`
and `ZTSTORE_APPSTREAM` point it at other files, to try them out.

## Known limits

- An AUR package that needs other AUR packages stops and says which ones. You
  install those first; it doesn't resolve AUR chains itself.
- The AUR's lists can't filter by architecture: the dump doesn't say. That is
  checked when you install, from the recipe's `.SRCINFO`.
- AUR packages have no icons until they are installed.
- Updates from the repos are whole-system, through `ztos-update`. Arch has no
  partial upgrades.
- Flathub: X11-only apps don't open (the phone runs no Xwayland); the details
  page says so. File pickers and other portals go through
  `xdg-desktop-portal-gtk`, the one portal backend on the phone.
- Flathub's permissions come from `flatpak remote-info -m`, which asks the
  network; without one the details page says it could not tell.
- More than eight screenshots are shown large eight at a time ("the next
  screenshots"): the core's pivots have at most eight tabs.
