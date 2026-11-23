# Upstream merge, 2026-11 (ROADMAP 8.46)

Branch `merge/upstream-2026-11`, from `origin/feature/desktop-dp` (2042d8b).
Merged, in order:

1. `origin/feature/camera` (fa48f4f): our camera work, not merged before.
2. Upstream `a12dfa6` on its own ("S9 layer: install the camera driver, its
   probe and udev rule"), reconciled with 1.
3. Upstream `main` at `ecb3e50` (ztOS 0.2.3-BETA-1.3): 40 commits past the
   merge base `aeb4749`.

Rule used where both sides changed the same behaviour: ours for anything in
ROADMAP section 8, upstream's for its new features. Nothing was deployed.

## 1. The camera

### feature/camera

One conflict: `scripts/build-bt-modules.sh`, where each side added a module
to `EXT_BT` (`sensors/hall:zrn-hall` here, `camera/driver:zrn-cam` there).
Both are kept.

### Upstream a12dfa6 against our on-demand design

Upstream's layer installed three things into the image: `zrn-cam-probe` in
`/usr/local/sbin`, `90-zrn-cam.rules` in `/etc/udev/rules.d`, and a copy of
`camera/driver/zrn-cam.ko` in `/var/lib/zrn-cam/`. Our design (feature/camera)
builds `zrn-cam.ko` into the module set (`scripts/build-bt-modules.sh`), which
goes to `/usr/lib/modules/<release>/extra`. Its `devname:zrn-cam` alias gives
`/dev/zrn-cam` (10:250) at boot through kmod-static-nodes, and the first open
loads the module. The udev rule is in `overlay/`, and `camera/driver/install.sh
--hold` blocks the load until approval.

| Upstream piece | Kept? | Why |
|---|---|---|
| `zrn-cam-probe` into `/usr/local/sbin` | yes | it never loads anything by itself, and its `KO` default is already `extra/` |
| `90-zrn-cam.rules` into `/etc/udev/rules.d` | no, already there | `overlay/etc/udev/rules.d/90-zrn-cam.rules` is the same file |
| `zrn-cam.ko` into `/var/lib/zrn-cam/` | no | nothing reads it there; the module comes with the module set. The layer now warns when no set has a `zrn-cam.ko` |
| (new) `/etc/modprobe.d/zrn-cam-hold.conf` | added | the image ships held: opening `/dev/zrn-cam` gets ENODEV and the Camera app shows its test scene until `camera/driver/install.sh --release`. `zrn-cam-probe` and `modprobe zrn-cam` by hand still work |

No other upstream camera fixes exist in the 40 commits: `camera/` and
`ztmetro/camera/` are untouched upstream. The only other mention is
`ztcamera` in a ztmetro.c list (see section 2).

## 2. The rest of upstream main

Sixteen files conflicted. Everything else merged by itself.

### Small conflicts

| File | Ours | Upstream | Resolution |
|---|---|---|---|
| `ztmetro/Makefile` | our apps and daemons (gallery, mms, servicemode, mirror, pods, ntfy, acct, clock, ledd, ztprofile), flathub/shots in the store, Settings with embedded pages | `csf/` renamed `coru/`, `passcoded/` (zrn-passcoded, zrn-passcode), `store/ztapps.c`, a Settings rule with `passcodepage.h` | all of both. One Settings rule (ours), with `settings/passcodepage.h` and `passcoded/passcode-proto.h` added to its dependencies. The coru rule keeps our `ztos-user.h` dependency |
| `ztmetro/coru/ztoscoru.c` (was `csf/ztoscsf.c`) | the control pipe made by `ctl_open()`: mode 0620, group of the phone's user; `CSF_CTL` overridable for the test | renamed to `/run/ztoscoru.ctl`, mode 0666, and `/run/ztoscsf.ctl` as a symlink to it for old shells | upstream's name and its old-name symlink, with our 0620 pipe. `CORU_CTL` and `OLD_CTL` can both be overridden; `sensors/test-csf-failure.c` now includes `coru/ztoscoru.c` and sets both |
| `installer/ztos_installer/build.py` | `check_host` through `host.py` (it already checks `xz`); Waydroid units out of the base system (19bfb5d) | `xz` added to the inline tool list; `ztoscoresensorframework` renamed `ztoscoru` | ours, with `ztoscoru` in the unit list. Waydroid stays out of the base units |
| `sdk/ztos_sdk/cli.py` | `find_uic()` with `QT_UIC` (Arch, Debian, Fedora paths) | the Debian path added inline (CI fix 9529e7e) | ours: it already has the Debian path |
| `.gitignore` | `ztmetro/csf/ztoscsf` | (none) | now `ztmetro/coru/ztoscoru` and the two passcoded binaries |

The rename to ztOSCOR-U also needs the phone migrated
(`scripts/dev/migrate-coru.sh`), see Risks.

### Two features both sides built: ours kept

Upstream 0.2.2 and 0.2.3 built **auto-rotation** and **proximity in calls**,
and we had built them too (ROADMAP 8.33 and the proximity work, both deployed).
The two versions talk to sensord differently, and only one of them can drive
the accelerometer. Ours is kept in both cases.

- **sensord** (`ztoscsf-sensord.c`):
  - Kept: our `orient on|off` and `orientation N` (orient.h), and our `call
    STATE ROUTE` from modemd with `prox_blank`.
  - Dropped: upstream's rotation engine (`rotation on|off|reload|log|record`,
    `rotation.conf`, `--rotation-selftest`). Also dropped: its `proximity
    near|far` messages. Deleted: `rootfs/overlay/etc/ztoscsf/rotation.conf`
    and `sensord/test-sensord-rotation.py`.
  - Taken from upstream: the hub time-sync throttle (at most once every 3 s),
    and `proxtest near|far|off`, mapped onto our reading.
  - `call on|off` is accepted and ignored. That form can't switch the sensor
    off during a call that the modem says is at the ear.
  - `docs/rotation/*.md` are upstream's design notes, left as they are; they
    don't describe what runs.
