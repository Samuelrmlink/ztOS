# The always-on display in a pocket (ROADMAP 8.44)

**The owner asked for:** "Turn off the always-on display when the proximity sensor detects that the phone is in an enclosed area like a pocket. Periodically check the proximity sensor every couple of minutes and turn the AOD back on if no longer in an enclosed area. Check what Samsung's android does and replicate that."

**Why it matters:** the shell draws the AOD, so the display output stays enabled and zrn-sleepd never puts the SoC to sleep while it shows (README, "What it costs"). Overnight on 2026-11-12 that kept the phone at about 2 W, and the battery reached 55 °C.

## What stock does (G960FXXUHFVG4, One UI 2 / Android 10)

Read from the decompiled stock firmware: `AODService_v50.apk`, SystemUI, `framework.jar`, `semcontextservice.jar`, `motionrecognitionservice.jar` and SettingsProvider, plus Samsung's starlte kernel (`drivers/sensorhub/brcm`, branch `tw10-android-4.9-q`). The bytecode readings have not been checked on a device. "Inferred" marks what the code implies but does not show; the hub's firmware is a binary.

**The sensor hub decides, not Android.**
- The AOD app registers for SemContext type **51, CARRYING_DETECTION**. Inside it is called DPCM, "Device Physical Context Monitor" (`SemContextMonitor.registerInner`).
- DPCM is an algorithm in the hub's firmware (library type 56). It combines motion, orientation and proximity.
- The AP only receives `{status, reason}`, two bytes (`DevicePhysicalContextMonitorRunner.getParseSyntaxTable`).

**What turns the AOD off:**
- Status 2 (off), with one of these reasons:
  - 8, `REASON_OFF_SCREEN_DOWN_START_STATE` (face down);
  - 9, `REASON_OFF_CARRYING_IN` (a pocket or bag).
- The AOD app then goes RUN → ENDING → ENDED → STANDBY (`AlwaysOnDisplayEx.handleStopAODBySensor`). It holds a 2 s "off by sensor" token (`setAODOffBySensor`, `sendEmptyMessageDelayed(2, 2000)`).
- STANDBY asks SystemUI for `DOZE_AOD_PAUSED`, whose screen state is `Display.STATE_OFF`. SystemUI applies it at once (`DozeScreenState.applyScreenState`).
- Reasons 6, 7 and 10 (still for a long time, or running) are logged as "Wrong Sensor event. SKIP."

**What brings it back:**
- Status 1, with reason 2, 3 or 4: moved face down, moved face up, or carried out.
- The app is in STANDBY and restarts the AOD at once (`handleStartAODBySensor`). There is no delay on the AP side.

**How it is checked:**
- Nothing polls on the AP. The listener stays registered while the AOD runs and while it stands by; the hub pushes each change, and its interrupt wakes the AP (inferred). No AP alarm was found.
- The AP sends the hub these parameters:

  | Parameter | Value sent | Setting it reads (and ignores) |
  |---|---|---|
  | `AOD_OVERTURN_DURATION` | 10 | `dpcm_sensor_aod_over_turn_duration`, default 3 |
  | `AOD_PROXIMITY_USE_DURATION` | 30 | `dpcm_sensor_aod_proxi_use_period`, default 60 |
  | `AOD_PROXIMITY_CHECK_DURATION` | 1 (from the setting) | `dpcm_sensor_aod_proxi_check_duration` |

  The code's own log reads "OVER_TURN_DURATION(Checking 3 times of x period)10s, PROXI_USE_PERIOD 30 s, PROXI_CHECK_DURATION 1 s".
- So the hub likely turns proximity on for about 1 s every 30 s, and wants about 10 s face down before it reports (inferred).
- The scenario mask (`aod_scenario_check`, default 110) switches on overturn, proximity periodically, walking, vehicle and cycling.

**No light threshold, and no user switch:**
- The AOD path uses no light reading.
- There is no switch for the user. `debug.aod.disable_sensor_scenario` turns it off, and a closed flip cover skips it.

