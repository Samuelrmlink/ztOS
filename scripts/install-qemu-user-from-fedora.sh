#!/bin/bash
# Install qemu-aarch64-static on an EL10 host from Fedora's signed RPM, without
# adding a Fedora repository and without registering anything in the RPM database.
# EL10 repositories and EPEL do not carry qemu-user-static. See docs/host-setup-rhel.md.
#
#   sudo scripts/install-qemu-user-from-fedora.sh               install, or update
#   sudo scripts/install-qemu-user-from-fedora.sh --uninstall   remove the two files
#
# Options for an RPM you already have (it is verified the same way):
#   --rpm FILE    use this RPM instead of downloading the newest one
#   --key FILE    use this key file instead of downloading it
#
# It installs two files and nothing else:
#   /usr/local/bin/qemu-aarch64-static
#   /etc/binfmt.d/qemu-aarch64-static.conf
#
# Nothing on the system changes until the key fingerprint, the RPM signature and
# the RPM's contents have all checked out. dnf does not know about these files, so
# updates are manual: run this again.
#
# To move to a later Fedora release, change FEDORA_RELEASE and KEY_FINGERPRINT.
set -euo pipefail
export LC_ALL=C

FEDORA_RELEASE=45
KEY_FINGERPRINT=4F50A6114CD5C6976A7F1179655A4B02F577861E
KEY_URL=https://src.fedoraproject.org/rpms/fedora-repos/raw/f$FEDORA_RELEASE/f/RPM-GPG-KEY-fedora-$FEDORA_RELEASE-primary
PKG_URL=https://dl.fedoraproject.org/pub/fedora/linux/updates/$FEDORA_RELEASE/Everything/x86_64/Packages/q/
PKG_NAME=qemu-user-static-aarch64

ENTRY_NAME=qemu-aarch64
ENTRY=/proc/sys/fs/binfmt_misc/$ENTRY_NAME
BIN=/usr/local/bin/qemu-aarch64-static
CONF=/etc/binfmt.d/qemu-aarch64-static.conf
RPM_BIN=usr/bin/qemu-aarch64-static
RPM_CONF=usr/lib/binfmt.d/qemu-aarch64-static.conf
BINFMT_DIRS="/etc/binfmt.d /run/binfmt.d /usr/local/lib/binfmt.d /usr/lib/binfmt.d"

WORK=
STAGED=

say() { printf '%s\n' "$*"; }
die() {
	printf 'FAILED: %s\n' "$*" >&2
	exit 1
}
cleanup() {
	[ -z "$STAGED" ] || rm -f -- "$STAGED"
	[ -z "$WORK" ] || rm -rf -- "$WORK"
}
trap cleanup EXIT

fetch() { curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location --retry 3 "$@"; }

# True when the kernel entry is on, runs our binary, and has the F flag.
entry_is_ours() {
	[ -r "$ENTRY" ] || return 1
	grep -qx enabled "$ENTRY" &&
		grep -qx "interpreter $BIN" "$ENTRY" &&
		grep -Eq '^flags: .*F' "$ENTRY"
}

# Other kernel entries, which no config file defines. Restarting systemd-binfmt
# flushes every entry and loads only what has a config file, so these go away.
entries_without_config() {
	local f name
	for f in /proc/sys/fs/binfmt_misc/*; do
		name=${f##*/}
		case $name in register | status | "$ENTRY_NAME") continue ;; esac
		# shellcheck disable=SC2086
		grep -qsF ":$name:" $(printf '%s/*.conf ' $BINFMT_DIRS) || printf '%s ' "$name"
	done
}

relabel() {
	selinuxenabled 2>/dev/null || return 0
	restorecon -F "$1"
}

restart_binfmt() {
	local lost
	lost=$(entries_without_config)
	if [ -n "$lost" ]; then
		say "These binfmt entries have no config file, and restarting systemd-binfmt removes them:"
		say "  $lost"
	fi
	if ! systemctl restart systemd-binfmt; then
		say "WARNING: systemd-binfmt reported a failure. Its log:"
		journalctl -b -u systemd-binfmt --no-pager -n 10 || true
		return 1
	fi
}

uninstall() {
	local removed=
	for f in "$BIN" "$CONF"; do
		if [ -e "$f" ]; then
			rm -f -- "$f"
			say "Removed $f"
			removed=1
		else
			say "Not present: $f"
		fi
	done
	[ -n "$removed" ] || [ -e "$ENTRY" ] || {
		say "Nothing to do."
		return 0
	}
	restart_binfmt || say "Continuing, to check the entry."
	# The restart leaves an inactive service stopped without flushing anything.
	if [ -r "$ENTRY" ] && grep -qx "interpreter $BIN" "$ENTRY"; then
		printf -- '-1' >"$ENTRY"
	fi
	if [ -e "$ENTRY" ]; then
		say "The $ENTRY_NAME entry is still there, and it is not this script's:"
		sed 's/^/  /' "$ENTRY"
	else
		say "The $ENTRY_NAME entry is gone."
	fi
	say "Done."
}

