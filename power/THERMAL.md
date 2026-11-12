# Stock's thermal policy (Galaxy S9, from source)

What Samsung's stock firmware does with temperature on the SM-G960F (starlte,
Exynos 9810): when it throttles, when it stops charging, and when it powers
off. Taken from source only. Nothing here was measured on the phone.

## Sources and how to read the citations

| Tag | What it is |
|---|---|
| **DT** | Stock device tree, rev26 (the F's board, `model_info-hw_rev = <0x1a>`), extracted from `stock/BOOT.img` (dump `dtb5_1fd7000`, as `power/thermistors/extract.py` describes). A path like `/battery/battery,temp_high_threshold_normal` is the node and property. |
| **K** | Samsung's vendor kernel, as the LineageOS exynos9810 clone (`third_party/vendor-kernel/lineage`, a sparse checkout). `file:function (Lnnn)` gives line numbers in that clone. Stock's binary may be a slightly different Samsung release. The code tells how stock behaves; the DT gives the numbers. |
| **CFG** | Stock's kernel config, `modem/stock-kernel.config` (Linux 4.9.118, extracted from the stock boot image). |
| **AOSP** | The Android framework (public AOSP). Samsung ships its own closed build of it. |

Units: the battery DT uses tenths of a degree C (500 = 50.0 C), the thermal
zones use milli-degrees (83000 = 83 C), currents are mA and voltages mV.
`temp_check_type = 2` means the checks compare temperatures, not raw ADC
values. `/battery/battery,temp_check_type`; K
`include/sec_charging_common.h` enum `sec_battery_temp_check`.

**ECT caveat.** Stock has `CONFIG_ECT=y` (CFG). Once a thermal zone is
registered, K `drivers/thermal/samsung/exynos_tmu.c:exynos_tmu_parse_ect`
(L1371–1530) replaces the trip temperatures with values from the bootloader's
ECT binary:

- **LITTLE and ISP** (no governor named, so step_wise): the trip temperatures
  and each trip's maximum frequency (or, for ISP, frame rate) come from ECT
  block `AP_THERMAL`.
- **BIG and G3D** (`power_allocator`): the trip temperatures, `k_po`, `k_pu`,
  `k_i`, `i_max`, `integral_cutoff`, `hotplug_in/out_threshold` and
  `limited_frequency` come from ECT block `PIDTM`.

So the DT trip values below are the fallback. They are what stock runs only
if ECT has the same numbers. **ECT is not in this repo.** It lives in sboot:
`power/suspend/audit-samsung-minmax.py` parses another block of it, but the
sboot image is not on disk now. Until it is parsed, or stock's dmesg lines
`Parsed From ECT : [i] Temperature : t, frequency : f` are captured, the
per-trip frequency caps for LITTLE and the frame-rate caps for ISP are
**unknown**. The DT has none: every LITTLE and ISP cooling map has
`cooling-device = <phandle 0x0 0x0>`.

## 1. sec_thermistor: the AP, Wi-Fi and PA thermistors

| Thermistor | DT node | ADC channel | What stock's kernel does with it |
|---|---|---|---|
| AP (board, by the SoC) | `/sec_thermistor@0`, `thermistor_name = "sec-ap-thermistor"` | 0 | Exports `/sys/class/sec/sec-ap-thermistor/temperature` and `temp_adc` only |
| Wi-Fi | `/sec_thermistor@2`, `"sec-wf-thermistor"` | 2 | Same, sysfs only |
| PA (modem) | `/sec_thermistor@10`, `"sec-pa-thermistor"` | 10 (0xa) | Same, sysfs only |

- The driver (`drivers/sensors/sec_thermistor.c`, `CONFIG_SENSORS_SEC_THERMISTOR=y`
  in CFG) is **not in our sparse checkout**. "Sysfs only" is inferred: no
  kernel thermal zone, cooling map or battery code in this DT or in
  `drivers/battery_v2`/`drivers/thermal` reads these channels.
- **SSRM** (Samsung's user-space SIOP and thermal manager, in the closed
  framework) is what reads the AP thermistor (skin temperature). It throttles
  CPU, GPU and brightness, and sets the charge limit through
  `/sys/class/power_supply/battery/siop_level` (0..100). Its temperature
  levels are **closed source**; nothing in the vendor tree gives them.
- What the kernel does with `siop_level < 100`: K
  `sec_battery.c:sec_bat_get_charging_current_by_siop` (L304–362), with the
  values from DT `/battery/battery,siop_*`:

| Cable | Input limit | Charge limit |
|---|---|---|
| Wired 5 V (USB, DCP) | 1200 mA (`siop_input_limit_current` 0x4b0) | min(1800 mA hard-coded, 1000 mA `siop_charging_limit_current`) |
| Wired HV (AFC 9 V) | 500 mA (`siop_hv_input_limit_current` 0x1f4); also VBUS goes to 5 V while charging (`sec_bat_change_vbus` L487) | 1000 mA (`siop_hv_charging_limit_current`) |
| USB-PD | 6000 / Vin(V) mA, so 6 W (L345–348) | 1800 mA |
| Wireless 5 V | 600 mA (`siop_wireless_input_limit_current`) | 700 mA (`siop_wireless_charging_limit_current`); also capped at 1000 mA, 500 mA at level 10 or below, 0 at level 0 |
| Wireless HV | 600 mA (`siop_hv_wireless_input_limit_current`) | 700 mA |
| Store mode, wireless HV | 450 mA (`siop_store_hv_wireless_input_limit_current`) | n/a |
| On a call | `siop_call_cc/cv_current` = 330 mA | n/a |

  With `siop_level < 100`, the kernel's own heat controls (mix, AFC/chg,
  PD, wireless; §3) are skipped: each one requires `siop_level >= 100`.
  SSRM takes over.

