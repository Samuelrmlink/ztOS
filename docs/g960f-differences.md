# G960F against G960N

What differs between the UK Galaxy S9 (SM-G960F) ztOS is developed on and the Korean SM-G960N it was
first built on, for roadmap items 2.1 to 2.7 and 3.0a3. Read on 2026-11-07, read-only.

**Verified** = read on the phone, in its VENDOR partition, or in code. **Inferred** = a reading of that
evidence, not tested. No G960N was available, so every statement about the N comes from this repository,
and N compatibility is by code review only. Values from `stock/` are not reproduced here: where a stock
image was the evidence, only the conclusion is given.

## Summary

| Item | Result | Evidence |
|---|---|---|
| 2.1 Variant | `ztos-variant`: factory record in EFS, then VENDOR, then SYSTEM. Says `SM-G960F` on this phone. | Verified |
| 2.2 Model | Settings reads the detected model. The device tree now names no letter (needs a boot image). | Verified offline; not installed |
| 2.3 Wi-Fi data | The built-in calibration is the wrong one for the F, and the F's right file is identified. Firmware and CLM match. `ccode=KR` only lasts until the first association. | Verified |
| 2.4 Modem values | `hw_rev` and `system_rev` fit the F. The pin list matches. `0xae` itself is not read on the F yet. | Partly verified |
| 2.5 Hub patch | The built-in patch is not the one in the F's stock kernel. Effect unknown. | Verified difference |
| 2.6 Partitions | `sda10`, `sda11`, `sda25` are BOOT, RECOVERY, USERDATA. Scripts now check. | Verified |
| 2.7 SIM, bands | This phone is single SIM. Bands are public lists only. | SIM verified; bands not |
| 3.0a3 Bluetooth address | It is in EFS at `bluetooth/bt_addr`, and in use. The roadmap entry was wrong. | Verified |

## 2.1 Variant detection

Nothing in the kernel can tell: the bootloader's command line and device tree do not reach us (`/proc/cmdline`
and the running tree are ours). Candidates, all readable without changing anything:

| Signal | On this phone | Survives a reflash? | Who can read it |
|---|---|---|---|
| EFS `imei/prodcode.dat`, first 8 characters | `SM-G960F` | Yes: factory-era file | Any user, once EFS is mounted |
| VENDOR `ro.product.vendor.model` | `SM-G960F` | No: says what was flashed | Root (mode 600) |
| SYSTEM `ro.product.system.model` | `SM-G960F` | No | Root |

`devices/samsung-starlte/overlay/usr/local/sbin/ztos-variant` tries them in that order. Only the model prefix
is read out, because the rest of the product code is not needed. A value that isn't `SM-G960` plus one
character falls through to the next source, and if none answers it prints `unknown` and exits 1.

- **Inferred:** a G960N has `prodcode.dat` in the same form. If not, the fall-through reaches its VENDOR.
- `ztos-variant.service` publishes the answer to `/run/ztos-variant`, the only thing it writes. It waits up to
  60 s for EFS, which the modem service mounts after boot.
- Tests: `devices/samsung-starlte/test-ztos-variant.sh`. On the phone, the script run over stdin (nothing
  installed) printed `SM-G960F`, as user and as root.

## 2.2 Reported model

- Settings' About page now shows `/run/ztos-variant`. Without it, it shows `SM-G960` and no letter. On an N with
  detection working, it reads exactly as before. The helper was compiled and run alone; the whole of `ztsettings.c`
  could not be built on this PC (missing libraries).
- `kernel/make-dts.py` now writes `Samsung Galaxy S9 (SM-G960)`, because one tree serves both. The only reader of
  the tree's model in this repository is a log line in `initramfs/init`. **This needs a new boot image.**
- Left alone: `android/waydroid/waydroid.cfg` still names the N (see decisions).

## 2.3 Wi-Fi data

| File | Against this phone's VENDOR (G960FXXUHFVG4) |
|---|---|
| `firmware/bcmdhd_sta.bin_b2` | Identical (sha256) |
| `firmware/bcmdhd_clm.blob` | Identical (sha256) |
| `firmware/nvram.txt_r02a_b2` | **Different**: the Korean tuning of the same module |