verify_key() {
	local key=$1 gh=$WORK/gnupg listing pubs fpr
	mkdir -m 700 "$gh"
	listing=$(gpg --homedir "$gh" --batch --no-autostart --with-colons --show-keys "$key" 2>/dev/null) ||
		die "gpg could not read the key file"
	pubs=$(grep -c '^pub:' <<<"$listing" || true)
	[ "$pubs" = 1 ] || die "the key file holds $pubs primary keys, expected 1"
	fpr=$(awk -F: '$1 == "fpr" { print $10; exit }' <<<"$listing")
	[ "$fpr" = "$KEY_FINGERPRINT" ] ||
		die "key fingerprint is '$fpr', expected $KEY_FINGERPRINT"
	say "Key fingerprint OK: $fpr"
}

# Uses a throwaway RPM database that holds only the Fedora key. The system's
# database and its keys are not read or changed.
verify_rpm() {
	local rpm=$1 key=$2 db=$WORK/rpmdb out rc=0 short bad got
	mkdir "$db"
	rpmkeys --dbpath "$db" --import "$key" || die "rpmkeys could not import the key"
	out=$(rpmkeys --dbpath "$db" --checksig --verbose "$rpm" 2>&1) || rc=$?
	sed 's/^/  /' <<<"$out"
	[ "$rc" = 0 ] || die "signature check failed (rpmkeys exit $rc): $rpm"
	# rpmkeys also exits 0 for an RPM with no signature at all, so read the
	# lines too: every check must say OK, and the signature checks must be there.
	short=$(printf '%s' "${KEY_FINGERPRINT: -8}" | tr 'A-F' 'a-f')
	bad=$(sed 1d <<<"$out" | grep -v ': OK$' || true)
	[ -z "$bad" ] || die "a check did not pass: $bad"
	grep -Eq "^ *Header .*Signature, key ID $short: OK$" <<<"$out" ||
		die "no header signature from key $short: $rpm"
	grep -Eq "^ *Payload .*digest: OK$" <<<"$out" ||
		die "no payload digest: $rpm"
	got=$(rpm --dbpath "$db" -qp --qf '%{NAME} %{ARCH} %{RELEASE}' "$rpm")
	case $got in
	"$PKG_NAME x86_64 "*".fc$FEDORA_RELEASE") ;;
	*) die "signed, but not the expected package: $got" ;;
	esac
	say "RPM signature OK: $(rpm --dbpath "$db" -qp --qf '%{NVRA}' "$rpm")"
}

extract() {
	local rpm=$1 ex=$WORK/x
	mkdir "$ex"
	rpm2cpio "$rpm" | (cd "$ex" && cpio -idm --quiet --no-absolute-filenames "./$RPM_BIN" "./$RPM_CONF") ||
		die "could not extract the RPM"
	[ -f "$ex/$RPM_BIN" ] && [ ! -L "$ex/$RPM_BIN" ] || die "the RPM has no $RPM_BIN"
	[ -f "$ex/$RPM_CONF" ] && [ ! -L "$ex/$RPM_CONF" ] || die "the RPM has no $RPM_CONF"
}

# Writes $WORK/conf: the RPM's own rule, with only the interpreter path changed.
make_conf() {
	local src=$WORK/x/$RPM_CONF from=":/$RPM_BIN:" to=":$BIN:" rule flags
	rule=$(grep -Ev '^[[:space:]]*([#;]|$)' "$src")
	[ "$(wc -l <<<"$rule")" = 1 ] || die "expected one rule in the RPM's conf"
	case $rule in
	":$ENTRY_NAME:"*"$from"*) ;;
	*) die "unexpected rule in the RPM's conf: $rule" ;;
	esac
	flags=${rule##*:}
	case $flags in
	*F*) ;;
	*) die "the RPM's rule has no F flag: $rule" ;;
	esac
	{
		say "# Installed by scripts/install-qemu-user-from-fedora.sh from $RPM_NAME"
		say "${rule/"$from"/"$to"}"
	} >"$WORK/conf"
	grep -qF ":$BIN:$flags" "$WORK/conf" || die "could not rewrite the interpreter path"
}

# Puts the binary next to its destination, under a temporary name, and checks it.
stage_binary() {
	local desc ldd_out
	STAGED=$(mktemp "$BIN.new.XXXXXX")
	install -o root -g root -m 755 "$WORK/x/$RPM_BIN" "$STAGED"
	desc=$(file -b "$STAGED")
	case $desc in
	*"ELF 64-bit"*"x86-64"*static*) ;;
	*) die "not a static x86-64 program: $desc" ;;
	esac
	ldd_out=$(ldd "$STAGED" 2>&1 || true)
	case $ldd_out in
	*"=>"*) die "ldd lists shared libraries: $ldd_out" ;;
	esac
	"$STAGED" --version >/dev/null || die "the binary does not run"
	say "Binary is static: ${desc%%, BuildID*}"
	say "  ldd: $(sed 's/^[[:space:]]*//' <<<"$ldd_out")"
}

