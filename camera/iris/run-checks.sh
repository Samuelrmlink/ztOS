# Shared preconditions for run-fp.sh and run-iris.sh (sourced, POSIX sh).
D=${D:-/var/lib/zrn-iris}

checks() {
	what=$1
	[ "$(id -u)" = 0 ] || { echo "$what: run with sudo"; return 1; }
	mkdir -p "$D"
	if [ -e "$D/doing" ]; then
		echo "$what: an earlier run did not finish: $(cat "$D/doing"). Not running; read the logs, then remove $D/doing by hand."
		return 1
	fi
	case "$(cat /run/ztbench/owner 2>/dev/null)" in
	"iris since"*) ;;
	*) echo "$what: not inside 'ztlock run iris'"; return 1 ;;
	esac
	h=$(date +%H)
	[ "$h" -ge 7 ] || { echo "$what: before 07:00 ($(date +%T %Z))"; return 1; }
	cap=$(cat /sys/class/power_supply/max170xx_battery/capacity 2>/dev/null || echo 0)
	[ "$cap" -ge 15 ] || { echo "$what: battery $cap % < 15 %"; return 1; }
	if [ -d /sys/module/zrn_cam ]; then
		echo "$what: zrn_cam is loaded; the camera must be idle and unloaded"
		return 1
	fi
	echo "$what: checks passed: $(date +%T), battery $cap %, $(uname -r)"
	return 0
}

mark() { printf '%s\n' "$*" > "$D/doing"; sync; }
unmark() { rm -f "$D/doing"; sync; }