**The right file is known (verified).** The factory record `FactoryApp/HwPartInform` names this phone's Wi-Fi
module `semco_sem_r02a_e32a_b2`, so it uses VENDOR's `etc/wifi/nvram.txt_r02a_b2` (sha256 `968b71eb...6692`).
VENDOR also carries `r02b` (Skyworks) and `murata_r041` for other modules; those are not for this phone, and
another G960F may need one of them.

What differs in the file built in now:

- header `for STAR1 KR` and `ccode=KR`; the F's has `ccode=0`
- higher maximum power: `maxp2ga` 79 against the F's 77, and `maxp5ga` 74 in all sub-bands against 71 to 75
- different 2.4 GHz PA calibration (`pa2ga*`), power offsets per rate, and per-channel offsets
- different SAR limits (`dynamicsarctrl*`, `rsdb_dynamicsarctrl*`)
- channel 12 and 13 limits and the OCL lines active, where the F's file has them commented out

So on an F today the radio runs on another market's calibration and SAR table. **Inferred:** this may affect
range and is a compliance concern. No failure has been tied to it.

**`ccode=KR` is short-lived (verified).** The regulatory domain is set from the access point on each association
(`CTRL-EVENT-REGDOM-CHANGE init=COUNTRY_IE`), and `brcmf_cfg80211_reg_notifier` passes it to the firmware, which
logged no rejection. KR therefore applies only from power-on until the first association. **Inferred:** the
channel table here (12 and 13 disabled; 144 and 149 to 165 enabled) is the US one and not KR's.

**Do not take the regulatory domain from the region code.** This UK phone is on a network whose access point says
US, and its EFS sales code says XEU. A domain derived from the code would be wrong here. Following the access
point is right, and needs no code.

**Code:** `scripts/build-boot.sh --wifi-nvram FILE` chooses the calibration; the default stays the current file, so
an N build is unchanged. The driver loads Wi-Fi at 0.47 s, before the root filesystem exists, so **changing it
needs a new boot image**. The F's file is not committed: it comes from the phone's VENDOR at build time.

## 1.7 and 3.7: do they connect to the Wi-Fi data?

**1.7 (going silent): no link established.** **3.7 (P2P): no link established, and none is suggested.** What was
found is below, and much of it changes how those two items should be read.

**`-52` is not one error (verified).** It is `-EBADE`, which `brcmf_fil_cmd_data` (`fwil.c:121`) returns for any
refusal by the firmware; the firmware's own code is only in debug logging. So a shared `-52` is no evidence of a
shared cause. Across the last ten boots:

| Message | Seen in | Reading |
|---|---|---|
| `WLC_SET_ROAM_TRIGGER error (-52)` | Every boot with Wi-Fi logs | At startup, no network involved. **Inferred:** unsupported by this firmware. |
| `brcmf_p2p_create_p2pdev: timeout` then `err=-5` | Every boot with Wi-Fi logs | A timeout, **not** `-52`. Before any association. |
| `brcmf_run_escan: error (-52)` | Three boots out of ten | A scan refused while connected. |
| `brcmf_cfg80211_get_station -52` | Today, one line | From reading `iw dev link` during this work; no baseline. |

**3.7.** The failure comes before any access point has set a domain, so the regulatory change below can't cause it.
`ccode=KR` is active at that moment, so the logs alone can't exclude the calibration file, but no mechanism connects
them. The boot-image test below settles it.

**1.7: this phone was silent during this work (verified).** Reads over about six minutes:

- `rx_packets` did not move (from 08:49:37 to 08:55:21, phone time), and the gateway's neighbour entry was `FAILED`.
- **The firmware and the kernel disagree on the channel.** `iw dev` asks the firmware (`chanspec`) and said 5280 MHz,
  80 MHz wide, on every read. `iw dev link` and the BSS table, from the association, said 2437 MHz, and no 5 GHz
  network was in the cached scan results.
- So the "5280 MHz" in roadmap 1.7 is the firmware's report, and may not be the network's channel.
- Earlier in the same boot, four rounds ran: `EAPOL-Key Replay Counter did not increase`, then `DISCONNECTED reason=0`
  not generated locally, then reconnect. Once a scan never finished (`brcmf_escan_timeout: timer expired`).
- This `EAPOL-Key` pattern appears in this boot only of the twelve.

