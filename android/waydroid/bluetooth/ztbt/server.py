# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-btbridge's socket, its trust checks, the freeze watch and the store."""
import os
import socket
import struct
import time

from gi.repository import Gio, GLib

from . import proto as P

AID_BLUETOOTH = 1002
CONTAINER_CGROUP = "lxc.payload.waydroid"
CGROUP_EVENTS = "/sys/fs/cgroup/" + CONTAINER_CGROUP + "/cgroup.events"
QUEUE_MAX = 4096
MAX_FDS = 1             # a request carries at most one fd


def peer_cred(sock):
    pid, uid, gid = struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    return pid, uid, gid


def in_container(pid):
    try:
        with open("/proc/%d/cgroup" % pid) as f:
            return any(CONTAINER_CGROUP in line.split(":", 2)[-1] for line in f)
    except OSError:
        return False


class Client:
    """One connection from an Android Bluetooth stack."""

    def __init__(self, server, sock, who):
        self.server = server
        self.sock = sock
        self.who = who
        self.queue = []
        self.closed = False
        self.dropped = 0
        sock.setblocking(False)
        self.in_watch = GLib.io_add_watch(sock.fileno(), GLib.PRIORITY_DEFAULT,
                                          GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self._readable)
        self.out_watch = None

    def _readable(self, fd, cond):
        if cond & (GLib.IO_HUP | GLib.IO_ERR) and not cond & GLib.IO_IN:
            self.close()
            return False
        try:
            data, anc, flags, _ = self.sock.recvmsg(P.MAX_PACKET, socket.CMSG_SPACE(4 * MAX_FDS),
                                                    socket.MSG_CMSG_CLOEXEC)
        except BlockingIOError:
            return True
        except OSError:
            self.close()
            return False
        fds = []
        for level, kind, payload in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds.extend(struct.unpack("%di" % (len(payload) // 4), payload[:len(payload) // 4 * 4]))
        if not data or flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
            for fd in fds:
                os.close(fd)
            if data:
                self.server.log(self.who, "packet or its fds cut short")
            self.close()
            return False
        self.server.bridge.handle(self, data, fds)
        return not self.closed

    def send(self, packet, droppable=False):
        if self.closed:
            return
        if not self.queue:
            try:
                self.sock.send(packet)
                return
            except BlockingIOError:
                pass
            except OSError:
                self.close()
                return
        if droppable:       # scan results while Android can't read (frozen): gone
            self.dropped += 1
            return
        if len(self.queue) >= QUEUE_MAX:
            self.server.log(self.who, "not reading: closing")
            self.close()
            return
        self.queue.append(packet)
        if self.out_watch is None:
            self.out_watch = GLib.io_add_watch(self.sock.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_OUT,
                                               self._writable)

    def _writable(self, fd, cond):
        while self.queue:
            try:
                self.sock.send(self.queue[0])
            except BlockingIOError:
                return True
            except OSError:
                self.out_watch = None
                self.close()
                return False
            self.queue.pop(0)
        self.out_watch = None
        return False

    def close(self):
        if self.closed:
            return
        self.closed = True
        for w in (self.in_watch, self.out_watch):
            if w is not None:
                GLib.source_remove(w)
        self.out_watch = None
        self.sock.close()
        self.server.log(self.who, "gone", "(%d scan results dropped)" % self.dropped if self.dropped else "")
        self.server.bridge.detach(self)
        self.server.clients.discard(self)
        if not self.server.clients and self.server.on_empty:
            self.server.on_empty()


class Server:
    def __init__(self, bridge, path, allow_uid=AID_BLUETOOTH, container_check=True, freezer=True, log=None):
        self.bridge = bridge
        self.path = path
        self.allow_uid = allow_uid
        self.container_check = container_check
        self.freezer = freezer
        self.log = log or (lambda *a: None)
        self.clients = set()
        self.sock = None
        self.freeze_monitor = None
        self.on_empty = None

    def listen(self, fd=None):
        """Listen on self.path, or on fd (systemd's socket activation)."""
        if fd is not None:
            s = socket.socket(fileno=fd)
            s.setblocking(False)
            self.sock = s
            GLib.io_add_watch(s.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_IN, self._accept)
            self.log("listening on the socket systemd gave")
            return
        d = os.path.dirname(self.path)
        os.makedirs(d, mode=0o711, exist_ok=True)
        os.chmod(d, 0o711)
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC)
        s.bind(self.path)
        # anyone may connect; SO_PEERCRED decides (the Bluetooth app's uid, in the container)
        os.chmod(self.path, 0o666)
        s.listen(4)
        s.setblocking(False)
        self.sock = s
        GLib.io_add_watch(s.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_IN, self._accept)
        self.log("listening on", self.path)

    def _accept(self, fd, cond):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return True
        try:
            pid, uid, gid = peer_cred(conn)
        except OSError:
            conn.close()
            return True
        who = "pid %d uid %d" % (pid, uid)
        if uid != self.allow_uid or (self.container_check and not in_container(pid)):
            self.log(who, "refused")
            conn.close()
            return True
        self.log(who, "connected")
        c = Client(self, conn, who)
        self.clients.add(c)
        self.bridge.attach(c)
        if self.freezer:
            self.watch_freeze()
        return True

    # ---- the container's freezer --------------------------------------------

    def watch_freeze(self):
        """Follow the container's cgroup.events (inotify; no polling)."""
        if self.freeze_monitor is not None or not os.path.exists(CGROUP_EVENTS):
            self._check_frozen()
            return
        f = Gio.File.new_for_path(CGROUP_EVENTS)
        try:
            self.freeze_monitor = f.monitor_file(Gio.FileMonitorFlags.NONE, None)
        except GLib.Error as e:
            self.log("can't watch the freezer:", e.message)
            return
        self.freeze_monitor.connect("changed", lambda *a: self._check_frozen())
        self._check_frozen()

    def _check_frozen(self):
        frozen = False
        try:
            with open(CGROUP_EVENTS) as f:
                for line in f:
                    k, _, v = line.partition(" ")
                    if k == "frozen":
                        frozen = v.strip() == "1"
        except OSError:
            if self.freeze_monitor is not None:
                self.freeze_monitor.cancel()
                self.freeze_monitor = None
        self.bridge.set_frozen(frozen)


class Store:
    """The pairing hint for ztos-bt-agent, and the bonds Android made."""

    def __init__(self, runtime_dir, state_dir, log=None):
        self.hint_path = os.path.join(runtime_dir, "ztos-btbridge", "pairing")
        self.bonds_path = os.path.join(state_dir, "ztos-btbridge", "android-bonds")
        self.log = log or (lambda *a: None)
        os.makedirs(os.path.dirname(self.hint_path), mode=0o700, exist_ok=True)
        self.pairing_hint(None)

    def pairing_hint(self, addr):
        if addr is None:
            try:
                os.unlink(self.hint_path)
            except FileNotFoundError:
                pass
            return
        tmp = self.hint_path + ".tmp"
        with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(addr + "\n")
        os.replace(tmp, self.hint_path)

    def _bonds(self):
        try:
            with open(self.bonds_path) as f:
                return {line.strip().upper() for line in f if line.strip()}
        except FileNotFoundError:
            return set()

    def _save(self, bonds):
        os.makedirs(os.path.dirname(self.bonds_path), mode=0o700, exist_ok=True)
        tmp = self.bonds_path + ".tmp"
        with open(tmp, "w") as f:
            f.write("".join(b + "\n" for b in sorted(bonds)))
        os.replace(tmp, self.bonds_path)

    def add_android_bond(self, addr):
        self._save(self._bonds() | {addr.upper()})

    def is_android_bond(self, addr):
        return addr.upper() in self._bonds()

    def forget_android_bond(self, addr):
        self._save(self._bonds() - {addr.upper()})


def log(*words):
    print(time.strftime("%H:%M:%S"), *[w for w in words if w != ""], flush=True)