- **ztmetro.c, rotation**:
  - Our `rotation.h` (`server->rot`) turns the screen.
  - Upstream's view-frame rotation, cover-flow, split view and launch flip
    are about 2500 lines. They are still compiled but never switched on:
    - `rotation_enabled` is never set, and the `rotation` config key is
      ignored;
    - `orient N|auto` only logs (ours is `rotate 0..3|auto`);
    - `rot_prep_arm` returns at once, so no full-screen texture and no
      landscape wallpaper decode 4 s after boot;
    - `rot_tell_sensor` does nothing.
  - Upstream's `static rot_box` is renamed `rot_turn_box`, so that it no
    longer clashes with rotation.h. All of this can be removed later.
- **ztmetro.c, proximity**:
  - Kept: ours (`prox_blank`/`prox_override`, the power-key override,
    `touches_cancel_all`).
  - Removed: upstream's `call_update`/`call_app_set`/`prox_touch_blocked`, the
    ctl `proximity near|far` and `call-active APP 0|1`, and the cheek-touch
    guard.
  - Lost: Waydroid call proximity. Upstream's `perfd/tickle.c` sends
    `call-active waydroid 1`, and the shell now ignores it.

### ztmetro.c (81 hunks)

- **Status row**:
  - Kept: ours (burn-in wrapper, measured icon heights, airplane, location
    mark, Bluetooth, pods/headset battery, `zt_clock_text`, AOD battery).
  - Taken from upstream: the themed ink, `bars_lit()`, the halo and the
    separate lock-screen plug layer.
  - The signature is now `(..., themed, lock_ink)`. The `st_cell_put` boxes
    are fitted to our geometry, and ST_BT is added.