**Inferred, not tested:** the firmware's radio has left the network's channel, or is stuck in a scan. That would fit
frozen reception with a live association, but nothing here shows why it happens. Power save, firmware roaming (the
roam trigger is refused at startup, so its other settings are skipped), and the country change during association are
all still candidates, as is the calibration.

## 2.4 Modem values

| Value | Repo (from the N) | G960F | Result |
|---|---|---|---|
| `model_info-hw_rev` | `0x1a` | Factory `HW_REV` and `HW_GPIO` are 26 | Matches (verified) |
| `model_info-system_rev` | `"026"` | Same 26; `cbd` logs `026` | Matches. Not in the F's stored trees, so stock's source for it is unknown. |
| Board revision pins | `gpg3-3..0`, `gpg1-5..2` | Same eight pins in the F's stock rev26 tree | Matches (verified) |
| `zrn,sys_rev` | `0xae` (174) | **Not read** | Sent to the modem now (`sys_rev:174, ds_det:0`) |

`0xae` is a hardcoded reading of the pins, recorded in `modem/README.md`. Reading them on the F means requesting
GPIO lines (`modem/ril/hwrev.py`), which is beyond this track's read-only limit, so it is in the test list below.
The logs here held no RAT history that could show the LTE-to-2G symptom either way. Nothing was changed.

## 2.5 Hub patch

- Vendor `lhd` is `G960FXXUHFVG4`, LHD2 ver. 108.20.545200, built July 2022. `lhd` takes its patch from `/dev/bbd_patch`
  (`lhd.conf`), so the driver's built-in patch is what the hub gets.
- The repo's `sensors/driver/q_os` patch came from a third-party kernel and is described as Android 10 and G960N.
  **Verified:** it is not the patch inside the F's stock kernel of the same build as `lhd`. Same ASIC, different blob.
  The `p_os` patch does not match either.
- The patch loads on the F (`Patch Done`).
- `ESW:CRASHED` per boot, counting lines (each crash logs two), oldest first: 2, 2, 2, 2, 22, 8, 62, 2, 2, 8, 26, 0.
  **Inferred:** the single crash in most boots comes at shutdown; the last one seen in the boot with 26 is seconds
  before its reboot. The busy boots are unexplained. **Whether the patch contributes is unknown.**
- The earlier asserts are attributed in `sensors/README.md` to AP-state commands. That may be the whole story.

## 2.6 Partitions

Verified on this phone by GPT label: `sda10` BOOT (55 MB), `sda11` RECOVERY (65 MB), `sda25` USERDATA. Also `sda3` EFS,
`sda12` RADIO, `sda19` VENDOR, `sda21` CACHE, which matches the fallback in `ztos-fs-recovery-reset`. The DTB partition
reads as all zeros here; stock's trees sit in the boot image.

| Script | Check before writing |
|---|---|
| `scripts/flash-boot-from-arch.sh` | New: `sda10` is BOOT, `sda11` is RECOVERY |
| `scripts/flash-from-rescue.sh` | New: `sda11` is RECOVERY |
| `scripts/flash-root.sh` | New: `sda25` is USERDATA |
| `installer/ztos_installer/targets.py` | New: `sda25` is USERDATA, before it is zeroed |
| `scripts/flash.sh` | Already checked `by-name`, and writes through it. Unchanged. |

The check reads `PARTNAME` from `/sys/class/block/<name>/uevent` and needs no root (`scripts/lib-partlabel.sh`,
tests in `scripts/test-partlabel.sh`). Against the live phone it passed all three and refused a wrong label. An N with
the layout these scripts already assume sees no change. Not yet exercised in a real flash, from the rescue shell or TWRP.

## 2.7 Dual SIM and bands

- **SIM, verified:** EFS `imei/factory.prop` has `ro.multisim.simslotcount=1` and `persist.radio.multisim.config=ss`. The
  driver's `ds_detect` is 1, and it tells the modem `ds_det:0`, which agrees. Dual-SIM F models exist, and for them the
  default `ds_detect` would be wrong. Stock's way of setting it was not traced.
- **Detection hint:** those two properties can tell a future dual-SIM phone apart without touching the modem.
- **Bands, not verified:** nothing on the phone can list them without modem commands. GSMArena's global list is
  1, 2, 3, 4, 5, 7, 8, 12, 13, 17, 18, 19, 20, 25, 26, 28, 32, 38, 39, 40, 41, 66. Swappa gives a much shorter, identical
  list for both F and N, which is plainly incomplete. No reliable G960N list was found, so **no F against N band
  comparison is claimed**.

