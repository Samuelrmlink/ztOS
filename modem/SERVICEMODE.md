# Samsung ServiceMode on the S9's modem (ROADMAP 8.38)

What `*#2263#`, `*#0011#`, `*#9090#` and `*#197328640#` are, how the stock
phone drives them, what ztOS does with them (look, never change), and the
changes that would need the owner's approval.

Each claim is marked **[observed]** (seen on this phone), **[source]** (read
in the stock code or in libsamsung-ipc) or **[inferred]**.

Sources:
- `ModemServiceMode.apk` (package `com.sec.android.RilServiceModeApp`,
  "ServiceModeApp_RIL") from the phone's read-only SYSTEM image,
  `/system/priv-app/ModemServiceMode/`, decompiled with androguard 4.1.4 on
  the PC in a scratch directory (not kept). `serviceModeApp_FB.apk` beside
  it is the AP side's (`*#9900#` SysDump and the like), not the modem's.
- `libsec-ril.so` (64-bit, `/vendor/lib64`), which has symbols:
  `Ipc41PacketBuilder::BuildIpcSvcMode*`, `IpcProtocol41::IpcTx*`,
  `ServiceModeManager::OnOemServiceMode`; disassembled with llvm-objdump 20.
- libsamsung-ipc (Replicant), `include/svc.h`.

## How it works

The menus are the CP's. The AP only enters a mode, presses keys and leaves;
the CP sends each screen back as lines of text. **[source]**