## 2. TMU: the SoC's own sensors (exynos_tmu, ACPM)

Temperatures are read through ACPM firmware (`exynos9810_tmu_read` →
`exynos_acpm_tmu_set_read_temp`, K L863). Every trip has 5 C hysteresis,
except the "switch-on" trips, which have 2 C.

### Trip points (DT `/thermal-zones/<zone>/trips/<trip>`, temperature in C)

| Zone (`zone_name`) | Governor | Polling (idle, passive) | Trips: type, C |
|---|---|---|---|
| BIG (`BIG_THERMAL`) | power_allocator | 1000 ms, 50 ms | big-cold active 20; big-switch-on active 55; **big-control-temp passive 83**; big-alert0..3 active 95, 100, 105, 110; **big-hot hot 115** |
| LITTLE (`LITTLE_THERMAL`) | step_wise (default, CFG) | interrupt only | little-alert0..6 active 20, **76, 81, 86, 91, 96, 101**; **little-hot hot 115** |
| G3D (`G3D_THERMAL`) | power_allocator | interrupt, 100 ms passive | gpu-cold active 20; gpu-switch-on active 78; **gpu-control-temp passive 88**; gpu-alert0..3 active 105, 110, 115, 115; **gpu-hot hot 115** |
| ISP (`ISP_THERMAL`) | step_wise | interrupt only | isp-alert0..6 active 20, 91, 96, 101, 101, 101, 101; isp-hot hot 115 |

### Cooling devices (DT `/thermal-zones/<zone>/cooling-maps`)

| Zone | Device (phandle) | How it limits |
|---|---|---|
| BIG | `/cpufreq/domain@1` (cpus 4-7, phandle 0x149), bound to big-control-temp only, states unlimited (`0xffffffff`) | IPA (power allocator) holds the cluster near **83 C**. It caps frequency from a power budget, so there is no fixed frequency per trip. `sustainable-power`, `k_*` and `i_max` are 0 in DT, so they come from ECT PIDTM. |
| LITTLE | `/cpufreq/domain@0` (cpus 0-3, 0x14c), one map per trip, alert0..hot | step_wise. Each trip's frequency cap comes **from ECT only** (`tbps.value` = `max_frequency`, K `of-thermal.c` L194–206); the DT gives none. **Unknown.** |
| G3D | `/mali@17500000` (0x156), bound to gpu-control-temp | IPA holds the GPU near **88 C**. Mali's own TMU table (driver not in checkout; use inferred): `gpu_tmu_control = 1`, `gpu_temp_throttling` = 572, 546, 455, 338, 260, 260 MHz (6 levels), `gpu_max_clock` 572 MHz. |
| ISP | `/fimc_is@16290000` (0x110), per trip | Caps the camera's frame rate (K `isp_cooling.c`). The rates come from ECT. **Unknown.** |

