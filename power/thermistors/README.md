# The S9's thermistors (ROADMAP 8.32)

Every thermistor stock reads, how it decodes them, and what ztOS can read.
`starlte-rev26.json` holds the tables, made by `extract.py` from stock's rev26
device tree (the F's board).

## Stock's thermistors

All of them are on the SoC's own ADC, `adc@14230000` (`samsung,exynos-adc-v3`,
12 bit, clock `gate_adcif`). None is on the S2MPS18, the MAX77705 or the
fuel gauge.

| Thermistor | ADC channel | Stock driver, sysfs | Table | What stock does with it |
|---|---|---|---|---|
| Battery | 4 | sec_battery `adc-temp`, `batt_temp` | `battery,temp_table_*` | Charging stops at 50 C (resumes 45) and at 0 C (resumes 5); swelling steps: above 41 C 1090 mA and a 4.15 V float, below 15 C 700 mA, below 5 C 700 mA and 4.15 V; a lower limit when battery and charger are both hot (mix_high_temp 42 C); the gauge's Temp register |
| USB connector | 1 | `adc-usb-temp`, `usb_temp` | `battery,usb_temp_table_*` | Cuts VBUS when hot (`vbus_limit`) |
| Charger | 3 | `adc-chg-temp`, `chg_temp` | `battery,chg_temp_table_*` | Input current limit above 46 C (`chg_high_temp`, back at 44) |
| Wireless coil | 4, the battery's channel | `adc-wpc-temp`, `batt_wpc_temp` | `battery,wpc_temp_table_*` | Pad current 600 mA above 38 C (450 mA with the screen on), back at 36 |
| AP | 0 | sec-thermistor `sec-ap-thermistor` | `adc_array` / `temp_array` | Thermal throttling input |
| Wi-Fi | 2 | `sec-wf-thermistor` | same as AP | Shown only |
| PA (modem) | 10 | `sec-pa-thermistor` | its own, 111 points | Shown only |

The coil reads channel 4, the battery's: stock's tree points both
`adc-temp` and `adc-wpc-temp` there, so the S9 has no coil thermistor of
its own (inferred from the tree). `adc-in-bat` (channel 11) has no table and
is unused. BatteryStatusEng also lists blanket, camera, DC-IC, PA1 and NR
thermistors; this phone's tree has none of them.

Decoding (`sec_bat_get_value_by_adc`, `sec_adc.c`): read the channel 5 times
(`adc_check_count`), drop the highest and the lowest, average the rest, then
interpolate linearly between the two table points around it. Past either end,
use the end value. Temperatures are in tenths of a degree. The battery, USB,
charger and coil tables are the same: ADC 275 is 90.0 C, 3676 is -20.0 C.

## What ztOS can read today

| Reading | Where | What it is |
|---|---|---|
| Gauge temperature | `max170xx_battery/temp`, thermal zone 0 | Not a measurement: always 33.3 C, the last value Android wrote (`../battery-cycles.md`) |
| P9320 die | `p9320-wireless/die_temp_raw` | The receiver's own die sensor, raw; its scale is not documented here |
| PMIC | hwmon `max77705`: VBYP, VSYS, IIN, ISYS | Voltages and currents, no temperature |

No thermistor is readable: `CONFIG_IIO` is off and mainline's `exynos_adc`
has no v3 compatible.

## Ways to read the ADC, with their cost

1. **Kernel config and driver.** Turn on `CONFIG_IIO` and `CONFIG_EXYNOS_ADC`,
   add `samsung,exynos-adc-v3` (Samsung's v3 is exynos7's set-up with the
   result at 0x08), the node and its clock, then `generic-adc-thermal` or
   `ntc_thermistor` zones from the tables. This needs a BOOT flash, which is
   not approved.
2. **Out-of-tree module.** `CONFIG_MODULES=y`, so a small module could map
   0x14230000 and convert on demand without IIO, giving the readings in
   sysfs or hwmon. It writes SoC registers (below), and the `gate_adcif` clock
   must be on first: a read of a gated block hangs the bus.
3. **ACPM or the PMIC's ADC.** Not applicable: no thermistor is wired there.

Each way writes these ADC registers, so it needs the owner's approval for
each one first (from `drivers/iio/adc/exynos_adc.c`, v3; none has been written):

| Register | Offset | Write | When |
|---|---|---|---|
| CON1 | 0x00 | 0x4 (soft reset), then 0x2 | Once, at set-up |
| CON2 | 0x04 | 0x60 (C_TIME 6), then the channel in bits 3:0 | Set-up, then before each conversion |
| CON1 | 0x00 | bit 0 set (EN_START) | Each conversion |
| INT_EN | 0x10 | 1, or leave it 0 and poll DAT bit 31 | Set-up |
| INT_ST | 0x14 | 1 (clear) | After each conversion, if interrupts are on |
| DAT | 0x08 | Read only: bit 31 done, bits 11:0 the value | Each conversion |
| CMU gate for `gate_adcif` | in the clock block at 0x15a80000 | Enable | Before any of these, if the bootloader left it off |