- **Repaint gating** (upstream's "repaint only when changed"):
  - `shade_tile_sig` includes our rotation and lock state, location, and the
    audio tile's output, so our quick tiles repaint when they change.
  - The status row is redrawn on every `draw_status`.
- **Lock and passcode**:
  - Upstream's `lock_open()` now also does what ours did inline:
    `lock_awake_unlocked`, `desk_phone_lock(false)`, `apps_suspend_sync`,
    `bt_pair_sync`, the idle re-arm and `activity()`.
  - The awake-why line is hit-tested first, then the passcode pad. The awake
    line is hidden while the pad is up.
- **Calls**:
  - Kept: upstream's call-screen flips, `calltest` and `call_end_begin` (with
    our `apps_suspend_sync`), and our `alarm_screen`.
  - A new `over_app` draws either the call or our alarm ring screen over the
    lock. The frosted backdrop is only for the call.
  - `ringing_key` (power silences, volume down rejects) runs first in our
    `key_event`, before `alarm_key`.
- **App sizing**: upstream's `area_*`/`view_*`/`app_orient`/`app_draw_fit`,
  with our per-app zoom, fullscreen and `app_top`.
- **Full-screen bars**: our `fs_status_y`/`fs_bars_t`, and the nav bar is
  skipped when hidden.
- **Our rotation in upstream's renderer**:
  - The scissors go through `zt_scissor`/`rot_box`, and `view_pw`/`view_ph`
    are logical while turned.
  - Planes and frosted bars are off while turned or with a full-screen app.
- **Volume banner**: ours (media rows, progress, audio modal). Upstream's
  light-theme banner and its "where it plays" row are dropped.
- **Keyboard**: our number pad, plus upstream's `osk_key_h`/`osk_top`/
  `osk_app_resizes`/`osk_publish` and its theme.
- **Notifications**: our zrn-therm tap and `notekey`/`noteclose`, plus
  upstream's `notifyx`/`notifyclose` and invoke. The log line is upstream's
  private one, plus our `led_notes_sync`.
- **`reload`**: ours (hub, desktop, zoom), plus upstream's saved `theme`.
- **`output_frame`**: upstream's ftrace line, then our AOD.

### Core (`zrnmetrocore.c/.h`)

- **Upstream added**:
  - a light/dark theme (`zmc_fg`/`zmc_bg`);
  - landscape sizes;
  - a deferred resize (`zmc_apply_resize`, `.resized`);
  - board fitting, `page_solid` and `ZTMETRO_NO_ENTRANCE`.
- **We had**: replaced `zmc_pivot_next` with the tab wheel
  (`zmc_pivot_wheel`), and added the desktop environment.
- **Both kept**:
  - The wheel and `zmc_header` share upstream's landscape title positions and
    ink. `struct zmc_wheel` has a `size`, so titles are measured again after
    a turn.
  - Upstream's deferred resize replaces our `configure_resized`. A slide in
    progress lands, the tab pictures are dropped, and `swipe_caught` is
    cleared.
  - `size_landscape()` is false on the XFCE desktop, so a wide desktop window
    keeps the phone header.
- **Follow-on edits**:
  - Ported onto the wheel: `zroovmusic/ztzroovmusic.c` and
    `whatsapp/ui/ztwhatsapp.cpp`, since `zmc_pivot_next`/`pivot_page` are gone.
  - `whatsapp/ui/Makefile` gained the two extra core protocol sources.
  - `tests/pivot-wheel.c` calls `zmc_apply_resize()` after a configure.

### Settings, Store, Sound, DevTest