Sources: [GSMArena, Galaxy S9](https://m.gsmarena.com/samsung_galaxy_s9-ampp-8966.php),
[Swappa, Galaxy S9 network](https://swappa.com/guide/samsung-galaxy-s9/network).

## 3.0a3 Bluetooth address

**The roadmap entry was wrong (verified).** The F stores it at EFS `bluetooth/bt_addr`: 17 bytes of text, the usual
colon-separated form, dated from the factory. `zrn-bluetooth-addr` reads it, and the running controller's address equals
it, so the phone does not use the chip's default. No code change is needed.

Two other findings:

- The Wi-Fi address equals EFS `wifi/.mac.info`. That file's date is later than the factory files' but the same as in the
  backup in `stock/`, so nothing has changed it since (verified). Its owner is Android's Wi-Fi user.
- The running Bluetooth firmware (0110.0232) is not this phone's VENDOR `bcm4361B2_semco.hcd` (baseline 0111). It is the
  right module, and an older build. It works, so it was left alone.

## Ready for phone test

Nothing below has been run. Each changes something on the phone, so they need coordinating. Tests 3 and 6 need a build and
flash. Before any flash, confirm `stock/` backups as usual.

**1. Board revision (2.4).** Requests eight GPIO lines as inputs, for an instant.

    sudo python3 modem/ril/hwrev.py

Expect `revision as stock computes it: 174`. Any other number means `zrn,sys_rev` is wrong on the F: record it, and
don't change it without a modem test under traffic.

**2. Silent Wi-Fi, country (1.7).** Should remove the country change during association.

    sudo iw reg set US       # use the country your access point advertises
    nmcli device disconnect wlp1s0; nmcli device connect wlp1s0

Then use the phone normally for an hour, and run test 5's reads every few minutes. Undo with a reboot.

**3. Calibration (2.3, 1.7, 3.7).** A new boot image.

    ssh <phone> 'sudo cat /run/android/vendor/etc/wifi/nvram.txt_r02a_b2' > build/nvram-g960f.txt
    sha256sum build/nvram-g960f.txt    # expect 968b71eb19fc8053e9a9bfb44e591563f3f4a2b46232912d760a1c6ba0626692
    scripts/build-boot.sh ztos-boot <this phone's usual flags> --wlan --wifi-nvram build/nvram-g960f.txt

Mind items 1.3 and 1.4: a rebuilt kernel has to match the installed modules. Pass if Wi-Fi associates and throughput holds.
Then check `dmesg | grep -E 'p2p|-52'` against the table above (3.7) and run test 5's reads (1.7). Fall back through TWRP
if it fails.

**4. Firmware roaming (1.7).** Done: the firmware reports a roam with `LINK` up and `BSSID`, not `E_ROAM`, and
`kernel/patch-brcmfmac-roam.py` passes that on.

**5. Reads when Wi-Fi goes silent (1.7).** Read-only, and useful in every test above.

    iw dev wlp1s0 info | grep channel       # the firmware's channel
    iw dev wlp1s0 link | grep freq          # the association's channel
    cat /sys/class/net/wlp1s0/statistics/rx_packets
    ip -4 neigh show dev wlp1s0

Silent means the two channels disagree and `rx_packets` stops. Pass means they agree and it grows, for an hour or more.

**6. Hub patch (2.5).** A module load. The F's stock patch would have to be extracted locally into `build/`, never committed
(`stock/` must not enter the repository), or taken from Samsung's G960F kernel source. Compare `ESW:CRASHED` counts over several
boots, with the same use. Do this only if the crashes matter: the sample here is noisy.

**7. Model (2.1, 2.2).** Install `ztos-variant`, its service and a rebuilt `ztsettings`, then start the service.

    cat /run/ztos-variant    # expect SM-G960F

About should say `SM-G960F`. No boot image needed. The device tree's model changes with the next boot image.

**8. Label checks (2.6).** On the next real flash from the rescue shell and from TWRP, with the phone's usual path. A mismatch
message there means the check, not the phone, needs looking at.
