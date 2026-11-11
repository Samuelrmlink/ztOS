# Alarms + Clock

`ztclock` is the app (alarms, world clock, timer, stopwatch, as a ztMETRO
pivot). `ztclockd` is a user service that rings alarms and the timer. Because
of the service, alarms ring when the app is closed, the screen is off and the
SoC is asleep.

| piece | where |
|---|---|
| `ztclock` | `/usr/local/bin`, `ztclock.desktop`, `ztclock.svg` |
| `ztclockd` | `/usr/local/bin`, file capability `cap_wake_alarm+ep` |
| `ztclockd.service` | `/usr/lib/systemd/user`, `WantedBy=default.target` |
| inhibit ACL | `devices/samsung-starlte/overlay/etc/tmpfiles.d/ztclock.conf` |
| shared format | `clock-store.h` (every file is described there) |

Install with `make install-clock` (or `make install`), then
`systemctl --user enable --now ztclockd`. On the phone, copy `ztclock.conf`
to `/etc/tmpfiles.d` and run `systemd-tmpfiles --create`.

## How an alarm gets from the app to the speaker

1. The app writes `~/.config/ztclock/alarms` or `timer`, under a flock, and
   replaces the file atomically with a rename.
2. ztclockd sees the change through inotify on that directory. It works out
   the soonest thing due: an enabled alarm's next time, a snooze, or the
   running timer. It arms one timerfd for that time.
3. The timerfd is `CLOCK_REALTIME_ALARM`, absolute, with
   `TFD_TIMER_CANCEL_ON_SET`. As the phone suspends, the kernel's alarmtimer
   programs the RTC (zrn-exynos-rtc, `rtc0`) with the soonest
   `*_ALARM` timer, so the SoC wakes for it. This is the path Android's
   `*_WAKEUP` alarms use, and power/suspend/README.md shows it waking SYS_SLEEP
   on time. The SoC-sleep side (zrn-sleepd, the RTC) belongs to the power work,
   not to this app.
4. On expiry, ztclockd takes the steps below, in this order:
   - writes `/run/zrn-sleepd/inhibit/timed-ztclock`, whose mtime is the end of
     the ring plus 30 s, so a crash cannot hold the phone up forever;
   - writes `$XDG_RUNTIME_DIR/ztclock.next` and `ztclock.ring`;
   - sends `screen on` and then `alarm_screen` to the shell's control pipe;
   - loops `pw-play`, as ztphone rings, rising from 30 % to the alarm volume
     over 30 s;
   - starts vibrating, using the ring pattern from core/zrnvibe.h.

   Snooze, dismiss and the auto-stop (`autostop` minutes, 10 by default)
   remove the inhibit. They also stop the sound and the motor and send
   `alarm_screen end`.

### Why a file capability

`CAP_WAKE_ALARM` is needed for `*_ALARM` timerfds. A `systemd --user` unit
cannot grant `AmbientCapabilities=`, because the user manager does not hold
the capability. A root helper would be one more daemon. `setcap
cap_wake_alarm+ep` on the binary gives exactly this one capability, and only
to this program. The unit must not set `NoNewPrivileges=`, which would drop
the capability. Without the capability, ztclockd falls back to
`CLOCK_REALTIME` and logs that it did. That timer cannot wake a sleeping SoC.

### Why the inhibit needs an ACL

`/run/zrn-sleepd/inhibit` belongs to root. The tmpfiles rule gives the
`wheel` group (the phone's user) `rwx` through an ACL, so ztclockd can write and remove
`timed-ztclock`. The rule leaves the owner and mode alone.

## Reboots, clock changes, time zones and DST

- **Reboot:** the unit starts with the user manager and re-arms from the files.
  A timer that ran out while the phone was off rings at start if it ended
  within the last 30 minutes.
- **Clock set** (NTP after boot, by hand): the cancel-on-set timerfd returns
  `ECANCELED`. Everything due between the last check and now rings, up to 30
  minutes back, and the timerfd is armed again. If the clock is set back,
  nothing rings twice.
- **Time zone:** inotify on `/etc` watches `localtime`. glibc is made to read
  the zone again (TZ set, then unset), and the schedule is worked out again.
- **Daylight saving:** each alarm's next time comes from `mktime` for that
  day with `tm_isdst = -1`, so 07:00 stays 07:00 local on both sides of a
  change. A time skipped in spring (02:30) rings at 03:30.

One-time alarms are switched off in the file when they ring. Their snooze
still comes: a snooze is dropped only when its alarm is deleted, or when the
app sends `unsnooze ID` after someone switches the alarm off, edits it or
deletes it.

## The shell

`alarm_screen` and `alarm_screen end` are new words on
`$XDG_RUNTIME_DIR/ztmetro.ctl`. They do what `call_screen` does for a call:
`ztclock --ring` (app_id `ztclock-ring`) shows over the lock screen and takes
touches there. The existing `call_over_lock` now applies to whichever app is
in `over_lock_app`. A call under way keeps the screen.

A shell without these words ignores them. In that case, if no ring screen has
said `hello` after 8 s, ztclockd starts `ztclock --ring` itself. It then shows
once the phone is unlocked, and the sound and vibration have gone on
regardless.

### Stopping it: a slide, not a tap

A finger that lands on the ring screen by chance must not end an alarm, so
there are no buttons on it. There are two tracks, each with a square handle:

- **stop**, the lower track: the accent handle starts at the left and is
  slid right past 85 % of the track;
- **snooze N min**, the upper track (alarms with a snooze only): the grey
  handle starts at the right and is slid left past 60 %, which is easier.
  The alarm rings again anyway.

The handle follows how far the finger moves, not where it is, so a touch
at the far end of a track does nothing. A click is felt once the handle
is far enough. Letting go there snoozes or stops. Letting go short of it,
or a cancelled touch, springs the handle home. A tap only nudges it.

### The keys while it rings

The shell handles these, since the app never sees the phone's keys. A key
never stops an alarm.

| key | alarm with a snooze | timer, or no snooze |
|---|---|---|
| volume up / down | snoozes it | nothing (it keeps ringing) |
| power, screen on | snoozes it | nothing (it keeps ringing, the screen stays on) |
| power, screen off | wakes the screen | wakes the screen |

While it rings, the shell also keeps the screen on past its idle time, since a
dark screen takes no touch and the slide could not be found. The volume keys
don't change the volume while it rings. A key that snoozes
gives a heavy tick, and one that does nothing gives a tap.

The Metro lock screen also shows the next alarm, with a small bell and its
time, after the date. It reads the time from `alarm` in `ztclock.next`.

## Testing

```
ztclockd --status                       # what is armed
journalctl --user -u ztclockd           # arm / ring / stop lines
ztclock --render alarms|edit|world|timer|stopwatch|sound|zones|ring out.png
```
