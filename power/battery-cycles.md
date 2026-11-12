# Battery cycles, capacity and temperature (ROADMAP 8.27)

What the fuel gauge's numbers mean on ztOS, how Samsung counts, and what is
wrong with the temperature. Labels: **observed** (read on the phone),
**source** (Samsung's kernel, the stock tree, DeviceKeystring, the mainline
driver), **inferred**.

## The gauge's cycle count

- `max170xx_battery/cycle_count` is the MAX77705 fuel gauge's Cycles register
  (0x17), handed over raw by mainline `max17042_battery` (source).
- Its unit is 1 % of a cycle. The MAX17055 datasheet: "stored as a percentage
  of a full cycle", "0 to 655.35 cycles with a 1% LSb" (source). Samsung's
  `fg_cycle` sysfs divides it by 100, `sec_battery_sysfs.c` FG_CYCLE (source).
- On the phone it went from 654 to 663 while charging from 18 % to 37 %
  (observed): about half a count for each per cent charged, so charge and
  discharge each count half of a cycle (inferred, in line with the datasheet).
- So 653 is **6.53 cycles**, not 653 or 163. The register is volatile and only
  reaches 655.35 cycles; it counts from the last time the gauge was reset or
  the counter wrapped, not over the battery's life (inferred).
- Mainline writes nothing to Cycles, the model or LearnCFG with this tree: it
  has no `config_data`, so the POR-init path does nothing (source). ztOS's
  driver did not reset it (source); when it last started is not readable
  without a raw register read (TimerH, 0xBE), which was not done.

## Capacity and health

- `charge_full` is register 0x10 (FullCapRep in the m5 map; Samsung's header
  calls it FULLCAP), what the gauge has learned (source). 2236 of 2967 mAh is
  **75 %**.
- `charge_full_design` 2967 mAh is DesignCap, 0x0B97 (observed), the stock
  tree's `fuelgauge,capacity = <0xb97>` (source): Samsung's own setting is
  still in the gauge, so the gauge still runs Samsung's configuration (inferred).
  Neither Samsung's kernel nor mainline loads a model table; Samsung's driver
  only adjusts QRTable20/30, FilterCFG and DesignCap at low temperature, and
  writes Temp (source).
- Samsung's ASOC is a different register: FullCapNom (0x23) over DesignCap,
  `max77705_fuelgauge.c` ENERGY_FULL (source). EFS `asoc` read 77 (observed,
  written 6 Nov when Android last ran). Mainline does not expose FullCapNom.
- Both are estimates that the gauge relearns at full charge. With the
  temperature frozen (below), its temperature compensation runs at 33.3 C
  (inferred).

## How Samsung counts

- `sec_battery` keeps `batt_cycle`, set only from userspace through
  `battery_cycle` (source). Android keeps `batt_discharge_level` in EFS
  (`/efs/FactoryApp/batt_discharge_level`): per cent of charge discharged over
  the battery's life; a cycle per 100 (observed: 103986, and CISD's
  BATT_CYCLE in `cisd_data` is 1039).
- **Samsung's count was 1039 cycles on 6 Nov 2026** (observed, file mtime).
  Only Android adds to it; ztOS does not, and never writes EFS.
- Stock uses it for the battery's ageing steps (`battery,age_data`: 4.35 V
  float up to 300 cycles, then 4.33, 4.31 at 400, 4.29 at 700, 4.24 V at
  1000) and for its health (`battery,health_condition`: good up to 900
  cycles and ASOC 75 %, normal to 1200 / 65 %, aged to 1500 / 55 %, then
  bad; the worse of the two wins) (source). At 1039 cycles stock would charge
  this battery to 4.24 V and call it "normal"; ztOS charges to 4.35 V
  (inferred from source and zrn-charge).
- DeviceKeystring's BatteryStatusEng (*#0228#) shows both: "Battery Cycle"
  (`battery_cycle`) and "FG Cycle" (`fg_cycle`, register / 100), and "FG
  Fullcapnom" (source).

## The temperature is not measured

- `max170xx_battery/temp` read 33.3 C in all 187 samples of
  `/var/lib/zrn-sleep/soc.csv`, idle, charging and on the pad (observed).
- Stock's battery temperature comes from the SoC's ADC (thermal_source 2 =
  ADC); stock sets Config.TEX (host-supplied temperature) and writes each
  reading into the gauge's Temp register (source). Nothing on ztOS writes it,
  so it keeps Android's last value (inferred).
- zrn-charge's temperature rules and the p9320 driver's 10 V check read this
  value, so **none of the charging temperature limits works** (source:
  `zrn-charge`, `kernel/p9320_wireless.c`).

## The kernel proposal (not applied, not flashed)

`power/max17042-gauge-proposal.patch`, against mainline 7.1
`drivers/power/supply/max17042_battery.c`. Reads only; it writes no
register:

1. `cycle_count` = Cycles / 100, whole cycles as the ABI says.
2. With Config.TEX set (0x1D bit 8), `temp` gives -ENODATA, and health gives
   "Unknown", instead of the stale register. p9320 then keeps 5 V (it treats
   unknown as hot) and zrn-charge refuses 9 V; zrn-charge's current derating
   would still need a real temperature.
3. `max_register = 0xff` on the regmap, so `debugfs/regmap/1-0036-max17042/
   registers` shows the gauge (FullCapNom 0x23, TimerH 0xBE, Config 0x1D,
   LearnCFG 0x28). Today it shows only register 0.

With item 1 in, `ztmetro/power/battery-health.h` must stop dividing by 100.

A real battery temperature needs the SoC's ADC: `power/thermistors/README.md`.
Two gauge writes would follow from it and need the owner's approval first:
Temp (0x08) from the ADC reading each minute, as stock does; or Config.TEX
cleared (0x1D bit 8 to 0) if the gauge's own thermistor input is wired, which
is not known.
