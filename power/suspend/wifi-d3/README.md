# Wi-Fi associated through sleep: bench record (ROADMAP 8.16)

Kernel side: kernel/patch-brcmfmac-d3.py, kernel/make-dts.py (--wlan). Not in use on the phone:
dp42 is on BOOT. **No more D3 tests until the owner allows them and a watchdog reset has been
shown to work on the test kernel.**

## What is established
- D3_ACK never came because mainline sent D3_INFORM through the TCM mailbox. This firmware
  (13.50.31.5, shared version 7, flags 0x30040007, no USE_MAILBOX) takes mailbox data on the
  control ring (0x23 out, 0x24 back), as Samsung's bcmdhd does. Over the control ring it ACKs
  (`D3: acknowledged`, swa1 and swa4).
- WL_REG_ON (gpg0-5) is input + pull-down in every SoC sleep at its reset CON_PDN (0x2aaa): the
  module lost power. With the DT's PREV hold it reads 0x2eaa.
- WL_HOST_WAKE (gpa0-7) registers as `brcmf_host_wake` and fires: once right after the ACK
  (swa1: that edge gave the suspend up).

## What does not work yet
- Leaving D3 without a PCIe D3/PERST cycle (a suspend given up, or d3_test): neither a bare
  HOSTRDY (swa1) nor D0_INFORM in the TCM mailbox on doorbell 1 plus HOSTRDY (swa4) brings
  the firmware back; every ioctl times out afterwards. Next to try: D0_INFORM as a
  control-ring message, the PMCSR D3hot -> D0 transition (pci_set_power_state) before HOSTRDY,
  or never leaving D3 without the link cycle (refuse the hot exit: unbind and rebind).

## Trials
- swa1-hang/: one `zrn-sleepd once 60` (connected mode). The ACK came; host-wake aborted the
  suspend; the firmware went silent after the hot exit. Old zrn-sleepd plus the AOD never
  rebound Wi-Fi: unreachable until the owner restarted the phone. No SoC hang.
- swa4-d3test/: `d3_test` 15 s, the system awake, deadman armed. The same silent firmware.
  The deadman's unbind then oopsed in brcmf_free_wiphy (kfree of the patch's static WoWLAN
  struct; fixed since with kmemdup) and the deadman died before its reboot step. The kernel
  stayed up, systemd kept feeding the watchdog (RuntimeWatchdog 30 s), so no reset. The owner
  restarted the phone.

## Watchdog
s3c2410_wdt.tmr_atboot=1 tmr_margin=60 are on every image's command line. systemd opens
/dev/watchdog0 (30 s) and feeds it while PID 1 runs; before that the kernel's watchdog worker
does (WATCHDOG_HANDLE_BOOT_ENABLED=y, OPEN_TIMEOUT=0). Lockup detectors are off. So only a
whole-system stall resets the phone; a task oops or a stuck driver does not, and the watchdog
is stopped through SoC sleep (power/suspend/README.md). A test deadman must not run its
recovery in the same task as the risky unbind: reboot from an independent timer, or with
`systemctl reboot --force --force` / sysrq.
