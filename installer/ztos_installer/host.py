# SPDX-License-Identifier: GPL-2.0-or-later
"""Is this computer ready? What `ztos doctor` reports and what a build insists on.

Each check runs the real thing where it can: the qemu binfmt entry by executing
a small aarch64 program, the namespace by entering one. A file being present
proves little: a binfmt entry can name an interpreter that is gone, or lack the
flag the chroot needs.
"""
from __future__ import annotations

import os
import platform
import pwd
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "sdk"
BINFMT = Path("/proc/sys/fs/binfmt_misc/qemu-aarch64")
MIN_PYTHON = (3, 11)        # the SDK imports tomllib
MIN_MKE2FS = (1, 47)        # build.py's -O ^orphan_file
GUIDE_RHEL = "docs/host-setup-rhel.md"

# what provides a thing: Arch, the Red Hat family, Debian and Ubuntu
PACKAGES = {
    "unshare": ("util-linux", "util-linux", "util-linux"),
    "sfdisk": ("util-linux", "util-linux", "fdisk"),
    "newuidmap": ("shadow", "shadow-utils", "uidmap"),
    "bsdtar": ("libarchive", "bsdtar", "libarchive-tools"),
    "mkfs.fat": ("dosfstools", "dosfstools", "dosfstools"),
    "mcopy": ("mtools", "mtools", "mtools"),
    "mke2fs": ("e2fsprogs", "e2fsprogs", "e2fsprogs"),
    "curl": ("curl", "curl", "curl"),
    "tar": ("tar", "tar", "tar"),
    "xz": ("xz", "xz", "xz-utils"),
    "gzip": ("gzip", "gzip", "gzip"),
    "ping": ("iputils", "iputils", "iputils-ping"),
    "python3": ("python", "python3", "python3"),
    "uic": ("qt6-base", "qt6-qtbase-devel", "qt6-base-dev-tools"),
    "qemu-user": ("qemu-user-static qemu-user-static-binfmt", "qemu-user-static-aarch64", "qemu-user-static"),
    "socat": ("socat", "socat", "socat"),
    "adb": ("android-tools", "android-tools", "adb"),
    "zstd": ("zstd", "zstd", "zstd"),
    "qemu-system": ("qemu-system-aarch64", "qemu-system-aarch64-core", "qemu-system-arm"),
    "qemu-firmware": ("edk2-aarch64", "edk2-aarch64", "qemu-efi-aarch64"),
    "qemu-gl": ("qemu-hw-display-virtio-gpu-pci-gl",
                "qemu-device-display-virtio-gpu-pci-gl qemu-device-display-virtio-gpu-gl", "qemu-system-gui"),
    "qemu-window": ("qemu-ui-gtk qemu-ui-opengl", "qemu-ui-gtk qemu-ui-opengl", "qemu-system-gui"),
}
BUILD_TOOLS = ("unshare", "newuidmap", "newgidmap", "bsdtar", "mkfs.fat", "mcopy", "mmd", "sfdisk",
               "curl", "tar", "xz")
PHONE_TOOLS = ("adb", "socat", "gzip", "ping")
PROVIDED_BY = {"newgidmap": "newuidmap", "mmd": "mcopy"}


def family() -> str | None:
    """"arch", "redhat", "debian", or None for anything else."""
    try:
        text = Path("/etc/os-release").read_text()
    except OSError:
        return None
    ids = " ".join(m.group(1).strip("\"'") for m in re.finditer(r"^(?:ID|ID_LIKE)=(.*)$", text, re.M)).split()
    for name, words in (("arch", ("arch", "archarm")),
                        ("redhat", ("fedora", "rhel", "centos")),
                        ("debian", ("debian", "ubuntu"))):
        if any(w in ids for w in words):
            return name
    return None


def install_hint(*needs: str) -> str:
    """How to get these on this computer, in its own package manager's words."""
    fam = family()
    column = {"arch": 0, "redhat": 1, "debian": 2}.get(fam)
    if column is None:
        return "install: " + " ".join(needs)
    names = dict.fromkeys(PACKAGES[PROVIDED_BY.get(n, n)][column] for n in needs)
    command = {"arch": "sudo pacman -S --needed", "redhat": "sudo dnf install",
               "debian": "sudo apt-get install"}[fam]
    hint = f"{command} {' '.join(names)}"
    return f"{hint}; see {GUIDE_RHEL}" if fam == "redhat" else hint


