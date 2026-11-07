#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect the phone's logs into one file you can share, without its identity.

    scripts/collect-logs.py [--out DIR] [--no-previous-boot]
    some-command | scripts/collect-logs.py --filter

Writes DIR/ztos-logs-<time>.tar.gz (default DIR: out/). It reaches the phone through
scripts/phone-do.sh, so ZT_USER, ZT_PW and ZT_SSH work as described in
scripts/phone-lib.sh. Everything it runs on the phone only reads.

What is kept out:
  - EFS is never read: no command here goes near it. A log line naming EFS, or the
    modem's identity files, is dropped whole.
  - A line about an IMEI, IMSI, ICCID, serial number or phone number is dropped whole.
  - Anywhere else, long numbers, phone numbers, MAC and Bluetooth addresses, Wi-Fi
    names, passwords, tokens, email addresses and the bootloader's androidboot.*
    values are replaced by a [marker].

Nothing is written to this computer before that is done. It is a filter, so read
the result before posting it: local IP addresses, app names and the user name stay in.
"""
import argparse
import io
import os
import re
import subprocess
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PHONE_DO = ROOT / "scripts" / "phone-do.sh"
TIMEOUT = 120

# (file, command, needs root). A command that fails as root is tried again as the user.
JOURNAL = "journalctl --no-pager -o short-iso -n 50000"
COMMANDS = [
    ("version.txt", "cat /etc/ztos/built /etc/os-release; uname -a", False),
    ("failed-units.txt", "systemctl --failed --no-pager; systemctl list-units --no-pager --type=service --state=running", False),
    ("journal.txt", f"{JOURNAL} -b", True),
    ("dmesg.txt", "dmesg", True),
    ("modules.txt", "lsmod", False),
    ("storage.txt", "df -h; lsblk -o NAME,SIZE,TYPE,MOUNTPOINT", False),
    ("network.txt", "ip -br addr; ip route; networkctl --no-pager; nmcli -t -f DEVICE,TYPE,STATE device", False),
    ("memory.txt", "free -m; uptime", False),
    ("crashes.txt", "coredumpctl list --no-pager; ls -la /var/lib/systemd/pstore; "
                    "find /var/lib/systemd/pstore -type f -name 'dmesg*' | sort | tail -n 4 | xargs -r cat", True),
    ("hardware-support.txt", "ztoscsf status", False),
]
PREVIOUS_BOOT = ("journal-previous-boot.txt", f"{JOURNAL} -b -1", True)

# The reason a command may not run. Checked before any command is sent.
FORBIDDEN_COMMAND = re.compile(r"efs|/dev/block|/dev/sd|by-name|by-partlabel|\bdd\b|getprop|AT\+|\brm\b", re.I)

DROP_LINE = re.compile(
    r"/efs\b|\befs/|\bmnt/vendor/efs|\bcpefs\b|\bnv_data\b|\bnv\.bin\b|by-(?:name|partlabel)/(?:CP)?EFS"
    r"|\b(?:imei\w*|imsi|iccid|msisdn|meid|cgsn|cimi|ccid|cnum|serialno|serial[_ -]?number|ap_serial)\b"
    r"|\bserial\s*[:=]|\bsim[_ -]?id\b|\bphone[_ -]?number\b",
    re.I)
REPLACE = [
    (re.compile(r"\bandroidboot\.\S+"), "androidboot.[removed]"),
    # NetworkManager, as seen on the phone: Config: added 'ssid' value 'Name'
    (re.compile(r"'(ssid|essid)' value '[^']*'", re.I), r"'\1' value '[wifi name]'"),
    (re.compile(r"\b(access point|wireless network|policy: set)\s+(['\"]).*?\2", re.I), r"\1 '[wifi name]'"),
    (re.compile(r"\b(ssid|essid)(\s*[=:]?\s*)(['\"]).*?\3", re.I), r"\1\2[wifi name]"),
    (re.compile(r"\b(ssid|essid)\s*[=:]\s*\S+", re.I), r"\1=[wifi name]"),
    (re.compile(r"\b(connection|profile|network)\s+'[^']*'", re.I), r"\1 '[name]'"),
    (re.compile(r"\b(psk|password|passwd|token|secret|bearer|api[_-]?key|auth[_-]?key|cookie)(\s*[=:]\s*|\s+)\S+", re.I),
     r"\1=[removed]"),
    (re.compile(r"(?<![0-9a-f])(?:[0-9a-f]{2}[:_-]){5}[0-9a-f]{2}(?![0-9a-f])", re.I), "[address]"),
    (re.compile(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+"), "[email]"),
    (re.compile(r"\+\d[\d -]{6,16}\d"), "[phone]"),
    (re.compile(r"(?<![\w.:])\d(?:[ -]?\d){9,}(?![\w.:])"), "[number]"),
    # the last word on long numbers, whatever is around them. Only an 0x address
    # survives: the kernel log is full of them, and they are no one's identity
    (re.compile(r"\b0x[0-9a-f]+\b|\d{14,}", re.I), lambda m: m.group() if m.group()[:2].lower() == "0x" else "[number]"),
]
DROPPED = "[line removed: identity or EFS]"


def saved_network_names() -> list:
    """The phone's saved connections, so a name is caught however a log words it.
    Only used here, in memory, as what to take out."""
    out = run("nmcli -t -f NAME connection show", False)
    return [n for n in (line.strip() for line in clean_prompts(out).splitlines()) if len(n) >= 3]


def redact_line(line: str, names: tuple = ()) -> str:
    if DROP_LINE.search(line):
        return DROPPED
    for name in names:
        line = line.replace(name, "[wifi name]")
    for pattern, replacement in REPLACE:
        line = pattern.sub(replacement, line)
    return line


def redact(text: str, names: tuple = ()) -> str:
    return "\n".join(redact_line(line, names) for line in text.splitlines()) + "\n"


def run(command: str, root: bool) -> str:
    if FORBIDDEN_COMMAND.search(command):
        raise SystemExit(f"collect-logs: refusing a command that may read identity data: {command}")
    argv = [str(PHONE_DO), *(["--root"] if root else []), command]
    try:
        done = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return f"(no answer in {TIMEOUT} s)\n"
    out = (done.stdout + done.stderr).decode("utf-8", "replace")
    if root and done.returncode != 0:
        return run(command, False)
    return out if done.returncode == 0 else out + f"\n(exit status {done.returncode})\n"


def clean_prompts(text: str) -> str:
    return re.sub(r"^\s*(?:\S+'s )?[Pp]assword:\s*", "", text, flags=re.M)


def main() -> int:
    parser = argparse.ArgumentParser(description="the phone's logs, with its identity left out")
    parser.add_argument("--out", default=str(ROOT / "out"), help="where the file goes (default: out/)")
    parser.add_argument("--no-previous-boot", action="store_true", help="only this boot's journal")
    parser.add_argument("--filter", action="store_true", help="no phone: filter stdin to stdout, for a log you already have")
    args = parser.parse_args()
    if args.filter:
        sys.stdout.write(redact(sys.stdin.read()))
        return 0
    if not os.environ.get("ZT_SSH") and not os.environ.get("ZT_PW"):
        if not sys.stdin.isatty():
            sys.exit("collect-logs: set ZT_PW (and ZT_USER), or ZT_SSH: see scripts/phone-lib.sh")
        import getpass
        user = os.environ.get("ZT_USER") or getpass.getuser()
        os.environ["ZT_PW"] = getpass.getpass(f"password for {user} on the phone: ")

    commands = COMMANDS + ([] if args.no_previous_boot else [PREVIOUS_BOOT])
    stamp = time.strftime("%Y%m%d-%H%M%SZ", time.gmtime())
    Path(args.out).mkdir(parents=True, exist_ok=True)
    target = Path(args.out) / f"ztos-logs-{stamp}.tar.gz"
    tmp = target.with_suffix(".part")
    removed = 0
    names = tuple(saved_network_names())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as raw, tarfile.open(fileobj=raw, mode="w:gz") as tar:
        for name, command, root in commands:
            print(f"collect-logs: {name}", flush=True)
            text = redact(clean_prompts(run(command, root)), names)
            removed += text.count(DROPPED)
            data = text.encode()
            info = tarfile.TarInfo(f"ztos-logs-{stamp}/{name}")
            info.size, info.mtime, info.mode = len(data), int(time.time()), 0o600
            tar.addfile(info, io.BytesIO(data))
    tmp.replace(target)
    print(f"collect-logs: {target} ({target.stat().st_size // 1024} KB), {removed} line(s) removed. "
          "Read it before you share it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
