# Stock's BatteryStatusEng page, field by field (ROADMAP 8.32)

DeviceKeystring (`/system/priv-app/DeviceKeystring`, *#0228#), activity
`com.sec.android.app.status.BatteryStatusEng`, decompiled on the PC from a
copy of the APK on the phone's read-only SYSTEM (md5 eb327133...). Its sysfs
paths come from the APK's `assets/base.dat` and `sm-g960f.dat` (base64 XML).
"Mainline" is what ztOS's 7.1 kernel offers. ZrnDevTest's temperatures and
charging pages show what can be shown.

| Field | Stock sysfs (`battery/` = `/sys/class/power_supply/battery/`) | Stock source | ztOS |
|---|---|---|---|
| Voltage now | `battery/voltage_now` | fuel gauge VCELL | `max170xx_battery/voltage_now` |
| Voltage ADC, VF ADC, VF OCV | `batt_vol_adc`, `batt_vf_adc`, `batt_vfocv` | sec_battery | `voltage_ocv` for VF OCV; none for the others |
| Average voltage, ADC, cal. | `batt_vol_aver`, `batt_vol_adc_aver`, `batt_vol_adc_cal` | sec_battery | `voltage_avg`; none for ADC and cal. |
| Level, level block | `capacity` | sec_battery (scaled gauge SOC) | `capacity` (the gauge's RepSOC, unscaled) |
| Current Chg ADC | `chg_current_adc` | sec_battery | none |
| Current now, average | `current_now`, `current_avg` | fuel gauge | `current_now`, `current_avg` |
| DC IC input current | `direct_charging_iin` | direct charger | none: the S9 has no direct charger |
| Full voltage, recharging | `fg_full_voltage` | sec_battery DT (`chg_float_voltage`, recharge) | `constant_charge_voltage`; recharge level is zrn-charge's |
| Battery Cycle | `battery_cycle` | Android's count from EFS `batt_discharge_level` / 100 | read from EFS by ztOS (frozen since Android last ran) |
| FG Cycle | `fg_cycle` | gauge Cycles (0x17) / 100 | `cycle_count`, raw: / 100 in ztOS's apps |
| FG Fullcapnom | `fg_fullcapnom` | gauge FullCapNom (0x23) | none (proposal: regmap debugfs) |
| FG Firmware | `fg_firmware` | not on the MAX77705 | none |
| Wireless Vout, Vrect, OP freq | `wc_vout`, `wc_vrect`, `wc_op_freq` | mfc_charger | `p9320-wireless/voltage_now`, `vrect`, `freq` |
| Wireless CMD info | `wc_cmd_info` | mfc_charger | none; `rx_status`, `tx_error` instead |
| Current max | `current_max` | sec_battery input limit | `max77705-charger/input_current_limit` |
| System current, average | `power_now`, `power_avg` | sec_battery | hwmon `max77705` ISYS (now only) |
| Wireless 2.0 auth, Duo pad power | `wc_auth_adt_sent`, `wc_duo_rx_power` | mfc_charger | none |
| Charging type | `charging_type` | sec_battery | `max77705-usbc/charger_type`, `usb_type` |
| Battery thermistor ADC, temp | `batt_temp_adc`, `batt_temp` | SoC ADC ch 4 | none (the gauge's `temp` is not measured) |
| USB thermistor | `usb_temp_adc`, `usb_temp` | SoC ADC ch 1 | none |
| CHG thermistor | `chg_temp_adc`, `chg_temp` | SoC ADC ch 3 | none |
| DC IC thermistor | `dchg_temp_adc`, `dchg_temp` | not on the S9 | none |
| Temperature (average), ADC, cal. | `batt_temp_aver`, `batt_temp_adc_aver`, `batt_temp_adc_cal` | sec_battery | none |
| AP thermistor | `sec-ap-thermistor/temperature`, `temp_adc` | SoC ADC ch 0 | none |
| Blanket, camera thermistors | `sec-bk-thermistor`, `sec-cf-thermistor` | not in the S9's tree | none |
| Coil thermistor | `batt_wpc_temp_adc`, `batt_wpc_temp` | SoC ADC ch 4 (the battery's) | none |
| PA0 thermistor | `sec-pa-thermistor/temperature` | SoC ADC ch 10 | none |
| PA1, Wi-Fi, NR thermistors | shown only if the file exists; Wi-Fi is `sec-wf-thermistor` (ch 2) | | none |
| LRP temperature | SemTemperatureManager, predicted surface | Android service | none |
| AG | `/sys/module/asv_exynos/parameters/asv_group` | SoC ASV group | none |
| Network, RSSI | telephony | | ztoscorenetworking |
| Quick start button | resets the fuel gauge; the kernel's quick-start also sets Cycles to 0xA0 | ModulePower.resetFuelGaugeIC | deliberately not offered: a gauge write |
