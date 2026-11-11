---
name: ztos-flash-boot
description: Guarded procedure for writing a kernel boot image to the S9's BOOT partition and rebooting, with label, size and hash checks and a staged fallback. Use it only when the owner has approved BOOT flashing for the work at hand.
---

# Flashing a boot image (guarded)

Flash only when the owner has approved it for this specific work. DP Alt Mode and deep-sleep/Wi-Fi-during-sleep kernel work have been approved. Anything else: ask first. RECOVERY and EFS are never written.

## Steps
1. **Build** the image (see `scripts/build-boot.sh` and the work branch's notes). Record its size and sha256.
2. **Stage** it on the phone as `~/zt-test/boot-NAME.img`, and check its sha256 there.
3. **Keep a fallback.** The last known-good image stays staged, with its own flash script. Write down which one it is before flashing.
4. **Flash under the phone lock**, with every guard in one `set -e` command:
   ```sh
   ztlock run OWNER 'set -e
     [ "$(readlink -f /dev/disk/by-partlabel/BOOT)" = /dev/sda10 ]
     [ "$(stat -c %s ~/zt-test/boot-NAME.img)" = SIZE ]
     [ "$(sha256sum < ~/zt-test/boot-NAME.img | cut -c1-64)" = SHA ]
     sudo dd if=$HOME/zt-test/boot-NAME.img of=/dev/sda10 bs=1M conv=fsync status=none; sync
     sudo head -c SIZE /dev/sda10 | sha256sum'
   ```
   The read-back hash must match before you reboot. The working template is `flash-dp42.sh` in the coordinator's scratchpad, which writes one script per image.
5. **Reboot** with `sudo systemctl reboot`, never poweroff. Don't reboot between 05:00-07:00 or 22:00-02:00 CST (the owner's sleep and alarms), or after the owner has unplugged the phone for an overnight test.
6. **Verify** after boot: `uname -v` shows the new build number, `ztlock who`, the shell is up, sleepd has state, and `ls /dev/rtc0` succeeds. If it doesn't come back within about 5 minutes, report it. Once it can be reached, flash the fallback with the same guards.

## Notes
- The module set must match the kernel release (`7.1.0-zboot-wd-dpN`). If the config changed, install the modules before rebooting.
- On this phone the partition numbers are fixed (BOOT=sda10). The partlabel check stops the write if they ever change.