Reference frequencies (DT `/cpufreq/domain@*`): LITTLE `min-freq` 455 MHz;
BIG `min-freq` 650 MHz, `max-freq`/`policy-max`/`pm_qos-booting` 1794 MHz
(boot-time). The `cpufreq-ufc` tables go up to 2964 MHz.

### Big-core hotplug (thermal)

| What | Value | Source |
|---|---|---|
| Big cores offlined (`PM_QOS_CPU_ONLINE_MAX` = 4, so cpus 0-3 only) at | **BIG >= 96 C** | DT `/BIG@10070000/hotplug_out_threshold = <0x60>`; K `exynos_tmu.c:exynos_throttle_cpu_hotplug` (L1157–1188), `NR_HOTPLUG_CPUS 4` (`exynos_tmu.h` L30) |
| Back online when | **BIG < 91 C** | `/BIG@10070000/hotplug_in_threshold = <0x5b>` |
| ECT override | BIG is power_allocator, so these are replaced if ECT PIDTM has `hotplug_out/in_threshold` | `exynos_tmu_parse_ect` L1502–1523 |
| Extra cap | When ACPM reports `stat == 2`, the cluster-1 max goes to ECT's `limited_frequency` (**unknown**); it is lifted at stat 0 or 1 | `exynos9810_tmu_read` L863–884 |

### Shutdown on AP temperature

- **There is no `critical` trip** anywhere in stock's thermal zones. The top
  trip in every zone is type `hot` at **115 C**. In K `thermal_core.c:handle_critical_trips`
  (L440–458), only `THERMAL_TRIP_CRITICAL` calls `orderly_poweroff()`. A
  `hot` trip only calls the sensor's `notify`, and exynos_tmu has none. So
  **stock's kernel never does an orderly power-off on TMU temperature.**
- **Hardware trip (inferred):** `tmu_core_enable` sets `THERM_TRIP_EN` (K
  L471–481). `exynos9810_tmu_initialize` (L376–468) programs the non-passive
  trips from the top down, so the 115 C `hot` trip lands in the first
  threshold slot, which is rising level 7 on the Exynos7-style TMU. The
  hardware thermal trip (a PMU reset or power-down, outside the kernel) is
  therefore **at 115 C**. This is inferred from the register layout; it is
  not documented in the source. `samsung,tmu_default_trip_temp = <0x73>`
  (115) is in every TMU node, but this kernel parses it and never uses it
  (K L1045).
- Any shutdown driven by skin temperature is SSRM's (closed).

## 3. sec-battery: charging thermal control and the "overheat limit"

All of these run only while a charger is attached:
`sec_bat_temperature_check` returns at once when the status is
`DISCHARGING` (K L1846–1851). `temp_check_count = 1`
(`/battery/battery,temp_check_count`): **one sample** is enough to enter or
leave a state. Samples come once per monitor pass: `battery,polling_time =
<10 30 30 30 3600>` s, by status (unknown, charging, discharging, not
charging, sleep), plus a pass on every event. Power-off charging (lpm) uses
the same values as normal (`*_lpm` = `*_normal` = `*_event` in DT).