@dataclass
class Check:
    what: str
    ok: bool
    detail: str = ""        # the version found, or what to do about it
    warning: bool = False   # worth saying, but a build can go on


def sbin_which(tool: str) -> str | None:
    """shutil.which, also looking where a user's PATH may leave out (mke2fs, sfdisk...)."""
    return shutil.which(tool, path=os.pathsep.join((os.environ.get("PATH", ""), "/usr/sbin", "/sbin")))


def check_python() -> Check:
    need = ".".join(map(str, MIN_PYTHON))
    version = ".".join(map(str, sys.version_info[:3]))
    if sys.version_info[:2] >= MIN_PYTHON:
        return Check(f"Python {need} or newer", True, version)
    return Check(f"Python {need} or newer", False, f"this is {version}: {install_hint('python3')}")


def check_tool(tool: str) -> Check:
    found = sbin_which(tool)
    return Check(tool, bool(found), "" if found else install_hint(tool))


def check_mke2fs() -> Check:
    what = "mke2fs {} or newer".format(".".join(map(str, MIN_MKE2FS)))
    tool = sbin_which("mke2fs")
    if not tool:
        return Check(what, False, install_hint("mke2fs"))
    out = subprocess.run([tool, "-V"], capture_output=True, text=True)
    m = re.search(r"mke2fs (\d+)\.(\d+)", out.stdout + out.stderr)
    if not m:
        return Check(what, False, "it does not say its version")
    found = (int(m.group(1)), int(m.group(2)))
    version = f"{found[0]}.{found[1]}"
    if found < MIN_MKE2FS:
        return Check(what, False, f"this is {version}: it cannot make the image's filesystem")
    return Check(what, True, version)