Stock path: the dialer broadcasts `android.provider.Telephony.SECRET_CODE`
for `*#CODE#`; `SecKeyStringBroadcastReceiver` (manifest: 48 codes) starts
`ServiceModeApp` with the code; `StartServiceMode()` turns the code into a
mode and sub type and sends an OEM hook request (`invokeOemRilRequestRaw`
through `com.sec.phone`'s SecPhoneService) to libsec-ril, which turns it
into Samsung IPC on `umts_ipc0`. **[source]**

The OEM hook layout (`ServiceModeApp$OemCommands`, big-endian):

| request | bytes |
|---|---|
| enter | `01 01 00 08` modem-type(4 GSM) mode sub query |
| key | `01 03 00 07` modem-type key query |
| end | `01 02 00 06` modem-type mode |

`query` = 1 is the app's 1.5 s refresh: libsec-ril answers it from the last
screen it cached and sends nothing to the CP
(`ServiceModeManager::OnOemServiceMode`). So the CP pushes screens on its
own; nothing polls it. **[source]**

The IPC messages (`Ipc41PacketBuilder`, `IpcProtocol41::IpcRxServiceMode`;
names from libsamsung-ipc): **[source]**

| message | dir | data |
|---|---|---|
| `0b01` SVC_ENTER, set | AP→CP | mode, sub type, magic. Sub type and magic `0x10` only for mode 1; mode 4 sends `04 00 00` |
| `0b02` SVC_END, set | AP→CP | the mode: 4 after the monitor, 1 after any other |
| `0b03` SVC_PRO_KEYCODE, set | AP→CP | one key: a menu line's character (`'1'` = 0x31), `'\'` (0x5c) back; typed values are their characters then `'S'` (0x53) |
| `0b05` SVC_DISPLAY_SCREEN | CP→AP | count, then count × (2-byte index, 32 chars). The whole screen. A screen of count 1 with index bytes `00 01` is dropped |
| `0b16` (wide) | CP→AP | count × (2, 40 chars) |
| `0b06` SVC_CHANGE_SVC_MODE | CP→AP | the mode now |
| `0b0a`..`0b13` | CP→AP | call-drop log, LTE scan file, RAM-dump status, big data, automation info, fake-cell info, CP logging, LTE band info, diag scan (handlers named in libsec-ril; not used here) |

## The codes

From `ServiceModeApp.StartServiceMode()` (bytecode; SendData(1, mode,
sub)). The sub type goes out as one byte, so 4131 (0x1023) is sent as 0x23.
**[source]**

| code | mode | sub | stock name (`OemCommands`) | here |
|---|---|---|---|---|
| `*#0011#` | 4 | 0 | MONITOR (field test; Korean CSCs open a different app) | allowed, keys filtered |
| `*#197328640#` | 1 | 0 | TEST_MANUAL, main menu | allowed, keys filtered |
| `*#2263#`, `*#22632#` | 1 | 0x16 | BAND_SEL_ENTER | allowed, **look only** |
| `*#9090#`, `*#7284#` | 1 | 0x13 | USB_UART_DIAG_CONTROL_ENTER | allowed, **look only** |
| `*#0599#`, `*#301279#`, `*#279301#` | 1 | 0x14 | RRC_VERSION_ENTER (HSPA control) | not offered |
| `*#27663368378#`, `*#276633683782#` | 1 | 0x23 | TSP_CP_DEBUGSCREEN | not offered |
| `*#37375625#` | 1 | 0x25 | TST_FREQ_LOCK_MODE_ENTER (frequency / cell lock) | not offered: changes |
| `*#2683662#` | 1 | 62 | LAB_TEST_MODE | not offered |
| `*#0228#` | 1 | 5 | BATTERY_INFO | not offered |
| `*#32489#` / `*#2580#` | 1 | 6 / 7 | CIPHERING / INTEGRITY_PROTECTION | not offered |
| `*#4238378#` | 1 | 23 | GCF_TESTMODE | not offered: changes |
| `*#1575#` | 1 | 21 | GPSONE_SS_TEST | not offered |
| `*#1111#` / `*#2222#`, `*#8888#` | 1 | 0x02 / 0x03 | FTA SW / HW version | not offered |
| `*#58366#` | 1 | 36 | SET_LTE_ON | **off limits** (acts on entry) |
| `LTE_ANT_PATH_NORMAL` | 1 | 34 | SET_LTE_ANT_PATH_NORMAL | **off limits** |
| `*#6984125*#`, `*#2886#` | 1 | 32 | TST_AUTO_ANSWER | **off limits** |
| `*#73876766#` .. `*#4387264636#` | 1 | 27..31 | SELLOUT_SMS | **off limits** (sends SMS) |
| `*#2767*2878#` | 1 | 33 | TST_NV_RESET | **off limits** (NV) |
| `*#33725327#` | 1 | 0x27 | EFS_CLEAR (factory binaries only) | **off limits** (EFS) |
| `*#66336#`, `CP_RAMDUMP` | 1 | 0x22 / 0x24 | CP RAM-dump mode / execute | **off limits** (crashes the CP) |
| `*#758353266223#` | 1 | 58 | SUIMPROBE | **off limits** |
| `*#1234567890#` | 1 | 59 | HOSTLINK | **off limits** |
| `*#119#` | 1 | 60 | ALERT_ERASE | **off limits** |
| `*#73#` | 1 | 61 | DIRECT_RF_TEST | **off limits** (RF) |
| `*#548378#` | 1 | 0x28 | 4RXD_TEST | **off limits** (RF) |
| `*#622#` | 1 | 0x3f | MCC_BACKOFF_TEST | **off limits** |
| `*#6201#` | 1 | 64 | LOGGING_SETTING | not offered: changes |

Others in the same app are not ServiceMode screens: `*#147852#` etc. (test
APNs), `*#745#`/`*#746#` (RIL dump), `*#3214789#` (GCF mode settings),
`*#22553767#` (call-drop log), `*#36764#` (CLAT). **[source]**

## What ztOS does

`ztmetro/modemd/servicemode.h` in ztoscorenetworkingd, on
`/run/ztoscorenetworking.ctl`:

```
svc enter 0011|197328640|2263|9090
svc key N        a digit, only as below
svc back         0b03 '\'
svc end          0b02
svc ping         the app is still open
svc netinfo      GETs only: 080a, 0830, 0833
```

Screens go to `/run/ztoscorenetworking/svc` (tmpfs: `state`, `code`, `note`,
`allow` = the digits that would pass, `line=` per line). The journal gets
counts only, never a screen's text; `0b` messages are not hex-logged even
with the debug file.

Guards:
- only the four codes above; anything else is refused before it reaches the
  CP;
- a digit only where the screen has that `[N]` line, the line names nothing
  on the deny list (whole words: NV, IMEI, RF, CAL, FACTORY, RESET, LOCK,
  BAND, RAT, MODE, ON, OFF, SET, AUTO, CONTROL, LOG, DIAG, TEST, …), the
  mode allows digits at all (`*#2263#` and `*#9090#` do not: their lines
  may themselves be settings), and the screen the last key asked for has
  come; never `'S'` (typed values), `'*'` or letters;
- left on `svc end`, 90 s after the last command (the app pings every 30 s
  while open), when the screen goes off, and before another mode; a modem
  start forgets it.

`ztservicemode` (Metro app, `ztmetro/servicemode/`): the four codes and
"network settings"; a code's page shows the modem's screen as it changes
(inotify on the file; no polling), menu lines the daemon would take in the
accent colour, and a keypad (digits, back, end). `ztservicemode 0011` or
`ztservicemode '*#0011#'` opens one directly. `ZT_SVC_REDACT=1` masks
identities for screenshots.

`make check-servicemode`: the messages and the key rules with no modem.

## Changes that need approval (none applied)

How the stock phone stores each, and what it would mean here. Under ztOS
the CP's NV saves (RFS `0x02` NvWrite) are acknowledged, or not answered,
but never stored (README, "The CP's IPC going silent"), and cbd loads
`nv_data.bin` from EFS at every modem start. So on ztOS any CP-side change
lasts until the next modem start, while the same change on stock Android
would be written to EFS. **[source + inferred]** One caveat: cbd itself
can write EFS (it has `-n /mnt/vendor/efs`); whether it ever saves the CP's
NV on its own (on a CP crash or a shutdown) is not known. **[inferred]**

| change | exact message | stock: kept where | on ztOS | undo |
|---|---|---|---|---|
| RAT preference (2G/3G/4G), the plain one | `080a` NET_MODE_SEL set, 1 byte mask: 1 GSM, 2 WCDMA, 4 CDMA, 8 LTE, 0x10 TD-SCDMA, 0x20 EVDO, 0x40 NR. LTE/3G/2G = `0b`, 3G/2G = `03`, 2G = `01`, 3G = `02`, 4G = `08`, 4G/3G = `0a` (`ConvertPreferredNetTypeToIpcWithBitmask`) **[source]** | Android's Settings DB (`preferred_network_mode`), sent again at every boot; the CP may also keep it **[inferred]** | ztoscorenetworkingd already sends `0b` at every modem start (`net_configure`, and `03` for Hutch); a change lasts until the next modem start **[source]** | `080a` set `0b` (or restart the modem) |
| legacy 2G/3G band mode (`RIL_REQUEST_SET_BAND_MODE`) | `0807` NET_BAND_SEL set, 25 bytes: `02`, u64 band mask LE, 16 × 0. Masks: Euro 0x16, USA 0x129, Japan 0x210, Australia 0x116, Australia-2 0x106; anything else (automatic) 0x0fffffff (`IpcTxNetSetBandMode`) **[source]** | CP NV **[inferred]** | until the next modem start | `0807` with 0x0fffffff |
| LTE band selection (stock hidden menu / `*#2263#`) | `0830` set, 1033 bytes: u16 length, then up to 1024 bytes of band table (`IpcTxSetExtendedBandMode`; layout of the table not read: take it from a `0830` GET first) **[source]**. Through the CP menu: `*#2263#`, then the band's `[N]` keys **[inferred: not navigated]** | CP NV **[inferred]** | until the next modem start | write back the table the GET returned; or the menu's "automatic"/all bands |
| cell lock (EARFCN + PCI) | `0833` NET set, 7 bytes: lock u8 (1 on, 0 off **[inferred]**), PCI u16 LE, EARFCN u32 LE (`IpcTxNetSetPciEarfcnLockIpc`); a second form `221f` (hidden menu) with the same 7 bytes (`IpcTxHiddenSetPciEarfcnLock`) **[source]**. Through the CP menu: `*#37375625#` (TST_FREQ_LOCK_MODE) and typed values ending in `'S'` **[source, not navigated]** | CP NV **[inferred]** | until the next modem start | `0833` with lock 0 (and the GET's old values) |

The current values can be read without changing anything: `svc netinfo`
sends `080a`, `0830` and `0833` as GETs and puts the answers, in hex, in the
svc file (and on the app's home page).

Permanently out of scope, whatever the menu: IMEI and NV editing
(`*#2767*2878#`, NV REBUILD and the like), unlocking, RF calibration
(`*#73#`, RF NV menus), factory reset and EFS clear (`*#33725327#`).

## On the phone

Not run on the phone yet (2026-11-12): the daemon and app are built there
and pass host tests; the install was queued under `ztlock run servicemode`
and paused at the coordinator's request before any message went to the CP.
With no SIM, `ztoscorenetworkingd` never asks `AT+CIMI`, so neither
`net_configure` (`080a` 0x0b) nor the telephony `AT+CRSM` reads run.

To run: `~/svc-run.sh` style sequence, `svc netinfo`, then for each code
`svc enter CODE`, wait for `screens>0`, copy the svc file, `svc end`;
redact before anything leaves the phone.
