---
name: ztos-phone
description: How to reach and work on the ztOS phone (Galaxy S9, starlte) safely, covering SSH, the shared phone lock, sudo, backups, shell restarts and the owner's hard limits. Use it before running any command on the phone.
---

# Working on the ztOS phone

## Connect
```sh
P='ssh -i ~/.ssh/ztos_phone -o IdentitiesOnly=yes -o UserKnownHostsFile=~/.ssh/known_hosts_ztos -o HostKeyAlias=172.16.42.1 -o ConnectTimeout=15 samuel@ztos'
$P 'uname -v; uptime'
```
- `ztos` is the Tailscale name. If it fails, use the LAN IPv6 `fddc:a166:d566:20:b405:4312:6b77:cca7` (Tailscale reaches sshd as 127.0.0.1, and sshd's PerSourcePenalties sometimes lock that address out). USB is `172.16.42.1`.
- Repo helpers (`scripts/phone-do.sh`, `scripts/phone-lib.sh`) work with `ZT_SSH='-i ~/.ssh/ztos_phone samuel@ztos'`.
- A sleeping phone turns Wi-Fi off, so SSH drops. `sudo zrn-sleepd status` shows why the phone is awake or asleep.

## Share the phone
Several agents use the phone at once. Run anything that disturbs it inside the lock: installs, service or shell restarts, screen on/off, power measurements, reboots.
```sh
$P "ztlock run MY-BRANCH 'cmd'"   # waits up to 30 min
$P 'ztlock who'
```
Keep each locked section short. Reads don't need the lock.

## Hard limits (the owner's rules)
- **Never write EFS, RECOVERY or BOOT.** EFS may be read, for example `bluetooth/bt_addr`. Never read or copy `stock/EFS.img`. Samsung's own gatekeeper writing EFS is fine; our code writing it is not. BOOT flashes are allowed only where the owner approved them; see `ztos-flash-boot`.
- **Register writes** to the MAX77705 (I2C, USB-C, charger, haptics) or raw SoC registers (devmem) need the owner's approval for each opcode or register. The approved list is in the coordinator's memory (`usbc-chip-writes.md`). If you aren't sure, stop and report the exact register and value.
- **Never `pkill -u samuel`**, and never kill the session broadly. Restart the shell only through greetd: `sudo /usr/local/sbin/zboot-session ztmetro`.
- **No Bluetooth activity that affects other devices.** The owner's AirPods are paired, so no connect, pair, scan or power toggles without approval.
- **Avoid shutting down.** The owner has to power it back on by hand. Reboots are fine.
- Use sudo with care, and no broad `rm`.
- Don't change the owner's alarms (`ztclockd`), and don't leave sleep disabled or inhibited.

## Before replacing anything
Back up the binary or config next to the original as `NAME.pre-<branch>`, and say in your report how to restore it.

## Useful places
| What | Where |
|---|---|
| Sleep log, battery per wake | `sudo zrn-sleepd log 100`, `/var/lib/zrn-sleep/soc.csv` |
| Battery | `/sys/class/power_supply/max170xx_battery/{capacity,current_now,current_avg}` (mA, negative = discharging) |
| Charger | `/sys/class/power_supply/max77705-charger/online` |
| Shell control | `$XDG_RUNTIME_DIR/ztmetro.ctl` (desktop commands: `ztmetro/desktop/README.md`) |
| Screenshot | `grim` in the session (`XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0`) |
| Battery accountant | `ztacct`, `/etc/ztacct/power_profile` |