def check_uic() -> Check:
    """The compiler for an app's .ui pages (apps/music/form.ui), and that it runs."""
    what = "uic (compiles the apps' pages)"
    if sys.version_info[:2] < MIN_PYTHON:
        return Check(what, False, "not checked: the SDK needs a newer Python first")
    sys.path.insert(0, str(SDK))
    from ztos_sdk.cli import find_uic
    command = find_uic()
    if command is None:
        return Check(what, False, install_hint("uic"))
    try:
        run = subprocess.run([command[0], "--version"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return Check(what, False, f"{command[0]} does not run: {e}")
    if run.returncode != 0:
        return Check(what, False, f"{command[0]} does not run: {(run.stderr or run.stdout).strip()[:200]}")
    return Check(what, True, command[0])


def user_name() -> str:
    try:
        return pwd.getpwuid(os.getuid()).pw_name
    except KeyError:
        return os.environ.get("USER", "")


def has_range(path: Path, name: str) -> bool:
    try:
        text = path.read_text()
    except OSError:
        return False
    return any(re.match(rf"{re.escape(key)}:\d+:[1-9]", line)
               for line in text.splitlines() for key in (name, str(os.getuid())))


def check_id_ranges() -> list:
    name = user_name()
    checks = []
    for path, flag in ((Path("/etc/subuid"), "--add-subuids"), (Path("/etc/subgid"), "--add-subgids")):
        ok = has_range(path, name)
        checks.append(Check(f"a range in {path} for {name}", ok,
                            "" if ok else f"sudo usermod {flag} 100000-165535 {name}"))
    return checks


def check_namespace() -> Check:
    """The namespace the build runs in, entered for real."""
    what = "a user namespace can be made"
    command = ["unshare", "--mount", "--fork", "--pid", "true"]
    if shutil.which("unshare") is None:
        return Check(what, False, "no unshare")
    if os.geteuid() != 0:
        command[1:1] = ["--map-auto", "--map-root-user"]
    try:
        run = subprocess.run(command, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return Check(what, False, str(e))
    if run.returncode != 0:
        return Check(what, False, run.stderr.strip()[:300] or f"unshare exit {run.returncode}")
    return Check(what, True)


def aarch64_program() -> bytes:
    """A complete aarch64 Linux program that exits with status 42."""
    code = struct.pack("<3I", 0xD2800540,   # mov x0, #42
                       0xD2800BA8,          # mov x8, #93 (exit)
                       0xD4000001)          # svc #0
    base, start = 0x400000, 64 + 56
    size = start + len(code)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = ident + struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, base + start, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, size, size, 0x1000)
    return header + segment + code


def elf_has_interpreter(path: str) -> bool:
    """Whether a 64-bit little-endian ELF asks for a dynamic loader (PT_INTERP)."""
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:6] != b"\x7fELF\x02\x01":
            return False
        phoff = struct.unpack_from("<Q", head, 32)[0]
        phentsize, phnum = struct.unpack_from("<HH", head, 54)
        f.seek(phoff)
        table = f.read(phentsize * phnum)
    return any(struct.unpack_from("<I", table, i * phentsize)[0] == 3 for i in range(phnum))


def run_aarch64() -> str | None:
    """Execute an aarch64 program through the kernel. None if it ran properly, else why not."""
    (ROOT / "build").mkdir(exist_ok=True)     # not /tmp, which may be noexec: the build runs from here
    with tempfile.TemporaryDirectory(prefix="doctor-", dir=ROOT / "build") as tmp:
        program = Path(tmp) / "exit42"
        program.write_bytes(aarch64_program())
        program.chmod(0o755)
        try:
            status = subprocess.run([str(program)], capture_output=True, timeout=30).returncode
        except (OSError, subprocess.TimeoutExpired) as e:
            return str(e)
    return None if status == 42 else f"it exited with {status}, not 42"


def check_qemu() -> list:
    """What runs the system's ARM programs in the chroot."""
    if platform.machine() in ("aarch64", "arm64"):
        return []
    what = "ARM programs run (qemu binfmt)"
    hint = install_hint("qemu-user")
    if not BINFMT.exists():
        return [Check(what, False, f"no qemu-aarch64 entry in binfmt_misc: {hint}")]
    entry = BINFMT.read_text()
    if not entry.startswith("enabled"):
        return [Check(what, False, f"the qemu-aarch64 entry is disabled ({BINFMT})")]
    m = re.search(r"^flags:\s*(\S*)", entry, re.M)
    if "F" not in (m.group(1) if m else ""):
        return [Check(what, False, "the qemu-aarch64 entry has no F flag, so it cannot start inside "
                                   f"the build's chroot: {hint}")]
    failed = run_aarch64()
    if failed:
        return [Check(what, False, f"a small aarch64 program did not run: {failed}")]
    checks = [Check(what, True, "ran a small aarch64 program")]
    m = re.search(r"^interpreter\s+(\S+)", entry, re.M)
    interpreter = m.group(1) if m else ""
    if interpreter and not Path(interpreter).exists():
        # with F the kernel keeps the file open, so it still runs today
        checks.append(Check("the qemu interpreter is installed", False, f"{interpreter} is gone: ARM programs "
                            f"stop running at the next reboot: {hint}", warning=True))
    elif interpreter and elf_has_interpreter(interpreter):
        checks.append(Check("the qemu interpreter is static", False, f"{interpreter} is dynamically linked, "
                            f"so it will not start inside the chroot: {hint}"))
    return checks


def build_checks() -> list:
    """Everything `ztos build` needs, whatever the target."""
    checks = [check_python()]
    checks += [check_tool(t) for t in BUILD_TOOLS]
    checks.append(check_mke2fs())
    checks.append(check_uic())
    if os.geteuid() != 0:
        checks += check_id_ranges()
    checks.append(check_namespace())
    checks += check_qemu()
    return checks


def phone_checks() -> list:
    """What putting the image on a phone takes, on top of building it."""
    return [check_tool(t) for t in PHONE_TOOLS]


def qemu_lists(kind: str, name: str) -> bool:
    """Whether QEMU itself offers this device or display. Asking it needs no path to its
    modules, which differ by distribution (/usr/lib/qemu, /usr/lib64/qemu...)."""
    try:
        out = subprocess.run(["qemu-system-aarch64", f"-{kind}", "help"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return re.search(rf'(^|[\s"]){re.escape(name)}($|[\s",])', out.stdout, re.M) is not None


def emulator_checks() -> list:
    """What `ztos emulator` uses. Optional: a build does not need them."""
    if shutil.which("qemu-system-aarch64") is None:
        return [Check("qemu-system-aarch64", False, install_hint("qemu-system"))]
    gl = qemu_lists("device", "virtio-gpu-gl-pci")
    window = qemu_lists("display", "gtk")
    return [Check("qemu-system-aarch64", True),
            Check("its graphics (virtio GPU with OpenGL)", gl, "" if gl else install_hint("qemu-gl")),
            Check("its window (GTK)", window, "" if window else install_hint("qemu-window"))]


def problems(checks: list) -> list:
    return [c for c in checks if not c.ok and not c.warning]


def describe(check: Check) -> str:
    return f"{check.what}: {check.detail}" if check.detail else check.what