- **`settings/ztsettings.c`**:
  - **Page tree**: our tree (`page-tree.h`) owns the pages. `PAGE_PASSCODE`,
    `PAGE_PASSCODE_ENTRY` and `PAGE_PASSCODE_KEY` hang under Lock Screen, and
    `tests/settings-tree.c` checks them.
  - **Passcode hooks**: upstream's hooks run inside our `activate`, `back`,
    `arrived`, `render_to_file` and `typed`.
  - **Light theme and frost**: these become two switches on Customization, and
    `theme`/`frost` are in our `config_format()`.
  - **Power page**: ours, with upstream's zrn-sleepd switch and status appended.
  - **Dropped**: upstream's flat main hub and its "Wake Waydroid on push"
    switch. That switch ran `sudo zrn-sleepd wowlan on|off`. Neither side's
    zrn-sleepd has a `wowlan` command, and an unknown argument starts the
    daemon, so the switch would have started a second root sleepd.
- **`store/ztstore.c`**:
  - **ztOS apps source**: added (upstream's `P_ZAPPS`/`P_ZAPP`/`*_MORE`)
    beside our Flathub and screenshot pages. The tabs are top, categories,
    installed and ztOS apps, on our wheel. The store opens on ztOS apps
    (upstream's choice).
  - **Lists**: upstream's zmc_list rewrite, painted on the core's thread, now
    carries our Flathub category rows. The Flathub catalogue swap takes
    `zmc_list_lock` (the rows hold `struct pkg` pointers).
  - **Details**: Flathub keeps our single detail page. Other packages get
    upstream's redesign and its "more" tab, plus our "Install it now".
- **`sound/ztsound.c` and `devtest/zrndevtest.c`**:
  - Ours had moved their pages into `sound-pages.c`/`devtest-pages.c` (embedded
    in Settings).
  - Upstream's changes are ported there: landscape plot sizes, theme greys,
    rebuild on width change, and `zmc_header_h()`.

### Daemons and apps

- **`modemd/ztoscorenetworkingd.c`**:
  - Kept: our data-at-start, ServiceMode idle timer and MMS.
  - Added: upstream's "mobile data held off while Wi-Fi is connected".
  - Fixed one interaction: upstream's hold tore the context down on Wi-Fi,
    which breaks MMS. Now the hold only sets `data_held`, and the context
    goes down once no MMS needs it.
  - Upstream's "calls no longer ignored after an answered one" merged by
    itself.
- **`notifyd/ztnotifyd.c`**:
  - A notification with a default action and no progress or sync key goes as
    upstream's `notifyx`. Everything else goes as our `notekey`.
  - Close sends both forms.
  - The log line is upstream's private version.
- **`phone/ztphone.c`**:
  - Upstream's emergency pad and `call_flip_overlay`, on our wheel.
  - Upstream's `.arrived = tab_arrived` is dropped, since the wheel does it.
- **`messages/ztmessages.c`**: upstream's themed compose strip, with our
  attachment row.

## 3. Upstream's sleep work against ours

Upstream changed `power/suspend/platform.c` (the `zrn-sleep-init` module) and
`zrn-sleepd`. We hadn't touched `platform.c`, so it merged by itself. Our
`zrn-sleepd` changes (sleep-why, ShellWatch, recovery, alarms) merged with
upstream's one addition there.

**What upstream added, in `platform.c`:**

| Change | Default | What it does |
|---|---|---|
| CP MIF settle (583846c) | **on**, `cp_settle_ms=300` | before the SMC, polls MIF request bit 0 (CP) and waits up to 300 ms (500 us steps) for the modem to drop it after `pda_active:0`. Logged as `cp_wait=` on the handoff line. Readable as `cp_settle_waited_us` |
| GRP4 trace | always | `g4_trace[4]` and `g4_relatch`: GRP4 read at three points in prepare, then up to 4000 x 1 us polls after the clear, to see when it latches again |
| freeze guard | off (`wdt_apm_guard_s=0`) | arms WDT_APM, with reset routed, just before the SMC and disarms it after. A hang inside ACPM's sleep then resets the phone |
| `gicc_off` | off | GIC CPU interface off across the GRP4 clear and SMC |
| analog EINT filter | off (`eint_filter_analog`) | gpa0-4 FLTCON set to enable+analog for the sleep, as stock does; restored on resume |
| `pd_log` | off | a second handoff line with the PMU domain status and FLTCON words |

Also a new tool, `power/suspend/pmic-rtc-alarm.py`. It arms or disarms the
S2MPS18 RTC alarm 0 and INT2M through `s9p-acpm`, which means **PMIC register
writes**. It is not installed by the layer.

**In zrn-sleepd**: `whatsapp_active()` (a linked ztwhatsapp account) now
counts as dozing, so sleeps are capped at `DOZE_SLEEP_S` as for push.

**How they interact with ours:**

- **The module is rebuilt now.** The image's `zrn-sleep-init.ko` comes from
  the module set (`build-bt-modules.sh` builds `power/suspend:zrn-sleep-init`),
  so the next set built from this branch carries upstream's code. Only the CP
  settle wait and the GRP4 trace are on by default.
  `prebuilts/power/zrn-sleep-init.ko` is unchanged until someone rebuilds it.
- **The CP settle wait comes before `grp4_at_smc`.** With the trace poll, the
  handoff is up to about 304 ms later when the CP holds MIF. Our `fw-abort`
  classification in zrn-sleepd reads `grp4_at_smc` (taken after the console
  line), so it still sees what ACPM will find. A late GRP4 latch is now more
  likely to be caught, and called fw-abort, rather than missed. That is the
  intent, but the fw-abort rate in `soc.csv` may move.
- **It's the modem half of our MIF work.** Our `mif_requests_before` diagnosis
  (`secure-handoff-audit.md`) saw CP or AP requests left at the SMC; this wait
  removes the CP case. Expect fewer sleeps that stay MIF-up with the modem
  registered.
- **Busy-waits.** The wait runs with one CPU online and interrupts off, so a
  CP that never drops its request costs 300 ms of busy-wait per sleep.
  zrn-sleepd's own timings (`sleep-why`, the cost scripts) will show it.
- **The freeze guard overlaps our watchdog work.** Our own work is the
  cluster-0 WDT kept disabled and the `watchdog-test` routing. Upstream's
  guard is off by default. Turning it on resets the phone on any ACPM hang,
  which also loses the console record we rely on. Leave it off until the owner
  chooses it.
- **The EINT filter and `gicc_off`** are trials that touch wake-up behaviour.
  They're off. They would change the `eint_effective`/GRP4 evidence our
  recovery tests are written against.
- **WhatsApp doze**: harmless when no account is linked (the status file is
  missing).
- **`pmic-rtc-alarm.py`**: it writes PMIC registers. Don't run it without the
  owner's approval.

Tests: `power/suspend/test-sleepd-recovery.py`, `test-sleep-why.py` and
`test-shell-watch.py` pass on the merged daemon.

## 4. The fingerprint port

`582c62c` adds `fingerprint/`. Nothing in the image uses it: the layer doesn't
install it, no DT includes `starlte-fingerprint.dtsi`, it isn't in
`build-bt-modules.sh` or `pick-modules.sh`, and `CONFIG_TEE` is off.
Upstream's README says it was built and never loaded.

**What it is.** The route is Samsung's own stack, unchanged, in the Android
chroot (`/run/android`): the fingerprint HAL, `libbauthserver`, `libMcClient`,
`mcDriverDaemon` and the Egistec trustlet (`...002e.tlbin`) inside Kinibi
(Trustonic t-base 400C). ztOS supplies two modules:

- **`zrn-mobicore.ko`**: Samsung's GPL `gud` MobiCore driver, ported to 7.1.
  - It creates `/dev/mobicore` and `/dev/mobicore-user`, and the notify IRQ is
    GIC SPI 233.
  - With `MC_DELAYED_TEE_START`, insmod alone makes no SMC. The first open by
    `mcDriverDaemon`, or `echo 1 > /sys/module/zrn_mobicore/parameters/start`,
    makes the first world switches: `MC_FC_MEM_TRACE`, `MC_FC_INIT` with the MCI
    buffer, then N_SIQ.
  - **So yes: it brings up Kinibi, the TEE, from Linux.**
  - Stock's SWAP_CPU at probe is removed.
- **`zrn-etspi.ko`**: a compact secure-mode `et5xx-spi`.
  - It provides `/dev/esfp0`, and drives the LDO (gpm43-0), SLEEP/RESET
    (PCAL6524 P0.6, the same expander as zrn-bt's BT_EN) and DRDY on gpa1-3.
  - It makes EL3 SiP calls: `CS_SET` 0x83000027 at power-off, and
    `PM_SUSPEND`/`PM_SUSPEND_CS_HIGH` at suspend once powered.
  - It also writes the gpp0 pin registers directly.
- **A DT fragment, not applied**: it has no `&spi_1` node, because SPI1 is
  `secure-mode` and the TA owns it.
- **A spec for `zrn-fingerprintd`**: D-Bus `net.ztos.Fingerprint1`, with the
  templates in the zrn-passcoded LUKS vault.

**Against our study** (`camera/iris/FINGERPRINT.md` on `research/iris`, 8.22).
The hardware facts agree: ET510, SPI1 secure from boot, gpa1-3, gpm43-0,
PCAL6524 pin 6, and the same SiP numbers. We judged path (a), capture from
Linux, impossible. We also judged path (b), the stock TEE path from Linux,
"not feasible", because it needs exactly what upstream has now done: the
MobiCore driver on 7.1, Kinibi started from Linux, the signed TA, bauth and the
HAL. Upstream has built (b)'s kernel half, but it remains unproven:

- whether Kinibi, as sboot leaves it, accepts `MC_FC_INIT` from our kernel
  (their H1, H2);
- whether the TA or bauth demand Samsung's property, Knox or boot state (U1, R2);
- whether the SPI1 clocks are on (U3).

**What it would touch, if enabled:**

- SMCs into the secure world: fastcalls, SIQs and SiP calls. Their worst case
  (H2) is a hard lock of CPU0 in the secure world, which is recovered only by
  the 30 s watchdog or a forced restart.
- **EFS**: `mcDriverDaemon` wants `/efs/TEE`. The plan bind-mounts it
  **read-only** and copies `TEE/00000000.authtokcont` first. Under our rule
  (our code never writes EFS) this has to stay read-only. Samsung's daemon may
  try to write it.
- The PCAL6524 read-modify-write race with zrn-bt (R6), and gpp0 writes next
  to pinctrl (R5).
- Sleep with the TEE up is untested (H6). Our SYS_SLEEP path has no Trustonic
  handoff (`power/suspend/secure-handoff-audit.md`). Never rmmod it; only a
  reboot undoes it (H7).

**The decision for the owner.** Running Kinibi and Samsung's secure-world code
from our kernel is the step. P2b of upstream's test plan is the first SMC.
Merging changes nothing on the phone: the code sits in `fingerprint/`, unbuilt
by our scripts. Its `build.sh` points at upstream's own workstation paths.
Nothing fingerprint-related was built, loaded or enabled here.

## 5. Build and tests

On the host, in `localhost/ztos-desktop-dev`. A podman mount with `:z`, not `:Z`,
lets several builds share the tree.

- `make -j8` in `ztmetro/` (every target in `all`, including `ztmetro`,
  `coru/ztoscoru` and `passcoded/*`) builds with no errors. The warnings are
  format-truncation ones, plus a few unused variables that were already there.
  Also built: `zroovmusic/ztzroovmusic` and `make -C whatsapp/ui`.
- **check targets on both sides**: check-store (now also flathub),
  check-arch, check-keyboard, check-launch, check-auth and check-demoloop-stop
  all pass.
- **Our check targets**: all pass except three:
  - `check-bt-pair-sheet` fails the same way before the merge: the
    `wlr-layer-shell` protocol header isn't on its include path.
  - `check-podsd` and `check-ntfyd` fail because the container has no python
    `dbus`. That is the environment, not the code.
  - The passing ones include check-csf, which now runs against
    `coru/ztoscoru.c`; also check-sensord, check-rotation, check-notify-daemon,
    check-mms, check-servicemode, check-settings-tree, check-settings-config,
    check-pivot-wheel, check-swipe-cost, check-volume-streams,
    check-audiod-live and check-lock-awake.
- **Python**: these pass:
  - `devices/samsung-starlte/test-zrn-thermald.py`;
  - `power/suspend/test-shell-watch.py`, `test-sleepd-recovery.py`,
    `test-sleep-why.py` and `watchdog-test/test-routing.py`;
  - `sensors/gnss/test-gnss-gate.py`, `test-gnss-stats.py`,
    `sensors/hrm/zrnhrmd/test_ppg.py` and `sensors/test-android-root.py`.
- **Python, failing before the merge too**, unchanged:
  - `power/idle/test-mct-deadline.py`: the test's stubs lack `mct_trace` and
    `pr_info`.
  - `power/suspend/test-acpm-tool.py`: it needs `third_party/s9plus`, which
    isn't in this checkout.
  - `sensors/test-sensorhub-watch.py`: it reads
    `rootfs/overlay/.../ztoscsf-sensorhub`, which now lives under
    `devices/samsung-starlte/overlay`.
- The tests that only run on the phone weren't run: `suspend-test.py`,
  `run_deep_sleep_test.py`, `shell-watch-device-test.py`,
  `ztclock-alarm-test.py` and `ufs-idle-test.py`.

## 6. Risks for the deploy

1. **The ztOSCOR-U rename needs a migration on the phone.** The phone runs
   `ztoscoresensorframework.service` and `/usr/local/sbin/ztoscsf`. Run
   `scripts/dev/migrate-coru.sh` with the deploy. It moves the marks in
   `/var/lib/ztoscsf` and the components list, and swaps the units.
   `ztoscoru` falls back to the old marks and list if they haven't moved, and
   links `/run/ztoscsf.ctl` to its pipe for an old shell. The pipe is 0620,
   owned by the phone user's group (ours), not upstream's 0666.
2. **A new root daemon**, `zrn-passcoded` (LUKS2 vault, cryptsetup). It is off
   until a passcode is set, and the lock screen fails open to a swipe if it is
   down. The pad and the awake-why line share the lock screen; check both
   together.
3. **The new deferred resize**:
   - The core's resize path is now upstream's. Check XFCE desktop windows on
     DP-1 and our rotation turned, with full-screen apps and the sliding bars.
   - Upstream's quad edge smoothing uses logical sizes while turned.
4. **The light theme is new** in every app. The volume banner and a few of our
   drawings (BT row fill, store skeleton, log boxes) stay dark.
5. **Calls**:
   - The alarm ring screen over the lock now draws through upstream's call
     overlay path.
   - Power silences and volume down rejects a ringing call; check that this
     doesn't fight our alarm keys.
   - Waydroid call proximity is gone.
6. **MMS on Wi-Fi with upstream's new Wi-Fi data hold** is untested.
7. **The App Store** opens on ztOS apps and fetches the signed index at launch.
   Its list rows are drawn on the core's thread; any package-freeing path that
   misses `zmc_list_lock` would be a use-after-free.
8. **The sleep module** gets upstream's CP settle wait (on by default) when
   the module set is rebuilt; see section 3. Watch `cp_wait=` and the
   fw-abort rate in the first sleeps.
9. **The camera** ships held: the Camera app shows its test scene until
   `camera/driver/install.sh --release`. The image needs a module set with
   `zrn-cam.ko` (the layer warns if there isn't one).
10. **Upstream's dormant rotation code** (about 2500 lines in ztmetro.c) is
    compiled in. It's inert, but adds size.
11. **Fingerprint and `pmic-rtc-alarm.py`** are in the tree. Neither is built
    or installed; both need the owner's decision.