| Function | Sensor | Trigger | Action | Recovery | Source |
|---|---|---|---|---|---|
| **Overheat limit** (`HEALTH_OVERHEATLIMIT`) | **USB connector** thermistor (`usb_thermal_source = 2`, so it replaces the battery) | usb_temp **>= 80.0 C** | Status NOT_CHARGING; charger buck off (no current from VBUS); on AFC, the TA is set to 0 V (`vbus_limit`) | usb_temp **<= 75.0 C**, then by battery temperature: GOOD if 5.0..45.0 C, else OVERHEAT | DT `temp_highlimit_threshold_normal = <0x320>`, `temp_highlimit_recovery_normal = <0x2ee>`; K `sec_bat_temperature_check` L1884–1900, L1976–1980, L2048–2061; vbus_limit cleared at usb_temp <= 75 C in `sec_bat_get_temperature_info` L2670–2674 |
| Overheat (`HEALTH_OVERHEAT`) | Battery thermistor | batt **>= 50.0 C** | NOT_CHARGING, buck off (runs from the battery). Once Vbat < 4.15 V, the buck goes back on with charging off (supplied from VBUS) | batt **<= 45.0 C** (and >= 5.0) | DT `temp_high_threshold_normal = <0x1f4>`, `temp_high_recovery_normal = <0x1c2>`; K L1923–1945, L2062–2076 |
| Cold (`HEALTH_COLD`) | Battery | batt **<= 0.0 C** | NOT_CHARGING, charging off (buck stays on) | batt **>= 5.0 C** | `temp_low_threshold_normal = <0x0>`, `temp_low_recovery_normal = <0x32>` |
| Wireless overheat or cold | Battery | >= 45.0 C or <= 0.0 C | As above | <= 40.0 C, >= 5.0 C | `wpc_high_threshold_normal = <0x1c2>`, `wpc_high_recovery_normal = <0x190>`, `wpc_low_*`; K `sec_bat_temperature` L1793–1805 |
| Charger hot (AFC 9 V or PD at max power) | Charger thermistor (`chg_temp`) | chg **>= 46.0 C** | AFC: input 1000 mA, charge 1250 mA. PD: steps down one PDO; at the lowest, input = nv_charge_power / V | chg **<= 44.0 C** | `chg_high_temp = <0x1cc>`, `chg_high_temp_recovery = <0x1b8>`, `chg_input_limit_current = <0x3e8>`, `chg_charging_limit_current = <0x4e2>`; K `sec_bat_check_afc_temp` L561–611, `sec_bat_check_pdic_temp` L613–668 |
| Mix (both hot) | Battery and charger | batt **>= 42.0 C** and chg **>= 50.0 C** (wired, siop 100) | Input ≈ 4350 mV × (topoff_1st + 50 mA) / (0.9 × Vin), at most topoff_1st + 50 mA; skips the other heat controls | batt **<= 39.0 C** | `mix_high_temp = <0x1a4>`, `mix_high_chg_temp = <0x1f4>`, `mix_high_temp_recovery = <0x186>`; K `sec_bat_check_mix_temp` L384–415 |
| Wireless pad (screen off) | Battery (`wpc_temp_control_source = 1`) | >= 38.0 C | Input 600 mA, HV pad Vout to 5 V | <= 36.0 C | `wpc_high_temp = <0x17c>`, `wpc_high_temp_recovery = <0x168>`, `wpc_charging_limit_current = <0x258>`; K `sec_bat_check_wpc_temp` L417–485 |
| Wireless pad (screen on, siop < 100, HV pad) | Battery | >= 38.0 C | Input 450 mA; Vout 5 V below 95 % SOC | <= 36.0 C | `wpc_lcd_on_high_temp = <0x17c>`, `_rec = <0x168>`, `wpc_lcd_on_charging_limit_current = <0x1c2>` |
| Wireless, store mode | Battery | 36.0 C (screen on: 36.0) | 400 mA with the screen on | 34.0 C (screen on: 30.0) | `wpc_store_*` |
| Sleep mode on a pad | n/a | n/a | 500 mA | n/a | `sleep_mode_limit_current = <0x1f4>` |

**There is no `battery,overheatlimit_threshold` or `overheatlimit_recovery`**
in this kernel or in stock's DT. Those names belong to later Samsung
battery drivers. On the S9, `temp_highlimit_*` plays that role, and it reads
the **USB connector** thermistor, not the battery. It is a **charging cut,
not a power-off**: no code in `drivers/battery_v2` powers off or shuts down
(the only `shutdown` symbols are the platform driver `.shutdown` hooks).
The health shows up in userspace as `OverheatLimit`; stock's UI then shows
a "disconnect the charger" warning (UI behaviour inferred).

### Swelling steps (battery thermistor; `CONFIG_BATTERY_SWELLING=y`, CFG)

