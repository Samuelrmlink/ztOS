#!/usr/bin/env bash
# Screen and visible-window freeze policy for the Waydroid payload.
set -euo pipefail
. "${ZTOS_USER_SH:-/usr/lib/ztos/user.sh}"
config=$ZTOS_HOME/.config/ztos/android.conf
background_apps=$ZTOS_HOME/.config/ztos/background-apps
# Android keeps running out of sight for a Waydroid app the user chose:
# Settings > Background apps (background-apps), or keep_awake in android.conf.
# These are the choices ztos-waydroid-network gives Android's background rights
# for and zrnperfd keeps a background Waydroid running for. Reading keep_awake
# alone froze Android under an app chosen in Settings, so Spotify went silent as
# soon as it left the screen (ROADMAP 8.54).
keep_awake_active() {
	[[ -r $config || -r $background_apps ]] || return 1
	python3 - "$config" "$background_apps" "$ZTOS_HOME" <<'PY'
import json, pathlib, sys

packages = set()
try:
    for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
        key, separator, value = line.partition('=')
        if separator and key.strip() == 'keep_awake':
            packages.update(item.strip() for item in value.split(',') if item.strip())
except OSError:
    pass
try:
    entries = pathlib.Path(sys.argv[2]).read_text().splitlines()
except OSError:
    entries = []
# The IDs Settings writes, read as ztos-waydroid-network and background-apps.h do.
for entry in entries:
    item = entry.strip().rsplit('/', 1)[-1]
    if not item or item.startswith('#'):
        continue
    if item.endswith('.desktop'):
        item = item[:-8]
    if item.startswith('waydroid.'):
        packages.add(item[9:])
    elif item.startswith('ztos-android-') and item.endswith('-waydroid'):
        packages.add(item[13:-9])
    elif item.endswith('-waydroid'):
        packages.add(item[:-9])
try:
    apps = json.loads(pathlib.Path(sys.argv[3], '.local/share/ztos/android/applications.json').read_text())
except (OSError, ValueError):
    sys.exit(1)
sys.exit(0 if any(apps.get(package+'@waydroid', apps.get(package, {})).get('runtime', 'atl') == 'waydroid' for package in packages) else 1)
PY
}
# Android is playing sound: its Pulse stream (application.name Waydroid) is open
# and not corked. A freeze then cuts the music mid-track while the app's media
# tile still says Playing (a frozen Android can post no change), so Android keeps
# running until the sound stops. Its display still sleeps: the app is stopped,
# as on a phone with the screen off, and only its media service plays on.
android_playing() {
	timeout 3 pactl -f json list sink-inputs 2>/dev/null | python3 -c '
import json, sys
try:
    streams = json.load(sys.stdin)
except ValueError:
    sys.exit(1)
sys.exit(0 if any(s.get("properties", {}).get("application.name") == "Waydroid"
                  and not s.get("corked", True) for s in streams) else 1)'
}
# One finite watcher while Android plays out of sight. It sleeps in `pactl
# subscribe` (no polling). Once Android has been quiet for the hold (the gap
# between two tracks is not the end) it applies the policy again, which freezes.
# It also ends when Android is in front again or the audio server goes.
audio_watch_start() {
	local lock=$runtime/ztos-waydroid-audio-watch.lock
	( exec 8>&- 9>"$lock"
	  flock -n 9 || exit 0
	  hold=${ZTOS_WAYDROID_AUDIO_HOLD:-10}
	  coproc events { exec timeout "${ZTOS_WAYDROID_AUDIO_WATCH_MAX:-86400}" pactl subscribe 2>/dev/null; }
	  events_fd=${events[0]}
	  quiet= resync=0
	  while :; do
	      [[ $(cat "$policy" 2>/dev/null) == active ]] && break
	      if android_playing; then quiet=; else quiet=${quiet:-$SECONDS}; fi
	      if [[ -n $quiet ]] && (( SECONDS - quiet >= hold )); then resync=1; break; fi
	      if [[ -n $quiet ]]; then
	          left=$(( hold - (SECONDS - quiet) ))
	          (( left > 0 )) || left=1
	          status=0
	          IFS= read -r -t "$left" line <&"$events_fd" || status=$?
	          # a timeout (>128) is the hold running out; anything else, the server went
	          if (( status != 0 && status <= 128 )); then android_playing || resync=1; break; fi
	      else
	          IFS= read -r line <&"$events_fd" || { android_playing || resync=1; break; }
	      fi
	  done
	  kill "${events_PID:-}" 2>/dev/null || true
	  exec 9>&-
	  if (( resync )); then exec "$BASH" "$0" sync; fi
	) </dev/null >/dev/null 2>&1 &
}
freeze_unless_kept() {
	keep_awake_active && return 0
	# A watcher is holding Android through a gap between tracks: it freezes
	# Android itself when the hold ends, so another focus change must not now.
	if [[ -e $runtime/ztos-waydroid-audio-watch.lock ]] &&
	   ! flock -n "$runtime/ztos-waydroid-audio-watch.lock" true; then
		return 0
	fi
	if android_playing; then
		audio_watch_start
		return 0
	fi
	sudo -n /usr/local/libexec/ztos-waydroid-recover freeze
}
launch_in_progress() {
	local marker=${XDG_RUNTIME_DIR:-$ZTOS_RUNTIME_DIR}/ztos-waydroid-launching pid=
	if [[ -r $marker ]]; then
		read -r pid < "$marker" || true
		if [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then return 0; fi
		rm -f "$marker"
	fi
	return 1
}
# zrn-pushd thaws Android to hand an app a push message, and holds off the
# freeze until the app has fetched it (the file holds the epoch it ends at);
# it runs `sync` itself when the hold is over.
push_hold_active() {
	local hold=$runtime/ztos-waydroid-push-hold until=
	[[ -r $hold ]] || return 1
	read -r until < "$hold" || true
	if [[ $until =~ ^[0-9]+$ ]] && (( until > $(date +%s) )); then return 0; fi
	rm -f "$hold"
	return 1
}
# Waydroid not set up yet (no images, or a kernel without binder): nothing to
# freeze, and nothing to confirm. Success, quietly, so the shell has no warning.
[[ -e ${ZTOS_WAYDROID_CFG:-/var/lib/waydroid/waydroid.cfg} ]] || exit 0
runtime=${XDG_RUNTIME_DIR:-$ZTOS_RUNTIME_DIR}
policy="$runtime/ztos-waydroid-power-state"
case ${1:-} in
    active|idle|screen-off)
        printf '%s\n' "$1" > "$policy.$$"
        mv -f "$policy.$$" "$policy"
        ;;
    sync) [[ -r $policy ]] || exit 0 ;;
    screen-on) exit 0 ;;
    *) exit 2 ;;
esac
# One finite worker at a time; stale children read current compositor policy.
exec 8>"$runtime/ztos-waydroid-power.lock"
flock -w 30 8
for ((pass=0; pass<3; pass++)); do
    state=$(cat "$policy")
    case $state in
        # Android's display is on while one of its windows is in front, and
        # asleep otherwise, before any freeze: a hidden app is stopped, not
        # left resumed (see ztos-waydroid-recover display).
        active)
            sudo -n /usr/local/libexec/ztos-waydroid-recover unfreeze
            sudo -n /usr/local/libexec/ztos-waydroid-recover display on
            ;;
        idle)
            launch_in_progress && exit 0
            sudo -n /usr/local/libexec/ztos-waydroid-recover display off
            push_hold_active && exit 0
            freeze_unless_kept
            ;;
        screen-off)
            sudo -n /usr/local/libexec/ztos-waydroid-recover display off
            push_hold_active && exit 0
            freeze_unless_kept
            ;;
        *) exit 2 ;;
    esac
    [[ $(cat "$policy") == "$state" ]] && exit 0
done
