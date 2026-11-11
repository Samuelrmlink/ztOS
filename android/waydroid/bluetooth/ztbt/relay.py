# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy bytes both ways between Android's stream and an RFCOMM socket.

Android holds only its end of a Unix socketpair; the RFCOMM socket stays in
the bridge. `prefix` goes to Android first (the channel and connect signal its
BluetoothSocket waits for). Non-blocking, on the GLib main loop; at most LIMIT
bytes wait in each direction, so a slow printer slows Android down instead of
filling memory. When one side ends, what is still owed to the other is
written out, then both are closed and on_close() runs (not after close()).
"""
import errno
import os

from gi.repository import GLib

LIMIT = 256 * 1024
CHUNK = 64 * 1024


class Relay:
    def __init__(self, a, b, prefix=b"", on_close=None):
        self.fds = (a, b)
        self.out = [bytearray(prefix), bytearray()]   # waiting to be written to a, to b
        self.eof = [False, False]                     # a ended, b ended
        self.on_close = on_close
        self.closed = False
        self.watches = [None, None]
        for fd in self.fds:
            os.set_blocking(fd, False)
        self._update()

    def _cond(self, i):
        c = 0
        other = 1 - i
        if not self.eof[i] and len(self.out[other]) < LIMIT:
            c |= GLib.IO_IN
        if self.out[i]:
            c |= GLib.IO_OUT
        return c

    def _update(self):
        if self.closed:
            return
        for i in (0, 1):
            if self.watches[i] is not None:
                GLib.source_remove(self.watches[i])
                self.watches[i] = None
            c = self._cond(i)
            if c:
                cond = GLib.IOCondition(int(c) | int(GLib.IO_HUP) | int(GLib.IO_ERR))
                self.watches[i] = GLib.io_add_watch(self.fds[i], GLib.PRIORITY_DEFAULT, cond, self._ready, i)
        # an end with nothing left to give and nothing owed: done
        for i in (0, 1):
            if self.eof[i] and not self.out[1 - i]:
                self._finish()
                return

    def _ready(self, fd, cond, i):
        if self.closed:
            return False
        try:
            if cond & GLib.IO_OUT and self.out[i]:
                n = os.write(fd, self.out[i])
                del self.out[i][:n]
            if cond & (GLib.IO_IN | GLib.IO_HUP):
                if not self.eof[i]:
                    data = os.read(fd, CHUNK)
                    if data:
                        self.out[1 - i] += data
                    else:
                        self.eof[i] = True
            if cond & GLib.IO_ERR and not cond & GLib.IO_IN:
                self.eof[i] = True
                self.out[i].clear()
        except BlockingIOError:
            pass
        except OSError as e:
            if e.errno not in (errno.EINTR, errno.EAGAIN):
                self.eof[i] = True
                self.out[i].clear()      # this side is gone: nothing more reaches it
        self.watches[i] = None           # _update makes a fresh one
        GLib.idle_add(self._update_once)
        return False

    def _update_once(self):
        self._update()
        return False

    def _finish(self):
        cb = self.on_close
        self.close()
        if cb:
            cb()

    def close(self):
        """Close both ends now; on_close is not called."""
        if self.closed:
            return
        self.closed = True
        self.on_close = None
        for i in (0, 1):
            if self.watches[i] is not None:
                GLib.source_remove(self.watches[i])
                self.watches[i] = None
            try:
                os.close(self.fds[i])
            except OSError:
                pass
