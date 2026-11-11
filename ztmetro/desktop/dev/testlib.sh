# testlib.sh, shared by desktop/dev/test-*.sh: in the container (run.sh --test) or on the
# phone (desktop/dev/phone-check.sh). What the tests need to know, from the
# shell's own state file, and the tools, from here or from $PATH.
c() { echo "$*" > "$XDG_RUNTIME_DIR/ztmetro.ctl"; }
c desktop state
sleep 0.3
STATE=$XDG_RUNTIME_DIR/ztmetro-desktop.state
# "output NAME KIND W H SCALE X Y": the desktop's output, its size in points, where it is in the layout
set -- $(sed -n 's/^output \([^ ]*\) [a-z]* \([0-9]*\) \([0-9]*\) \([0-9.]*\) \([0-9-]*\) .*/\1 \2 \3 \4 \5/p' "$STATE")
OUT=${OUT:-${1:-HEADLESS-2}}
W=${W:-$(awk -v w="${2:-1600}" -v s="${4:-1}" 'BEGIN { print int(w / s) }')}
H=${H:-$(awk -v h="${3:-900}" -v s="${4:-1}" 'BEGIN { print int(h / s) }')}
OX=${OX:-${5:-360}}
PANEL=${PANEL:-HEADLESS-1}			# the phone's own output
OUTDIR=${OUTDIR:-/out}				# screenshots
LOG=${LOG:-/out/ztmetro.log}			# the shell's log
tool() { if [ -x "./desktop/$1" ]; then echo "./desktop/$1"; elif [ -x "./pods/$1" ]; then echo "./pods/$1"; else command -v "$1"; fi; }
POKE=${POKE:-$(tool zt-deskpoke)}
DESKLAT=${DESKLAT:-$(tool zt-desklat)}
PODS=${PODS:-$(tool ztpods)}
MOVE=${MOVE:-$(tool ztmove)}
# a window that asks for our frame (server-side decorations), and its app_id
SSD_CMD=${SSD_CMD:-foot -a ssd -T ssd}
SSD_ID=${SSD_ID:-ssd}
# an app opened on the phone, and its app_id
PHONE_CMD=${PHONE_CMD:-foot -a footphone}
PHONE_ID=${PHONE_ID:-footphone}
poke() { "$POKE" "$OUT" "$W" "$H" "$*"; }
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "PASS $*"; }
no() { fail=$((fail + 1)); echo "FAIL $*"; }
check() { if eval "$2"; then ok "$1"; else no "$1 ($3)"; fi; }
# a checksum of a part of the desktop (x y w h, output points at scale 1)
region() {
	grim -o "$OUT" -t ppm /tmp/region.ppm 2>/dev/null
	python3 - "$@" <<'EOF'
import hashlib, sys
x, y, w, h = [int(v) for v in sys.argv[1:5]]
d = open('/tmp/region.ppm', 'rb').read()
parts, i = [], 0
while len(parts) < 4:
    while d[i:i+1].isspace(): i += 1
    j = i
    while not d[j:j+1].isspace(): j += 1
    parts.append(d[i:j]); i = j
i += 1
width = int(parts[1])
rows = b''.join(d[i + ((y + r) * width + x) * 3: i + ((y + r) * width + x + w) * 3] for r in range(h))
print(hashlib.md5(rows).hexdigest())
EOF
}
waitfor() { i=0; while [ $i -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
