"""Heart: heart rate (and an experimental SpO2) from the sensor beside the camera.

It asks zrn-hrmd (sensors/hrm/zrnhrmd) for a measurement through
/run/zrn-hrmd.ctl and shows /run/zrn-sensors/hrm as it changes. The sensor's
LEDs are on only while a measurement runs.

Not a medical device: the SpO2 figure is uncalibrated for this phone.
"""
import os

import ztos
from PySide6.QtCore import QPointF, Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

CTL = "/run/zrn-hrmd.ctl"
STATE = "/run/zrn-sensors/hrm"
WAVE = "/run/zrn-sensors/hrm-wave"
SECONDS = 30

app = ztos.App()
store = ztos.store()


def read_state():
    try:
        with open(STATE) as f:
            return dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
    except OSError:
        return {}


def read_wave():
    try:
        with open(WAVE) as f:
            return [float(v) for v in f.read().split()]
    except (OSError, ValueError):
        return []


def send(cmd):
    try:
        fd = os.open(CTL, os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        os.write(fd, (cmd + "\n").encode())
    finally:
        os.close(fd)
    return True


class Trace(QWidget):
    """The pulse wave, as zrn-hrmd last wrote it."""

    def __init__(self):
        super().__init__()
        self.setFixedHeight(120)
        self.points = []

    def set_points(self, points):
        self.points = points
        self.update()

    def paintEvent(self, ev):
        if len(self.points) < 2:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(ztos.accent(), 2.5))
        w, h = self.width(), self.height()
        n = len(self.points)
        path = QPainterPath(QPointF(0, h / 2 - self.points[0] * h * 0.45))
        for i, v in enumerate(self.points[1:], 1):
            path.lineTo(QPointF(i * w / (n - 1), h / 2 - v * h * 0.45))
        p.drawPath(path)


class Home(ztos.Page):
    def __init__(self):
        super().__init__("heart rate")
        self.big = self.text("--", size=64)
        self.unit = self.text("beats per minute", size=15, color=QColor("#999999"))
        self.trace = self.add(Trace(), gap=8)
        self.space(12)
        self.status = self.text("Cover the sensor beside the camera with a fingertip, "
                                "keep still, then tap measure.")
        self.spo2 = self.entry("SpO2 (experimental)", "--")
        self.progress = self.add(ztos.ProgressBar(0, caption="measuring"))
        self.space(12)
        self.go = self.button("measure", self.toggle)
        self.text("Not a medical device. Heart rate is good at rest; the SpO2 figure is "
                  "not calibrated for this phone and must not be used to judge oxygen levels.",
                  size=13, color=QColor("#999999"))
        self.running = False
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        last = store.get("last")
        if last:
            self.big.setText(str(last))

    def toggle(self):
        if self.running:
            send("stop")
            return
        if not send(f"start {SECONDS}"):
            self.status.setText("The heart rate service is not running.")
            return
        self.running = True
        self.go.setText("stop")
        self.timer.start(500)

    def refresh(self):
        st = read_state()
        state = st.get("state", "")
        bpm = st.get("bpm", "")
        self.big.setText(bpm or "--")
        sp = st.get("spo2", "")
        self.spo2.set_detail(f"{sp} %  (uncalibrated)" if sp else "--")
        total = max(1, int(st.get("total", SECONDS) or SECONDS))
        self.progress.set_value(100 * int(st.get("elapsed", 0) or 0) / total)
        self.trace.set_points(read_wave() if state == "measuring" else [])
        self.status.setText({
            "measuring": "Measuring: keep still." if bpm else "Finding your pulse: keep still.",
            "no-finger": "No finger on the sensor, or too much movement.",
            "error": f"Sensor error: {st.get('error', '')}",
            "done": "Done." if bpm else "No steady pulse found. Try again, keeping very still.",
            "stopped": "Stopped.",
        }.get(state, ""))
        if state in ("done", "stopped", "error"):
            self.finish(bpm)

    def finish(self, bpm):
        self.running = False
        self.timer.stop()
        self.go.setText("measure")
        if bpm:
            store["last"] = bpm
            store.save()
            ztos.tile_update("Heart rate", ["last measured"], value=f"{bpm} bpm")


app.run(Home())
