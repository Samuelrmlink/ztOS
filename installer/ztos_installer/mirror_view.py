# SPDX-License-Identifier: GPL-2.0-or-later
"""The window of ztos mirror: the live screen, and the mouse as a finger.

JPEG frames decode with Qt, on the network thread, so a window with nothing
but PySide6 works on any host without codec packages (Fedora and EL have no
H.264 decoder without RPM Fusion: see ztmetro/mirror/README.md).
"""
from __future__ import annotations

import threading
import time

from PySide6.QtCore import QObject, QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen
from PySide6.QtWidgets import QApplication, QWidget

from . import mirror as m

HELP = ("mouse: one finger   Ctrl+drag: two, mirrored (pinch)   right-click: pin a finger, again to lift it\n"
        "Esc back   Home home   PgUp/PgDn volume   P power (hold for the menu)")

KEY_MAP = {
    Qt.Key_Escape: "back", Qt.Key_Backspace: "back", Qt.Key_Home: "home", Qt.Key_H: "home",
    Qt.Key_PageUp: "volup", Qt.Key_Plus: "volup", Qt.Key_Equal: "volup",
    Qt.Key_PageDown: "voldown", Qt.Key_Minus: "voldown", Qt.Key_P: "power",
}

MOUSE, MIRRORED = 0, 1          # the fingers the mouse and Ctrl drive
PINS = range(2, m.MAX_FINGERS)  # pinned ones


class Bridge(QObject):
    frame = Signal(QImage, int)     # image, bytes
    status = Signal(dict)
    link = Signal(str)              # "" when up, else what is wrong


class Connector:
    """Keeps a link up: it dies, this makes another. A refusal (a device that
    restarted has a new token) or nothing listening asks SSH to start it again."""

    def __init__(self, device, bridge, **settings):
        self.device, self.bridge, self.settings = device, bridge, settings
        self.link = None
        self.stopping = threading.Event()
        self.take_back = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _on_frame(self, jpeg, w, h, seq, device_us, encode_us):
        image = QImage.fromData(jpeg, "JPEG")
        if not image.isNull():
            self.bridge.frame.emit(image, len(jpeg))

    def _run(self):
        restart = True
        while not self.stopping.is_set():
            try:
                if restart:
                    self.bridge.link.emit("starting ztmirror on the device")
                    self.device.start()
                    restart = False
                lost = threading.Event()
                self.link = m.Link(self.device, on_frame=self._on_frame, on_status=self.bridge.status.emit,
                                   on_closed=lost.set, **self.settings)
                self.bridge.status.emit(self.link.status)
                self.bridge.link.emit("")
                while not lost.wait(0.5):
                    if self.stopping.is_set():
                        self.link.close()
                        return
                if self.link.taken_over:
                    # ztos touch, say. Coming straight back would fight it for the device.
                    self.bridge.link.emit("another client has the device\nclick to take it back")
                    self.take_back.clear()
                    while not self.take_back.wait(0.5):
                        if self.stopping.is_set():
                            return
                    continue
                self.bridge.link.emit("the link dropped: reconnecting")
            except m.Refused as e:
                self.bridge.link.emit(f"refused: {e}")
                restart = True
            except m.Fail as e:
                self.bridge.link.emit(str(e))
            except (OSError, ValueError) as e:
                self.bridge.link.emit(f"no link to {self.device.host}:{self.device.port}: reconnecting ({e})")
                restart = True
            self.link = None
            self.stopping.wait(1)

    def stop(self):
        self.stopping.set()
        if self.link:
            self.link.close()