K `sec_battery.c:sec_bat_swelling_check` (L1513–1650), with currents applied in
`sec_bat_set_charging_current` (L834–863). Float 4.35 V normally
(`chg_float_voltage = <0x10fe>`).

| Step | Enter | Action | Leave |
|---|---|---|---|
| High swelling | batt **>= 41.0 C** (`swelling_high_temp_block = <0x19a>`) | **Charging stops (buck off).** Once Vbat **< 4.00 V** (`swelling_high_rechg_voltage = <0xfa0>`), it recharges at a **4.15 V** float (`swelling_drop_float_voltage = <0x1036>`), **1090 mA** (`swelling_high_temp_current = <0x442>`; 700 mA on a pad, `swelling_wc_high_temp_current`), topoff 175 mA | batt **<= 39.0 C** (`swelling_high_temp_recov = <0x186>`): float back to 4.35 V |
| Cool | batt **<= 15.0 C** (`swelling_low_temp_block_1st = <0x96>`) | 700 mA (`swelling_low_temp_current = <0x2bc>`), float unchanged; when full, recharge at float − 150 mV (K L1383–1390) | batt **>= 20.0 C** (`swelling_low_temp_recov_1st = <0xc8>`) |
| Low swelling | batt **<= 5.0 C** (`swelling_low_temp_block_2nd = <0x32>`) | Charging stops. Below 4.00 V (`swelling_low_rechg_voltage`), recharge at 4.15 V and 700 mA, topoff 175 mA | batt **>= 10.0 C** (`swelling_low_temp_recov_2nd = <0x64>`) |

## 4. ztOS's zrn-charge against stock

`devices/samsung-starlte/overlay/usr/local/sbin/zrn-charge` (header and
L446–490) against the table above:

| zrn-charge | Stock | Verdict |
|---|---|---|
| >= 50 C stop, resume 45 | 50.0 / 45.0, buck off | Matches |
| <= 0 C stop, resume 5 | 0.0 / 5.0 | Matches |
| >= 41 C: 1090 mA (700 on a pad), 4.15 V, back at 39 | Same numbers, **but stock first stops and resumes only below 4.00 V** | Close. zrn-charge charges at once up to 4.15 V. Stock would not charge between 4.00 and 4.15 V when it enters this step. Stock's way is stricter. |
| <= 5 C: 700 mA, 4.15 V, back at 10 | Same, with the same "stop, then recharge below 4.00 V" | Same note |
| <= 15 C: 700 mA, back at 20 | 15.0 / 20.0, 700 mA | Matches |
| 9 V only below 41 C | Stock keeps AFC at 9 V during swelling (buck off). It cuts to 5 V only via siop < 100 or full, and to 0 V via the overheat limit. | zrn-charge is stricter; fine |
| (none) | Charger thermistor 46/44 C limit; mix 42+50/39 C; **USB connector 80/75 C cut** | **Missing** in zrn-charge |

## 5. Low battery and temperature

| What | Value | Source |
|---|---|---|
| Low-temperature empty compensation on | batt (gauge Temp, written from the battery thermistor) **<= 10.0 C** and raw SOC > 5.0 % | `/max77705-fuelgauge/fuelgauge,using_temp_compensation`, `fuelgauge,low_temp_limit = <0x64>`; K `max77705_fuelgauge.c` L1895–1907; the temperature is written by `max77705_fg_write_temp` L250–265 from `sec_battery.c` L2729–2734 |
| Cold (SW) mode | Gauge VEmpty = `v_empty_origin` 0x7d54 (2.50 V empty, 3.36 V recover, decoded per the ModelGauge VEmpty layout, inferred); VALRT at **3.20 V** (`sw_v_empty_voltage = <0xc80>`). On the alert, SOC is forced to 0 while discharging, so Android shuts down at 0 %. Leaves at **3.48 V** (`sw_v_empty_recover_voltage = <0xd98>`) | K `max77705_fg_set_vempty` L202–247, L1909–1917, L106–112 |
| Normal (HW) mode | VEmpty 0xa7e2 (**3.35 V empty, 3.92 V recover**, inferred decoding); VALRT 3.10 V (`sw_v_empty_voltage_cisd = <0xc1c>`) | same |
| Gauge auto-discharge at high temperature | Off: `fuelgauge,auto_discharge_en` is absent, so `discharge_temp_threshold = 600` and `discharge_volt_threshold = 4200` are unused | K L1247–1271, L2425–2440 |
| Battery-temperature power-off | **Not in the kernel.** AOSP `BatteryService.shutdownIfOverTempLocked` shuts down when the battery temperature is **> `config_shutdownBatteryTemperature`**, AOSP default **680 (68.0 C)**, on any health update, charging or not, with no count or timer. That temperature is `battery/temp`, which is the battery thermistor (`sec_battery.c` L4585). Samsung's overlay value is **unverified** (closed framework-res). | AOSP; K L4585 |

