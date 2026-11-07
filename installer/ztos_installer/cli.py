# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos: install ztOS.

    ztos install                 the guided installer (asks where, and who you are)
    ztos install --target qemu   ztOS in the QEMU emulator, to try it on this computer
    ztos install --target starlte   ztOS on a Galaxy S9
    ztos emulator                start the emulator with the ztOS installed there
    ztos build --target T        only build the system (for qemu: the disk too)
    ztos build --target gsi      the generic system image alone, for ztConflo (--gsi)
    ztos guide                   how installing on the phone goes, step by step
    ztos doctor                  is this computer ready to build ztOS?
    ztos mirror --target T       the screen in a window, the mouse as a finger (phone or qemu)
    ztos touch --target T ...    fingers and keys from a script, for tests
"""
from __future__ import annotations

import argparse
import getpass
import re
import sys
from pathlib import Path

from . import build as b
from . import host
from . import targets as t

BANNER = r"""
        _    ___  ____
   ____| |_ / _ \/ ___|
  |_  /| __| | | \___ \     the ztOS installer
   / / | |_| |_| |___) |
  /___| \__|\___/|____/
"""


def ask(question: str, default: bool = True) -> bool:
    hint = "[Y/n]" if default else "[y/N]"
    while True:
        answer = input(f"{question} {hint} ").strip().lower()
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False


def prompt(question: str, default: str = "", check=None) -> str:
    while True:
        answer = input(f"{question}" + (f" [{default}]" if default else "") + ": ").strip() or default
        problem = check(answer) if check else None
        if not problem:
            return answer
        print(f"  {problem}")


def valid_user(name: str):
    if not re.fullmatch(r"[a-z_][a-z0-9_-]{0,30}", name):
        return "lower-case letters, digits, - and _; starting with a letter"
    if name in ("root", "bin", "daemon", "nobody"):
        return "that name is the system's"
    return None


def valid_timezone(name: str):
    return None if Path("/usr/share/zoneinfo", name).is_file() else "not a time zone (like Asia/Colombo, or UTC)"


def wizard(args) -> b.Settings:
    print(BANNER)
    print("This installs ztOS: ztMETRO's phone shell on Arch Linux ARM, with its apps and the SDK.\n")
    if args.target == "gsi":
        sys.exit("the GSI is built, not installed: ztos build --target gsi")
    if args.target:
        target = args.target
    else:
        print("Where should ztOS go?")
        print("  1. the emulator (QEMU) on this computer: try it, nothing else is touched")
        print("  2. a Samsung Galaxy S9 (starlte): replaces Android, erases the phone's data")
        target = {"1": "qemu", "2": "starlte"}[prompt("1 or 2", "1", lambda a: None if a in ("1", "2") else "1 or 2")]
    if target == "starlte":
        print(t.PHONE_GUIDE)
        if not ask("Go on?", default=False):
            sys.exit(0)
    s = b.Settings(target=target)
    print("\nYour account on ztOS")
    s.user = args.user or prompt("  user name", "ztos", valid_user)
    if args.password:
        s.password = args.password
    else:
        while True:
            first = getpass.getpass("  password: ")
            if len(first) < 4:
                print("  at least 4 characters")
                continue
            if getpass.getpass("  again: ") == first:
                s.password = first
                break
            print("  those were not the same")
    s.hostname = args.hostname or prompt("  the phone's name", "ztos",
                                         lambda a: None if re.fullmatch(r"[a-zA-Z0-9-]{1,63}", a) else "letters, digits and -")
    s.timezone = args.timezone or prompt("  time zone", "UTC", valid_timezone)
    s.fresh = args.fresh
    print(f"\nztOS for {'the emulator' if target == 'qemu' else 'the Galaxy S9'}, user {s.user}, "
          f"named {s.hostname}, time zone {s.timezone}.")
    print("Building it downloads about 2 GB and takes a while (30-60 minutes: ARM programs run emulated because this computer is x86).")
    if not args.yes and not ask("Start?"):
        sys.exit(0)
    return s


def cmd_install(args):
    mode = getattr(args, "mode", None)
    if not mode and not args.yes:
        print(BANNER)
        print("Welcome to the ztOS Installer!\n")
        print("Choose an option:")
        print("  1. Build image in this PC and install")
        print("  2. Build image in this PC and store")
        print("  3. Reboot the phone from ztOS Rescue (it is stuck in Rescue)")
        choice = prompt("1, 2 or 3", "1", lambda a: None if a in ("1", "2", "3") else "choose 1, 2 or 3")
        if choice == "3":
            t.reboot_from_rescue()
            return
        mode = {"1": "build-install", "2": "build-store"}[choice]
    else:
        mode = mode or "build-install"

    if mode == "download":
        if args.target:
            target = args.target
        else:
            print("\nWhere should ztOS go?")
            print("  1. the emulator (QEMU) on this computer: try it, nothing else is touched")
            print("  2. a Samsung Galaxy S9 (starlte): replaces Android, erases the phone's data")
            target = {"1": "qemu", "2": "starlte"}[prompt("1 or 2", "1", lambda a: None if a in ("1", "2") else "1 or 2")]
        if target == "starlte":
            print(t.PHONE_GUIDE)
            if not ask("Go on?", default=False):
                sys.exit(0)
        from . import download
        try:
            image, boot = download.get_prebuilt(target)
        except b.Fail as e:
            print(f"\nztos: {e}")
            if ask("Would you like to build on this PC instead?"):
                mode = "build-install"
            else:
                sys.exit(1)
        if mode == "download":
            print("\nPrebuilt images use default credentials:")
            print("  Username: zerone")
            print("  Password: toor\n")
            if target == "qemu":
                print("ztOS is ready in the emulator disk:", image)
                if args.yes or ask("Start it now?"):
                    t.run_qemu(headless=args.headless, gl=getattr(args, "gl", False))
                else:
                    print("Start it later with: installer/ztos emulator")
            else:
                t.install_phone(image, ask)
            return

    s = wizard(args)
    image = b.build(s)
    if mode == "build-store":
        print(f"\nztOS system image successfully built and stored at:\n  {image}\n")
        print(f"You can install it anytime using:")
        print(f"  installer/ztos install --target {s.target}")
        return

    if s.target == "qemu":
        print("\nztOS is installed in the emulator's disk:", image)
        if args.yes or ask("Start it now?"):
            t.run_qemu(headless=args.headless, gl=getattr(args, "gl", False))
        else:
            print("Start it later with: installer/ztos emulator")
    else:
        t.install_phone(image, ask)


def cmd_build(args):
    s = b.Settings(target=args.target, user=args.user or "ztos", password=args.password or "ztos",
                   hostname=args.hostname or "ztos", timezone=args.timezone or "UTC", fresh=args.fresh)
    b.build(s)


def cmd_emulator(args):
    t.run_qemu(headless=args.headless, gl=args.gl)


def cmd_screenshots(args):
    from . import screenshots
    screenshots.take(Path(args.out), keep=args.keep)


def cmd_guide(args):
    print(t.PHONE_GUIDE)


def cmd_doctor(args):
    import shutil

    def show(checks):
        for c in checks:
            state = "yes" if c.ok else ("warning" if c.warning else "NO")
            print(f"  {c.what}: {state}" + (f" ({c.detail})" if c.detail else ""))

    build = host.build_checks()
    print("building ztOS")
    show(build)
    print("installing on a phone (the emulator does not need these)")
    show(host.phone_checks())
    print("the emulator")
    print(f"  qemu-system-aarch64: {'yes' if shutil.which('qemu-system-aarch64') else 'no'}")
    gpu = Path("/usr/lib/qemu/hw-display-virtio-gpu-pci-gl.so").exists()
    window = Path("/usr/lib/qemu/ui-gtk.so").exists() and Path("/usr/lib/qemu/ui-opengl.so").exists()
    print(f"  its graphics (virtio GPU with OpenGL): {'yes' if gpu else 'no: sudo pacman -S qemu-hw-display-virtio-gpu-pci-gl'}")
    print(f"  its window (GTK + OpenGL): {'yes' if window else 'no: sudo pacman -S qemu-ui-gtk qemu-ui-opengl'}")
    failed = host.problems(build)
    if failed:
        raise b.Fail(f"this computer cannot build ztOS yet: {len(failed)} thing(s) marked NO above")
    print("this computer can build ztOS")


def cmd_mirror(args):
    from . import mirror
    mirror.cmd_mirror(args)


def cmd_touch(args):
    from . import mirror
    mirror.cmd_touch(args)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="ztos", description="the ztOS installer",
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = parser.add_subparsers(dest="command")

    def with_settings(p):
        p.add_argument("--target", choices=["qemu", "starlte", "gsi"])
        p.add_argument("--user")
        p.add_argument("--password", help="(else asked; never stored)")
        p.add_argument("--hostname")
        p.add_argument("--timezone")
        p.add_argument("--fresh", action="store_true", help="start again from Arch Linux ARM (not the last build)")

    p = sub.add_parser("install", help="the guided installer")
    with_settings(p)
    p.add_argument("--mode", choices=["download", "build-install", "build-store"], help="installer mode")
    p.add_argument("--yes", action="store_true", help="no questions where there are answers")
    p.add_argument("--headless", action="store_true", help="the emulator without a window (vnc 127.0.0.1:5905)")
    p.set_defaults(func=cmd_install)
    p = sub.add_parser("build", help="build the system (and the emulator's disk)")
    with_settings(p)
    p.set_defaults(func=cmd_build, target="qemu")
    p = sub.add_parser("emulator", help="start ztOS in the emulator")
    p.add_argument("--headless", action="store_true")
    p.add_argument("--gl", action="store_true", help="draw with the computer's GPU (virgl): faster, but some "
                   "drivers crash QEMU with EGL_BAD_ACCESS")
    p.set_defaults(func=cmd_emulator)
    p = sub.add_parser("screenshots", help="boot the emulator and take screenshots of ztOS")
    p.add_argument("--out", default=str(b.ROOT / "docs" / "screenshots"))
    p.add_argument("--keep", action="store_true", help="leave the emulator running afterwards")
    p.set_defaults(func=cmd_screenshots)
    sub.add_parser("guide", help="installing on the phone, step by step").set_defaults(func=cmd_guide)
    sub.add_parser("doctor", help="is this computer ready?").set_defaults(func=cmd_doctor)

    def with_device(p):
        p.add_argument("--target", choices=["phone", "qemu"], default="phone")
        p.add_argument("--host", help="its address, if not the usual one")
        p.add_argument("--port", type=int, default=27183)
        p.add_argument("--token", help="ztmirror's token: for one you started yourself, without ssh")
        p.add_argument("--ssh", help="ssh arguments to reach it (default: the target's, or $ZT_SSH)")
        p.add_argument("--keep", action="store_true", help="leave ztmirror running afterwards")

    p = sub.add_parser("mirror", help="the device's screen in a window, the mouse as a finger")
    with_device(p)
    p.add_argument("--scale", type=int, choices=[1, 2, 4], default=2, help="1/N of the screen's pixels (default 2)")
    p.add_argument("--quality", type=int, default=70, help="JPEG quality, 10 to 100 (default 70)")
    p.add_argument("--fps", type=int, default=30, help="at most this many frames a second (default 30)")
    p.add_argument("--snapshot", metavar="FILE", help="no window: save one frame (.jpg, or .png with PySide6)")
    p.add_argument("--stats", type=float, metavar="SECONDS", help="no window: count frames and bytes for a while")
    p.set_defaults(func=cmd_mirror)
    p = sub.add_parser("touch", help="fingers and keys from a script, in points",
                       formatter_class=argparse.RawDescriptionHelpFormatter, description="""\
  tap X Y                  one tap
  swipe X0 Y0 X1 Y1 [MS]   drag one finger
  pinch CX CY R0 R1 [MS]   move two fingers apart (or together)
  key NAME [down|up]       power, volup, voldown, home, back (a press if neither)
  play FILE|-              a timeline, as for scripts/ztctl.py: one "MS LINE" per line, with
                           LINE down ID X Y, move ID X Y, up ID, cancel ID, tap X Y, key NAME down|up
""")
    with_device(p)
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("what", choices=["tap", "swipe", "pinch", "key", "play"])
    p.add_argument("args", nargs="*")
    p.set_defaults(func=cmd_touch)

    args = parser.parse_args(argv)
    if not args.command:
        args = parser.parse_args(["install", *(argv or sys.argv[1:])])
    try:
        args.func(args)
        return 0
    except b.Fail as e:
        print(f"ztos: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nztos: stopped")
        return 130