class View(QWidget):
    def __init__(self, connector, bridge):
        super().__init__()
        self.connector = connector
        self.image = None
        self.points = None
        self.screen_state = "on"
        self.link_problem = "connecting"
        self.mouse_down = self.mirrored_down = False
        self.pins = {}          # finger: (x, y)
        self.keys_down = set()
        self.frames = self.bytes = 0
        self.last_frame = 0.0
        self.fps = self.mbit = 0.0
        self.setWindowTitle("ztOS")
        self.setFocusPolicy(Qt.StrongFocus)
        self.setMouseTracking(False)
        self.setMinimumSize(180, 370)
        bridge.frame.connect(self.on_frame)
        bridge.status.connect(self.on_status)
        bridge.link.connect(self.on_link)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(1000)
        available = QApplication.primaryScreen().availableGeometry()
        h = min(int(available.height() * 0.9), 1000)
        self.resize(h * 360 // 740, h)

    # ---- what arrives ----
    def on_frame(self, image, size):
        first = self.image is None
        self.image = image
        self.frames += 1
        self.bytes += size
        self.last_frame = time.monotonic()
        if first:
            self.fit_aspect()
        self.update()

    def fit_aspect(self):
        h = self.height()
        self.resize(h * self.image.width() // self.image.height(), h)

    def on_status(self, status):
        self.points = status.get("points")
        if self.screen_state != status.get("screen"):
            self.screen_state = status.get("screen")
            self.update()

    def on_link(self, problem):
        self.link_problem = problem
        if problem:
            self.release_all(tell=False)
        self.update()

    def tick(self):
        self.fps, self.mbit = self.frames, self.bytes * 8 / 1e6
        self.frames = self.bytes = 0
        link = self.connector.link
        rtt = f"  {link.rtt_ms} ms" if link and link.rtt_ms is not None else ""
        self.setWindowTitle(f"ztOS  {self.fps} fps  {self.mbit:.0f} Mbit/s{rtt}")

    # ---- where things are on the widget ----
    def image_rect(self):
        if not self.image:
            return QRectF(self.rect())
        w, h = self.image.width(), self.image.height()
        s = min(self.width() / w, self.height() / h)
        return QRectF((self.width() - w * s) / 2, (self.height() - h * s) / 2, w * s, h * s)

    def unit(self, pos):
        r = self.image_rect()
        return (min(1.0, max(0.0, (pos.x() - r.x()) / r.width())),
                min(1.0, max(0.0, (pos.y() - r.y()) / r.height())))

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), Qt.black)
        r = self.image_rect()
        if self.image:
            p.setRenderHint(QPainter.SmoothPixmapTransform)
            p.drawImage(r, self.image)
        message = None
        if self.link_problem:
            message = self.link_problem
        elif self.screen_state == "off":
            message = "the screen is off\nno frames until it wakes: press P"
        elif not self.image:
            message = "waiting for the first frame\n\n" + HELP
        if message:
            p.fillRect(self.rect(), QColor(0, 0, 0, 170))
            p.setPen(Qt.white)
            p.setFont(QFont(p.font().family(), 11))
            p.drawText(self.rect().adjusted(16, 16, -16, -16), Qt.AlignCenter | Qt.TextWordWrap, message)
        # the fingers we hold, for the eye only
        p.setPen(QPen(QColor(255, 255, 255, 220), 2))
        p.setBrush(QColor(255, 80, 0, 110))
        dots = list(self.pins.values())
        if self.mouse_down:
            dots.append(self.mouse_at)
            if self.mirrored_down:
                dots.append((1 - self.mouse_at[0], 1 - self.mouse_at[1]))
        for x, y in dots:
            p.drawEllipse(QPointF(r.x() + x * r.width(), r.y() + y * r.height()), 12, 12)

    # ---- fingers ----
    def link(self):
        link = self.connector.link
        return link if link and not self.link_problem else None

    def mousePressEvent(self, e):
        link = self.link()
        if not link:
            self.connector.take_back.set()
            return
        x, y = self.unit(e.position())
        if e.button() == Qt.LeftButton:
            self.mouse_down = True
            self.mouse_at = (x, y)
            link.down(MOUSE, x, y)
            if e.modifiers() & Qt.ControlModifier:
                self.mirrored_down = True
                link.down(MIRRORED, 1 - x, 1 - y)
        elif e.button() == Qt.RightButton:
            near = [f for f, (px, py) in self.pins.items() if abs(px - x) < 0.05 and abs(py - y) < 0.03]
            if near:
                link.up(near[0])
                del self.pins[near[0]]
            else:
                free = [f for f in PINS if f not in self.pins]
                if free:
                    self.pins[free[0]] = (x, y)
                    link.down(free[0], x, y)
        self.update()

    def mouseMoveEvent(self, e):
        link = self.link()
        if not link or not self.mouse_down:
            return
        x, y = self.unit(e.position())
        self.mouse_at = (x, y)
        link.move(MOUSE, x, y)
        if self.mirrored_down:
            link.move(MIRRORED, 1 - x, 1 - y)
        self.update()

    def mouseReleaseEvent(self, e):
        if e.button() != Qt.LeftButton or not self.mouse_down:
            return
        link = self.link()
        if link:
            link.up(MOUSE)
            if self.mirrored_down:
                link.up(MIRRORED)
        self.mouse_down = self.mirrored_down = False
        self.update()

    def release_all(self, tell=True):
        link = self.link() if tell else None
        if link:
            if self.mouse_down:
                link.cancel(MOUSE)
            if self.mirrored_down:
                link.cancel(MIRRORED)
            for f in self.pins:
                link.cancel(f)
            for k in self.keys_down:
                link.key(k, False)
        self.mouse_down = self.mirrored_down = False
        self.pins.clear()
        self.keys_down.clear()
        self.update()

    def focusOutEvent(self, e):
        # the release may go to another window: nothing stays held behind our back
        self.release_all()

    # ---- keys ----
    def keyPressEvent(self, e):
        name = KEY_MAP.get(e.key())
        link = self.link()
        if not name or e.isAutoRepeat() or name in self.keys_down or not link:
            return
        self.keys_down.add(name)
        link.key(name, True)

    def keyReleaseEvent(self, e):
        name = KEY_MAP.get(e.key())
        if not name or e.isAutoRepeat() or name not in self.keys_down:
            return
        self.keys_down.discard(name)
        link = self.link()
        if link:
            link.key(name, False)

    def closeEvent(self, e):
        self.release_all()
        self.connector.stop()
        e.accept()


def run(device, scale=2, quality=70, fps=30):
    app = QApplication.instance() or QApplication([])
    bridge = Bridge()
    connector = Connector(device, bridge, scale=scale, quality=quality, fps=fps)
    view = View(connector, bridge)
    view.show()
    print(HELP)
    app.exec()
    connector.stop()