## What ztOS uses

zrn-thermald takes two things from stock: the
battery overheat power-off threshold (stock's is the framework's: battery
thermistor above 68.0 C, AOSP default; the kernel only cuts charging, at
USB 80 C) and the TMU throttle steps (BIG IPA target 83 C, big cores off at
96 C and back below 91 C, GPU IPA target 88 C, LITTLE steps at 76, 81, 86,
91, 96 and 101 C with caps from ECT, hardware trip at 115 C).

## ztOS's zrn-thermald

`devices/samsung-starlte/overlay/usr/local/sbin/zrn-thermald`, run by
`zrn-thermald.service` after zrn-therm. Every number is a constant at the top
of the script.

| What | Input | ztOS | Stock |
|---|---|---|---|
| Power off (`systemctl poweroff`) | Battery thermistor | Above 68.0 C on 3 fresh readings in a row: zrn-therm's sample count has to move between them. Unknown or stale resets the count. | Above 68.0 C, one reading (BatteryService) |
| Warning | Battery | 50.0 C, closes below 47.0 | Charging stops at 50 |
| Warning | AP thermistor | 58.0 C, closes below 55.0 | SSRM (closed) |
| Warning | Battery unknown | 3 looks and at least 30 s without a reading | none |
| Throttle level 1/2/3/4 | AP thermistor | 48/52/56/60 C, each lets go 3 C lower | SSRM (closed) |
| | Battery thermistor | 45/50/54/58 C, 3 C | none |
| | TMU big (s9p_cpufreq) | 83/91/96/105 C, 5 C | IPA control 83, cores off at 96 |
| | TMU little | 76/86/96/101 C, 5 C | step_wise 76..101 |
| | TMU GPU | 78/88/105/110 C, 5 C | IPA 78 on, 88 control |
| | Any other thermal zone | as big | |

The levels go up straight to the highest one any input asks for. They come
down one at a time, once every input is under the leaving level's mark by its
fall-back. Unknown inputs neither raise a level nor hold it.

| Level | Little cores | Big cores | GPU cooling state |
|---|---|---|---|
| 1 | 1.79 GHz | 1.79 GHz | 0 (572 MHz) |
| 2 | 1.50 GHz | 1.47 GHz | 1 (455 MHz) |
| 3 | 1.15 GHz | 1.07 GHz | 2 (260 MHz) |
| 4 | 650 MHz | 400 MHz | 2 |

- **zrnperfd** writes `scaling_max_freq` too, for its boost ladder.
  zrn-thermald sends it the ceiling as `thermal-cap LITTLE BIG` (kHz, or
  `off`) on `/run/zrnperfd.ctl`, and zrnperfd applies it to every limit it
  writes. zrnperfd's status shows it as `thermal_cap=LITTLE,BIG`; it isn't
  kept across a zrnperfd restart, so zrn-thermald sends it again whenever the
  status differs.
  - **Fallback:** if zrnperfd isn't running (its pipe has no reader), or is an
    older build without the command, zrn-thermald clamps the limits itself.
  - It keeps zrnperfd's last value as its wish and gives it back when the
    ceiling lifts.
  - While a ceiling is on, it watches `/run` (inotify) for `zrnperfd.status`,
    which zrnperfd rewrites after each change of limits, and clamps again at
    once.
