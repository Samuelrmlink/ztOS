"""The advertisements devices actually sent, from ztos-bt-adwatch (adwatch/):
the kernel's management Device Found events, passed on unchanged while the
bridge holds a connection to it (only while Android scans).

A packet: the controller index (2 bytes, little-endian), then Device Found's
parameters: address (6, little-endian), address type (1: 0 BR/EDR, 1 LE
public, 2 LE random), RSSI (1, signed), flags (4), data length (2), data (the
advertising data, then the scan response if there was one, as received).
"""
import os
import socket
import struct

SOCKET = "/run/ztos-bt-adwatch.sock"

# Device Found's flags (the kernel's mgmt.h)
NOT_CONNECTABLE = 0x04
SCAN_RSP = 0x20             # only a scan response: its advertisement isn't in this one

# Android's legacy event types (ScanResult.getEventType's bits: connectable 0x01,
# scannable 0x02, scan response 0x08, legacy 0x10)
ADV_IND = 0x13
ADV_NONCONN_IND = 0x10
SCAN_RSP_TO_ADV_IND = 0x1b
SCAN_RSP_TO_ADV_SCAN_IND = 0x1a

_FIXED = struct.Struct("<H6sBbIH")


def parse(packet):
    """One packet: (index, address, address type, rssi, flags, data), or None
    if it doesn't add up."""
    if len(packet) < _FIXED.size:
        return None
    index, raw_addr, addr_type, rssi, flags, n = _FIXED.unpack_from(packet)
    data = bytes(packet[_FIXED.size:])
    if len(data) != n or addr_type > 2:
        return None
    addr = ":".join("%02X" % b for b in reversed(raw_addr))
    return index, addr, addr_type, rssi, flags, data


def event_type(flags):
    """Android's event type for a report with these flags. Connectable is
    known; whether a non-connectable one was scannable isn't."""
    connectable = not flags & NOT_CONNECTABLE
    if flags & SCAN_RSP:
        return SCAN_RSP_TO_ADV_IND if connectable else SCAN_RSP_TO_ADV_SCAN_IND
    return ADV_IND if connectable else ADV_NONCONN_IND


class Watch:
    """The connection to ztos-bt-adwatch, held while wanted. on_ad(address,
    address type, rssi, flags, data) for each LE report, the data as received. Without the
    helper (not installed, not allowed) it does nothing: the bridge falls
    back to records rebuilt from BlueZ's properties."""

    def __init__(self, on_ad, log=None, path=SOCKET):
        self.on_ad = on_ad
        self.log = log or (lambda *a: None)
        self.path = path
        self.sock = None
        self.watch = None
        self.said = False

    def set(self, on):
        if on and self.sock is None:
            self._open()
        elif not on and self.sock is not None:
            self._close()

    def _open(self):
        from gi.repository import GLib
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC)
        try:
            st = os.stat(self.path)
            if st.st_uid != 0:          # the helper's socket is systemd's: root's
                raise OSError("not root's")
            s.connect(self.path)
        except OSError as e:
            s.close()
            if not self.said:
                self.log("raw advertisements: no ztos-bt-adwatch (%s); records rebuilt from BlueZ" % e)
                self.said = True
            return
        s.setblocking(False)
        self.sock = s
        self.said = False
        self.watch = GLib.io_add_watch(s.fileno(), GLib.PRIORITY_DEFAULT,
                                       GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self._readable)

    def _close(self):
        from gi.repository import GLib
        if self.watch is not None:
            GLib.source_remove(self.watch)
            self.watch = None
        self.sock.close()
        self.sock = None

    def _readable(self, _fd, cond):
        while True:
            try:
                packet = self.sock.recv(1024)
            except BlockingIOError:
                return True
            except OSError:
                packet = b""
            if not packet:
                self.watch = None
                self.sock.close()
                self.sock = None
                return False
            self.feed(packet)

    def feed(self, packet):
        rec = parse(packet)
        if rec is None:
            return
        _index, addr, addr_type, rssi, flags, data = rec
        if addr_type in (1, 2):
            self.on_ad(addr, addr_type, rssi, flags, data)