install_all() {
	local rpm key sha changed= f other
	key=$WORK/key
	if [ -n "$OPT_KEY" ]; then
		cp -- "$OPT_KEY" "$key"
	else
		say "Downloading $KEY_URL"
		fetch -o "$key" "$KEY_URL" || die "could not download the key"
	fi
	verify_key "$key"

	if [ -n "$OPT_RPM" ]; then
		rpm=$OPT_RPM
	else
		local name
		name=$(fetch "$PKG_URL" |
			grep -oE "$PKG_NAME-[0-9][0-9A-Za-z.+~^_-]*\.fc$FEDORA_RELEASE\.x86_64\.rpm" |
			sort -Vu | tail -n 1) || true
		[ -n "$name" ] || die "no $PKG_NAME found at $PKG_URL"
		rpm=$WORK/$name
		say "Downloading $PKG_URL$name"
		fetch -o "$rpm" "$PKG_URL$name" || die "could not download the RPM"
	fi
	sha=$(sha256sum "$rpm" | cut -d' ' -f1)
	say "RPM sha256: $sha"

	verify_rpm "$rpm" "$key"
	RPM_NAME=$(rpm --dbpath "$WORK/rpmdb" -qp --qf '%{NVRA}' "$rpm")
	extract "$rpm"
	make_conf
	stage_binary

	# Everything checked out. Changes to the system start here.
	if cmp -s "$STAGED" "$BIN"; then
		say "Unchanged: $BIN"
		rm -f -- "$STAGED"
	else
		mv -f -- "$STAGED" "$BIN"
		say "Installed: $BIN"
		changed=1
	fi
	STAGED=
	relabel "$BIN"

	if cmp -s "$WORK/conf" "$CONF"; then
		say "Unchanged: $CONF"
	else
		install -o root -g root -m 644 "$WORK/conf" "$CONF"
		say "Installed: $CONF"
		changed=1
	fi
	relabel "$CONF"

	if [ -z "$changed" ] && entry_is_ours; then
		say "Unchanged: the $ENTRY_NAME entry was already right."
	else
		if [ -r "$ENTRY" ]; then
			say "Replacing the existing entry, whose interpreter was: $(sed -n 's/^interpreter //p' "$ENTRY")"
		fi
		restart_binfmt || say "Continuing, to check the entry."
		entry_is_ours || die "$ENTRY is not right after the restart. Files are in place; --uninstall removes them."
		say "Registered with systemd-binfmt."
	fi

	say ""
	say "$ENTRY:"
	sed 's/^/  /' "$ENTRY"
	ls -lZ "$BIN" "$CONF" | sed 's/^/  /'
	say "Installed $RPM_NAME. No repository was added, and the RPM database was not changed."
	if rpm -q "$PKG_NAME" >/dev/null 2>&1; then
		say "NOTE: the $PKG_NAME package is also installed. $CONF takes priority over its conf."
	fi
	say "Updates are manual: run this script again."
	say "Done."
}

OPT_RPM= OPT_KEY= MODE=install RPM_NAME=
while [ $# -gt 0 ]; do
	case $1 in
	--uninstall) MODE=uninstall ;;
	--rpm)
		OPT_RPM=${2:?--rpm needs a file}
		shift
		;;
	--key)
		OPT_KEY=${2:?--key needs a file}
		shift
		;;
	-h | --help)
		sed -n '2,/^set -e/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'
		exit 0
		;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

[ "$(id -u)" = 0 ] || die "this needs root: sudo $0"
[ "$(uname -m)" = x86_64 ] || die "this is for x86_64 hosts"
# Reading this mounts binfmt_misc if it is not mounted yet.
[ -r /proc/sys/fs/binfmt_misc/status ] || die "binfmt_misc is not available"

if [ "$MODE" = uninstall ]; then
	uninstall
	exit 0
fi

missing=
for t in curl gpg rpm rpmkeys rpm2cpio cpio file ldd sha256sum systemctl; do
	command -v "$t" >/dev/null || missing="$missing $t"
done
[ -z "$missing" ] || die "missing tools:$missing. These come from: dnf install curl gnupg2 rpm cpio file glibc-common"

# Another config file for this entry would fight with ours at every boot.
for f in $(grep -lsF ":$ENTRY_NAME:" $(printf '%s/*.conf ' $BINFMT_DIRS) || true); do
	case $f in
	"$CONF" | */qemu-aarch64-static.conf) ;; # same name: ours in /etc overrides it
	*) die "$f also defines the $ENTRY_NAME entry. Remove it, then run this again." ;;
	esac
done

WORK=$(mktemp -d /var/tmp/qemu-from-fedora.XXXXXX)
install_all
