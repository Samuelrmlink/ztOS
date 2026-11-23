# zrn-cam: the SoC and PMIC writes it makes

**Approved by the owner 2026-11-12** ("Approve list + (b)"): exactly A to K below,
with the values given, plus (b). Later the same day the owner also approved (a)
(release `dvfs_cam` at close) and BUCK2 off at close. Anything not here needs
its own approval first.

"Oct" means the October 2026 driver already made this write with this value.

## On every open of a camera (rear or front)

**A. S2MPB03, I2C 0x56, bit-banged on gpm0-0/gpm1-0. Rear only. (Oct)**
- Registers: one per LDO at 0x02+n. Bits 5:0 are the voltage code, written only if it differs. Bit 7 is the enable.
- When: set at power-up in stock's 2L3 order, cleared at power-off.

| LDO | Register | Code | Voltage |
| --- | --- | --- | --- |
| LDO4 | 0x06 | 26 | 1.025 V |
| LDO6 | 0x08 | 46 | 2.95 V |
| LDO5 | 0x07 | 14 | 2.15 V |
| LDO1 | 0x03 | 26 | 1.025 V |
| LDO3 | 0x05 | 44 | 1.8 V |
| LDO2 | 0x04 | 32 | 1.1 V |
| LDO7 | 0x09 | 40 | 2.8 V |

**B. S2MPB02, I2C 0x59, through zrn-flash's bus 97 (gpp6-0/1).**
- Registers: LDOn is at 0x1D+n. Bits 6:0 are the voltage, bit 7 is the enable.
- Rear:

| LDO | Register | Code |
| --- | --- | --- |
| LDO14 | 0x2B | 108 |
| LDO10 | 0x27 | 48 |
| LDO15 | 0x2C | 88 |

- Front:

| Rail | Register | Code |
| --- | --- | --- |
| BUCK2 | enable 0x0D bits 7:6 = 11, voltage 0x0E | 144 (1.3 V) |
| LDO16 | 0x2D | 88 |
| LDO13 | 0x2A | 88 (it reads 92 at boot) |
| LDO6 | 0x23 | 48 |
| LDO5 | 0x22 | 36 |