**What is not involved:**
- AOSP's doze proximity (`DozeSensors`, `ProximityCheck`): SystemUI's `DozeFactory` doesn't build it.
- PowerManager.
- Samsung's "accidental touch protection" (`screen_off_pocket`, on by default). That feature blocks wake-ups and touches in a pocket but doesn't hide the AOD. It is the one with lux: `MIN_LIGHT_POCKET_OUT = 5`, `POCKET_RECOGNITION_TIME = 3000` ms.
- The kernel's `PROXIMITY_POCKET` (sensor 35, 3 bytes) and the HAL's PocketMode sensors.

## What ztOS does

The decision is the same: hide the AOD while the phone is in a pocket or face down, and bring it back when that ends. How the decision is made differs, because talking to the hub's DPCM library (SContext packets) isn't open to us. Its protocol is undocumented, and the hub's firmware asserts at the slightest misstep (sensors/README.md).

- **`ztoscsf-sensord` owns the "covered" state.**
  - The shell sends "pocket on" while the AOD would show: the screen is off and nothing else keeps it dark.
  - sensord then checks **at once and every 2 minutes** of the phone's time (CLOCK_BOOTTIME).
  - A check turns proximity and light on, takes the first reading of each (both report as soon as they are added), and turns them off again. On the host that is about a quarter of a second; it gives up after 1.5 s. The thresholds go first, as Samsung's driver sends them.
  - Nothing streams between checks.
- **Covered means proximity near and under 5 lux.**
  - The 5 lux is our addition. It is the same figure as stock's accidental-touch pocket-out (`MIN_LIGHT_POCKET_OUT`). Stock's hub uses motion as its second opinion; we use the light, so a hand near the top of the screen in a lit room doesn't hide the AOD.
  - If light never answers, proximity decides alone.
  - If proximity never answers, nothing changes: the AOD shows.
- **The shell is told only of changes:** "pocket covered" or "pocket clear". It keeps that as `aod_covered`, and `aod_why` gains "the phone is covered (a pocket, or face down)" after every other reason. When covered, the output is switched off as for any other dark, so zrn-sleepd may sleep.
  - The decision is a separate hook, independent of how the AOD is drawn. A panel-held AOD (8.45) can be turned off by the same `aod_covered`.
  - Turning the screen on forgets it. The next screen-off shows the AOD and checks at once.
- **While the SoC sleeps, a check that falls due waits for the next wake.** That can be a resume (sensord follows `resume_seq`), a doze window or anything else; no wake is added for it. Asleep in a pocket, the AOD therefore comes back at the first wake after the phone leaves the pocket:
  - up to 5 minutes in a doze schedule (`DOZE_SLEEP_S`);
  - at most 30 minutes otherwise (`MAX_SLEEP_S`).

  Stock brings it back at once, because its hub wakes the AP. Ways to get closer:
  - **An alarm wake every 2 minutes.** Costs a full resume each time. zrn-sleepd keeps the phone up 3 s for its own RTC wake, but 20 s for someone else's alarm (`AWAKE_AFTER['alarm']`); Wi-Fi is rebound and the audio DSP restarted. Up to 30 a hour.
  - **Proximity left on in the hub while the SoC sleeps.** The hub's `host_req` is already a wake source, so only a change wakes the AP, as on stock. It costs the hub's sampling, and a wake at every flicker in a pocket.

  Neither is built. The second is closer to stock.
- **Awake (on a charger, or the AOD showing), checks run every 2 minutes** on sensord's own timer. Stock's hub looks every 30 s (inferred).
- **What the owner sees:**
  - `zrn-sleep-why` (and `zrn-sleepd status`) shows `note=AOD hidden: phone covered`.
  - The shell's log has "aod: the phone is covered" and "screen dark, AOD not shown: the phone is covered (...)".
  - `/run/ztoscsf-sensors/status` has `pocket=on|off covered|clear|unknown CHECKS`.
  - `ztmetro.state` has `aod_covered yes|no`.
- **The AOD's look is unchanged.**

## Tests

- **Host:**
  - `make -C ztmetro check-sensord` runs `sensord/test-sensord-pocket.py`: checks at once and every interval, nothing between checks, near and dark, near and lit, far, no reading, no light, pocket off, stop while covered.
  - `power/suspend/test-sleep-why.py` checks the note.
  - The shell side ran in the headless container (`desktop/dev/run.sh`):
    - "pocket covered" hides the AOD and "pocket clear" shows it;
    - it is ignored with the screen on or the AOD switched off;
    - the screen coming on forgets it.
- **Phone:** see the report of the 8.44 deploy.
