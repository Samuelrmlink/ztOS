---
name: ztos-deploy
description: Build ztmetro (the phone shell and its apps) or other userspace pieces and install them on the ztOS phone with backups and a restore path, then restart the shell safely through greetd. Use it when testing a shell or app change on the device.
---

# Deploying userspace to the phone

## ztmetro (the shell and Metro apps)
ztmetro builds **on the phone**, since it needs the phone's wlroots 0.20 and GLES.
```sh
ZT_SSH='-i ~/.ssh/ztos_phone -o IdentitiesOnly=yes -o UserKnownHostsFile=~/.ssh/known_hosts_ztos -o HostKeyAlias=172.16.42.1 samuel@ztos' \
  HOST=<this PC's address the phone can reach> scripts/ztmetro-push.sh
```
The push script sends `ztmetro/` over a socat port (5581), runs `make` and `make install`. If the phone isn't on USB (172.16.42.x), copying a tarball over SSH is simpler and works anywhere: `tar c -C ztmetro --exclude=ref . | $P 'mkdir -p ~/ztm-BRANCH && tar x -C ~/ztm-BRANCH'`, then build in that directory. Build in **your own** directory, never the shared `~/ztmetro`, because other agents deploy at the same time.

## Install safely
1. Back up first: `sudo cp -a /usr/local/bin/X /usr/local/bin/X.pre-BRANCH`. Install only the binaries you changed.
2. Install and restart inside the lock: `ztlock run BRANCH '...'`.
3. Restart the shell only through greetd: `sudo /usr/local/sbin/zboot-session ztmetro`. Never `pkill -u samuel`. A shell restart closes the owner's open apps, so do it only when you need to.
4. Check it came back: the shell process is up, and a `grim` screenshot looks right.
5. Write the restore command into your report, for example `sudo cp -a X.pre-BRANCH X` followed by a greetd restart.

## Tests
- Host tests: `make check-*` targets in `ztmetro/Makefile`. Run the ones near your change.
- Headless desktop mode: `ztmetro/desktop/dev` (a container).
- Rendering without the phone: `scripts/ztmetro-render.sh`.