- **The GPU** goes through its devfreq cooling device
  (`cooling_device0`), a separate request in the kernel that doesn't fight
  zrnperfd's `max_freq`. While writing it, the GPU is held awake through
  `power/control`, as zrnperfd does.
- **Cost:** one CLOCK_BOOTTIME timerfd at zrn-therm's own period: 5 s with the
  screen on or a charger in, otherwise 60 s. It never wakes a sleeping phone.
  A timer that comes due during sleep fires at resume, and the first look
  then waits 1 s for zrn-therm's own sample.
- **Notifications** go down the shell's control pipe, as ztnotifyd's do.
  `zrn-thermald`, `zrn-thermald-unknown` and `zrn-thermald-shutdown` are the
  keys. They are sent again quietly when the shell restarts (the pipe changes).
- **The power-off's reason** goes to `/var/lib/zrn-thermald/shutdown`,
  written with fsync, and to `events.log` and the journal before poweroff. At
  the next start it is shown as a notification and moved to `shutdown.last`.
  `zrn-thermald status` prints the status and the last reason.
- **Test mode:** `--dry-run` writes `shutdown.dry-run` and never powers off.
  `--fake FILE` reads the temperatures from FILE (lines like `battery 523` or
  `ap unknown`) and implies `--dry-run`.
- **Host tests:** `python3 devices/samsung-starlte/test-zrn-thermald.py`.

## The night of 2026-11-11/12: where the heat came from

These are read from `/var/lib/zrn-sleep/soc.csv`, `~/battery-float/temp.log`,
the zrn-sleepd log and the journal of that boot (01:31-05:21).

- **The heat source was the AOD keeping the SoC awake.** zrn-sleepd's "awake
  74 %" is all `why=aod showing`. While the AOD shows, zrn-sleepd doesn't
  suspend, and the phone draws 1.7-2.4 W: current_avg -480..-610 mA at
  3.6-3.9 V. In those stretches zrnperfd was at idle0, with the screen off,
  cpu0-1 at 1.15 GHz and the big cluster at its lowest. The power isn't load
  on the cores, it is the awake SoC.

  | AOD stretch | Charge | Battery temperature |
  |---|---|---|
  | 01:31-03:10 | 75→37 % | 41→51 C |
  | 03:41-04:17 | 33→18 % | 43→54 C |
  | 04:52-05:21 | 12→2 % | 46→55.2 C |

  The AOD turned on again at a 5-minute Wi-Fi wake, and at about 05:00 the
  low-battery LED notifications turned the screen on several times.
- **"Asleep 26 %"**: twelve sleeps of 296 s, each costing 11-13 mAh, or about
  155 mA (about 0.57 W). That is high for SoC sleep. The battery cooled by
  1.2-1.5 C per 5 minutes in them. Which rails stay up in sleep (MIF and
  memory) is the rail-map agent's work.
- **Faster heating at low charge, but not from the battery's resistance.**
  At the same temperature the battery warmed 0.7-1.3 C per 10 min in the
  first stretch (73→37 %), and 2.8-3.8 C per 10 min in the second and third
  (31→2 %). The power drawn hardly changed: about 2.0 W, with current_avg
  about -510 mA early and -560 mA late.
  - **Internal resistance:** from 15 current steps (sleep, wake and Wi-Fi rows
    within 20 s, |ΔI| > 150 mA, at 41-51 C), the median is 89 mΩ at 30-60 %,
    96 mΩ at 15-30 % and 110 mΩ under 15 %.
  - **I²R heat:** at 0.55 A that is 27-33 mW, under 2 % of the heat. It
    cannot account for a threefold rise.
  - **Likely cause (inferred):** the phone was losing heat more slowly in the
    later stretches, for example covered or on a bed.
  - The 10 % more current late in the night is the only measured
    contribution from low charge.
- **snd-zrn-abox:** at the time of this check (07:05, charging, another agent
  compiling) all three PCMs were closed. The ~1000 IRQ/s with two streams
  open at idle could not be reproduced. That is the power agent's item.
- The busiest interrupt, 14360000.i2c (the MAX77705 bus: charger, gauge,
  usbc) ran at about 300 IRQ/s while charging. This is noted only.
