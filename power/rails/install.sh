#!/bin/sh
# Put the ROADMAP 8.31 fixes on the phone, as root, from a directory holding
# what to install (copied there by the caller):
#
#   sudo sh install.sh rails      zrn-rails.ko -> extra/, loaded at boot
#                                 (modules-load.d); not loaded now
#   sudo sh install.sh abox       snd-zrn-abox.ko -> extra/ (the running one
#                                 stays until the module is reloaded)
#   sudo sh install.sh perfd      zrnperfd -> /usr/local/sbin (restart the
#                                 service yourself, inside ztlock)
#   sudo sh install.sh restore    every backup below put back, the boot
#                                 entry removed
#
# Backups: NAME.pre-rail-map next to each file replaced, made once.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
REL=$(uname -r)
M=/usr/lib/modules/$REL/extra
LOAD=/etc/modules-load.d/zrn-rails.conf
[ "$(id -u)" = 0 ] || { echo "as root" >&2; exit 1; }
backup() { [ -e "$1" ] && [ ! -e "$1.pre-rail-map" ] && cp -a "$1" "$1.pre-rail-map"; return 0; }
check() {
	v=$(modinfo -F vermagic "$1")
	case "$v" in "$REL "*) ;; *) echo "$1 is for '$v', not $REL" >&2; exit 1 ;; esac
}
case "${1:-}" in
rails)
	check "$D/zrn-rails.ko"
	backup "$M/zrn-rails.ko"
	install -m644 "$D/zrn-rails.ko" "$M/zrn-rails.ko"
	printf '%s\n' '# ROADMAP 8.31 (power/rails/zrn-rails.c): MIF DVFS for zrnperfd, touch AVDD 3.0 V' zrn-rails > "$LOAD"
	depmod "$REL"
	echo "zrn-rails installed; loads at boot ($LOAD)"
	;;
abox)
	check "$D/snd-zrn-abox.ko"
	backup "$M/snd-zrn-abox.ko"
	install -m644 "$D/snd-zrn-abox.ko" "$M/snd-zrn-abox.ko"
	depmod "$REL"
	echo "snd-zrn-abox installed; the running module is unchanged until reloaded"
	;;
perfd)
	backup /usr/local/sbin/zrnperfd
	install -m755 "$D/zrnperfd" /usr/local/sbin/zrnperfd.new
	mv /usr/local/sbin/zrnperfd.new /usr/local/sbin/zrnperfd
	echo "zrnperfd installed; restart zrnperfd.service to use it"
	;;
restore)
	for f in "$M/zrn-rails.ko" "$M/snd-zrn-abox.ko" /usr/local/sbin/zrnperfd; do
		[ -e "$f.pre-rail-map" ] && cp -a "$f.pre-rail-map" "$f" && echo "restored $f"
	done
	[ -e "$M/zrn-rails.ko.pre-rail-map" ] || rm -f "$M/zrn-rails.ko"
	rm -f "$LOAD"
	depmod "$REL"
	echo "restored; reboot (or reload snd-zrn-abox, restart zrnperfd, rmmod zrn-rails)"
	;;
*)
	sed -n '2,15p' "$0"; exit 2 ;;
esac