- When: set at power-up (Oct values); cleared at power-off.
- Exception at power-off: a rail found already on at power-up gets its enable bits and voltage put back as found, not switched off. Approved 2026-11-12.
- BUCK2 is the exception to that exception: it boots at 0x0D = 0x58, 0x0E = 0x40 (enable bits 01), and at close 0x0D bits 7:6 are written 00 whatever they were. The rail map found nothing it feeds. Approved 2026-11-12.
- LDO9 (0x26, the sensor hub's) is never written.

**C. GPIO. (Oct)**
- Direct writes through 0x10830000:
  - gpc0 PUD (+0x68): pins 0 to 3 set to pull 3 while in use, then restored.
  - gpp5 PUD (+0x28): pins 2 to 5 set to 0, then restored.
- MCLK on gpc1 (+0x80 CON, +0x84 DAT, +0x88 PUD, +0x8C DRV). Rear pin 4, front pin 5.
  - On: PUD 0, DRV 0, CON 2.
  - Off: DAT bit 0, CON 1, PUD 1.
- Through gpiolib:
  - Reset pins gpg2-6 (rear) and gpg2-5 (front).
  - The open-drain I2C pins.

**D. PMU 0x14060000.**
- Bit 0 of +0x70C (rear PHY) or +0x730 (front PHY): set at stream start (Oct).
- **(b)** Cleared at stop on both cameras. Oct cleared it on the front only. Approved 2026-11-12.
- Read only: +0x404C (ISPPRE status). The driver refuses to start if the domain is not 0xF.

**E. SYSREG ISPPRE 0x16210500. (Oct)**
- Bit 0 (rear) or bit 2 (front): cleared then set at stream start, cleared at stop.

**F. CSIS, rear 0x16230000 or front 0x16240000. Stream start and stop. (Oct)**
- CMN 0x004: bit 1 software reset; bits 8-9 lanes; bits 10-11 interleave; bits 16-19 VC enable; bit 21 cleared; bit 0 enable.
- PHY_CMN 0x704: lane enables and bit 0.
- CFG/RES 0x40 to 0x74.
- MSK0 0x10 = 0x0FFFFF1F, MSK1 0x18 = 0xFFF0. Both are 0 at stop. No IRQ is requested.
- SRC0/SRC1: write 1 to clear.

**G. D-PHY, rear 0x16160000 or front 0x16180000. (Oct)**
- Rear only: +0x404 = 0x10, +0x40C = 0x05, +0x414 = 0x68, +0x418 = 0x6C.
- +0x2418 or +0x1018 = 0x02.
- +0x2484 or +0x1084: low byte = 1.
- Per lane:
  - +0xE0 bit 0 and +0x3C bits 5-6: skew, rear full size only.
  - +0xAC bit 2.
  - +0xB0 settle: rear 66 or 10, front 36 or 72.

**H. CSIS WDMA 0x162F0000. (Oct)**
- +0x00 low nibble = 6, +0x08 bit 1, +0x0C = 0x01400140, +0x10 low 5 bits = 0.

**I. CSIS VC DMA, rear 0x162B0000 or front 0x162C0000. (Oct)**
- CTRL 0x00 = 3 to run, 0 to stop.
- FMT 0x04 = 0.
- ADDR 0x10 to 0x2C = the buffer address, rotated over 4 buffers each frame.
- RESOL 0x40 = width.
- 0x400 = 0, 0x41C = 0.
- MASK 0x404 = 0x00F11F00, 0 at stop.
- SRC 0x408: write 1 to clear.

**J. APM DVFS through the ACPM mailbox. (Oct)**
- Request: channel 0, domain 7 (`dvfs_cam`) at 690000 kHz, on every stream start.
- How: the queue in APM SRAM, then mailbox 0x14100000 +0x08 INTGR0. On the reply, +0x20 INTCR1, and +0x1C INTGR1 if more is pending.
- It is never released.
- The debugfs `dvfs-` command accepts only this request.

**K. The camera's own chips. (Oct)**
- Sensor:
  - The vendor mode tables.
  - 0x0104 group hold, 0x0100 stream.
  - 0x0202 exposure, 0x0204 gain.
  - Front: 0x6028 = 0x4000, 0x0216 = 0.
  - 0x0601 = 2 (colour bars) only with `live_tp`.
- Focus AK737x at 0x0C: 0x00 and 0x01 position, 0x02 = 0.
- Aperture AK7372 at 0x4C: 0xAE = 0x3B; 0xA6 = 0x7B, then 0; 0x00 to 0x02. Register 0x03 is refused.

## Not used

- Iris: S2MPS18 LDO39 to LDO41 (0x70 to 0x72) are written only with `iris_allowed=1`, which is never set.
- No CMU register is written. 0x1A241094 and 0x1A241890 are read only.

## Added 2026-11-12

**(a) Release `dvfs_cam` when a stream ends**, and on the error paths after it was raised.
- Request: the same transport as J, domain 7, at 590000 kHz.
- The value: stock DT `devfreq_cam` has `freq_info = <690000 590000 690000 590000 690000 690000>`, so 590000 is its default and minimum. The ECT itself (inside sboot) isn't on this PC or readable from the phone, so it was not used.
- The debugfs `dvfs-` command accepts only 7-690000 and 7-590000.

The ACPM DVFS channel 0 is shared with snd-zrn-abox (domain 9) and the planned MIF request (power/rail-map). Each sender takes only replies with its own sequence number, and leaves the others' in the queue. Two senders writing the queue at the same instant are not locked against each other across modules, so tests that use the channel are serialised with `ztlock`.
