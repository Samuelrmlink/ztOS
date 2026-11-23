#!/usr/bin/env python3
"""ztos-bt-reconnect's decisions (ROADMAP 8.52), on the host: make check-bt-reconnect.

BlueZ is played by this test (its signals as dbus-python hands them over, and
Connect()'s answers); PipeWire by pw-dump data that goes through ztos-audiod's
own code to the state file the daemon reads. No D-Bus, no PipeWire, no phone."""
import configparser
import importlib.machinery
import importlib.util
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "../..")


def load(name, rel):
    loader = importlib.machinery.SourceFileLoader(name, os.path.join(ROOT, rel))
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


r = load("ztos_bt_reconnect", "rootfs/overlay/usr/local/bin/ztos-bt-reconnect")
a = load("ztos_audiod", "rootfs/overlay/usr/local/bin/ztos-audiod")
r.log = lambda *w: None

PODS = "40:DA:5C:62:CA:1B"
BOOM = "00:0D:44:EB:50:54"
CHAM = "E3:19:D9:61:F8:3B"
ADAPTER = "/org/bluez/hci0"
A2DP_SINK = "0000110b-0000-1000-8000-00805f9b34fb"
HFP_HF = "0000111e-0000-1000-8000-00805f9b34fb"
NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
GATT = "00001801-0000-1000-8000-00805f9b34fb"


def path(mac):
    return "%s/dev_%s" % (ADAPTER, mac.replace(":", "_"))


def sink(mac):
    return "bluez_output.%s.1" % mac.replace(":", "_")


# ---- PipeWire, as pw-dump says it, through ztos-audiod to its state file
def node(i, props, state="idle"):
    return {"id": i, "type": "PipeWire:Interface:Node",
            "info": {"state": state, "props": props, "params": {"Props": [{"mute": False, "channelVolumes": [0.5]}]}}}


def link(o, i):
    return {"id": 900 + o * 10 + i, "type": "PipeWire:Interface:Link", "info": {"output-node-id": o, "input-node-id": i}}


def graph(bt=(), app_to=None, app_state="running", eq=False):
    """bt: the Bluetooth devices with an output; app_to: where the one app's stream goes
    ("speaker", or an address); eq: through ztos-eq (its chain then plays into that output)."""
    d = [node(42, {"node.name": "ztos-speaker", "media.class": "Audio/Sink", "node.description": "Speaker"})]
    ids = {"speaker": 42}
    for n, mac in enumerate(bt):
        i = 50 + n
        ids[mac] = i
        d.append(node(i, {"node.name": sink(mac), "media.class": "Audio/Sink", "node.description": mac,
                          "device.api": "bluez5", "api.bluez5.address": mac}))
    if app_to is not None:
        d.append(node(70, {"node.name": "Grayjay", "media.class": "Stream/Output/Audio",
                           "application.name": "Grayjay", "media.name": "song"}, app_state))
        if eq:
            d.append(node(80, {"node.name": "ztos-eq", "media.class": "Audio/Sink", "node.link-group": "eq"}))
            d.append(node(81, {"node.name": "ztos-eq.out", "media.class": "Stream/Output/Audio",
                               "node.link-group": "eq"}, "running"))
            d += [link(70, 80), link(81, ids[app_to])]
        else:
            d.append(link(70, ids[app_to]))
    return d


def audiod_state(dump):
    """What ztos-audiod writes, from its own functions."""
    g = a.read_graph(dump)
    sink_ids = {s["id"]: s["name"] for s in g["sinks"].values()}
    on = {s["id"]: a.playing_on(s["id"], g["links"], sink_ids, g["chains"]) for s in g["streams"]}
    outputs = a.output_entries(g["sinks"], "ztos-speaker", a.playing_outputs(g["streams"], on))
    return json.dumps({"version": 1, "active": None, "outputs": outputs, "apps": []})


# ---- BlueZ and the main loop, played here
class FakeIO:
    def __init__(self):
        self.t = 1000.0
        self.timers = {}
        self.n = 0
        self.connects = []         # (time, path)
        self.answer = {}           # path -> list of errors to answer Connect() with (None: connected)
        self.props_asked = []
        self.props = {}
        self.pauses = []           # times the players were paused

    def clock(self):
        return self.t

    def after(self, seconds, fn):
        self.n += 1
        self.timers[self.n] = (self.t + seconds, fn)
        return self.n

    def cancel(self, h):
        self.timers.pop(h, None)

    def connect(self, p, done):
        self.connects.append((self.t, p))
        answers = self.answer.get(p) or ["org.bluez.Error.Failed"]
        err = answers.pop(0) if len(answers) > 1 else answers[0]
        # BlueZ answers a page that finds nothing after about 5 s
        self.after(5, lambda: done(err))

    def pause(self):
        self.pauses.append(self.t)

    def device_props(self, p, cb):
        self.props_asked.append(p)
        cb(self.props.get(p))

    def run(self, seconds):
        end = self.t + seconds
        while True:
            due = [(t, h) for h, (t, _) in self.timers.items() if t <= end]
            if not due:
                break
            t, h = min(due)
            self.t = max(self.t, t)
            _, fn = self.timers.pop(h)
            fn()
        self.t = end


def world(devices=((PODS, [A2DP_SINK, HFP_HF]), (BOOM, [A2DP_SINK, HFP_HF]), (CHAM, [GATT, NUS]))):
    io = FakeIO()
    d = r.Daemon(io, clock=io.clock)
    objs = {ADAPTER: {r.ADAPTER_IFACE: {"Powered": True}}}
    for mac, uuids in devices:
        objs[path(mac)] = {r.DEVICE_IFACE: {"Address": mac, "UUIDs": uuids, "Connected": True,
                                            "Paired": True, "Trusted": True}}
    d.objects(objs)
    return io, d


def drop(io, d, mac, reason="org.bluez.Reason.Timeout", signal_first=True):
    """The link goes: BlueZ's Disconnected signal and Connected=false."""
    if signal_first and reason:
        d.disconnected_signal(reason, path(mac))
    d.properties_changed(r.DEVICE_IFACE, {"Connected": False}, path(mac))
    if not signal_first and reason:
        io.run(0.3)
        d.disconnected_signal(reason, path(mac))


def back(d, mac):
    d.properties_changed(r.DEVICE_IFACE, {"Connected": True}, path(mac))


def calls(io, mac=None):
    return [t for t, p in io.connects if mac is None or p == path(mac)]


# 1. playing through the AirPods, out of range mid-song: called back, and stops once back
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
state = json.loads(audiod_state(graph(bt=[PODS], app_to=PODS)))
o = next(o for o in state["outputs"] if o["name"] == sink(PODS))
assert o["playing"] is True and o["address"] == PODS, o
drop(io, d, PODS)
d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))    # the sink goes; the song moves to the speaker
io.run(2.9)
assert calls(io) == [], io.connects
io.run(0.2)
assert len(calls(io, PODS)) == 1, io.connects                  # 3 s after it dropped
io.run(30)
assert len(calls(io, PODS)) >= 3, io.connects                  # 10 s, 20 s, ...
n = len(calls(io))
back(d, PODS)
d.audio_text(audiod_state(graph(bt=[PODS], app_to="speaker")))
io.run(600)
assert len(calls(io)) == n, "kept calling after it came back"
# ...and ztos-audiod hands it the sound as it returns (it is the only Bluetooth output)
assert a.takeover_target({"ztos-speaker"}, {"ztos-speaker": {"kind": "speaker"},
                                            sink(PODS): {"name": sink(PODS), "kind": "bluetooth"}}) == sink(PODS)

# 2. bounded: never back, the calls stop at the window, spaced out, one at a time
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
drop(io, d, PODS)
io.run(3600)
ts = calls(io, PODS)
assert ts and ts[-1] - 1000 <= r.WINDOW_S, ts
assert 6 <= len(ts) <= 15, ts
assert all(b - a_ >= 5 for a_, b in zip(ts, ts[1:])), ts       # never a second while one is out
assert not d.policy.pending

# 2b. a connect that succeeds stops it too (Connect()'s answer, before the signal)
io, d = world()
io.answer[path(PODS)] = ["org.bluez.Error.Failed", None]
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
drop(io, d, PODS)
io.run(600)
assert len(calls(io, PODS)) == 2, io.connects

# 3. connected but paused: the app's stream is linked to it but not running
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, app_state="paused")))
assert json.loads(audiod_state(graph(bt=[PODS], app_to=PODS, app_state="paused")))["outputs"][0]["playing"] is False
drop(io, d, PODS)
io.run(600)
assert calls(io) == [], io.connects

# 3b. paused just before it went (playing earlier does not count)
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, app_state="paused")))
drop(io, d, PODS, reason="org.bluez.Reason.Remote")
io.run(600)
assert calls(io) == [], io.connects

# 4. playing on the phone's speaker, the AirPods connected and idle: not called back
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to="speaker")))
drop(io, d, PODS)
io.run(600)
assert calls(io) == [], io.connects

# 5. only the device the sound was going through: playing on the AirPods, the
#    speaker (BOOM) connected beside them and idle
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS, BOOM], app_to=PODS)))
drop(io, d, BOOM)
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
io.run(600)
assert calls(io) == [], "BOOM was called back, but the sound was on the AirPods"
drop(io, d, PODS)
io.run(5)
assert calls(io, PODS) and not calls(io, BOOM), io.connects

# 5b. ...and the other way round: playing on BOOM, the AirPods drop
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS, BOOM], app_to=BOOM)))
drop(io, d, PODS)
io.run(600)
assert calls(io) == [], io.connects

# 6. through ztos-eq (the chain plays into the AirPods): counts as playing through them
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, eq=True)))
drop(io, d, PODS, reason="org.bluez.Reason.Remote")   # switched off mid-song
io.run(5)
assert calls(io, PODS), io.connects

# 7. disconnected from the phone (Settings' "disconnect", Bluetooth off): never
for reason in ("org.bluez.Reason.Local", "org.bluez.Reason.Authentication"):
    io, d = world()
    d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
    drop(io, d, PODS, reason=reason)
    io.run(600)
    assert calls(io) == [], (reason, io.connects)

# 7b. the reason after Connected=false (either order) still counts
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
drop(io, d, PODS, reason="org.bluez.Reason.Local", signal_first=False)
io.run(600)
assert calls(io) == [], io.connects

# 7c. suspend while playing, and no reason given at all: called back
for reason in ("org.bluez.Reason.Suspend", None):
    io, d = world()
    d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
    drop(io, d, PODS, reason=reason)
    io.run(5)
    assert calls(io, PODS), reason

# 8. not an audio device: the Chameleon (cubridge connects it by address) is never
#    touched, whatever happens; cubridge's own Connect() goes to BlueZ as before
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
for _ in range(3):
    drop(io, d, CHAM, reason="org.bluez.Reason.Timeout")
    io.run(60)
    back(d, CHAM)                       # cubridge's Connect()
    io.run(60)
assert calls(io) == [] and io.props_asked == [], (io.connects, io.props_asked)
# even if an output with its address appeared (it can't), its UUIDs say no
io, d = world()
d.audio_text(json.dumps({"outputs": [{"name": sink(CHAM), "kind": "bluetooth", "playing": True}]}))
drop(io, d, CHAM)
io.run(600)
assert calls(io) == [], io.connects

# 8b. UUIDs not known yet: asked for, and a non-audio answer means no
io, d = world(devices=())
d.objects({ADAPTER: {r.ADAPTER_IFACE: {"Powered": True}}})
d.properties_changed(r.DEVICE_IFACE, {"Connected": True}, path(BOOM))
io.props[path(BOOM)] = {"UUIDs": [GATT]}
d.audio_text(audiod_state(graph(bt=[BOOM], app_to=BOOM)))
drop(io, d, BOOM)
io.run(600)
assert io.props_asked == [path(BOOM)] and calls(io) == [], io.connects

# 9. Bluetooth switched off, the device removed, bluetoothd gone: the calls stop
for stop in ("off", "removed", "gone"):
    io, d = world()
    d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
    drop(io, d, PODS)
    io.run(15)
    n = len(calls(io))
    assert n >= 1
    if stop == "off":
        d.properties_changed(r.ADAPTER_IFACE, {"Powered": False}, ADAPTER)
    elif stop == "removed":
        d.interfaces_removed(path(PODS), [r.DEVICE_IFACE])
    else:
        d.bluez_owner("")
    io.run(600)
    assert len(calls(io)) == n, (stop, io.connects)

# 10. boot, or bluetoothd/WirePlumber starting: nothing is called (nothing dropped here)
io, d = world()
d.bluez_owner(":1.5")
d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))
io.run(600)
assert calls(io) == []

# 11. its output went well before the link (a profile change, say): too old to count
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
io.run(10)
d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))
io.run(r.GONE_GRACE_S + 5)
drop(io, d, PODS)
io.run(600)
assert calls(io) == [], io.connects
# ...within the grace it does (the sink goes a moment before BlueZ says so)
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))
io.run(2)
drop(io, d, PODS)
io.run(5)
assert calls(io, PODS), io.connects

# 11b. the stream moves to the speaker (the AirPods' output still there, idle) after
#      Connected=false but before BlueZ's reason: still playing as it dropped
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
drop(io, d, PODS, signal_first=False)
d.audio_text(audiod_state(graph(bt=[PODS], app_to="speaker")))
io.run(5)
assert calls(io, PODS), io.connects

# 12. one disconnect, one decision: back, then dropped again idle, not called back
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
drop(io, d, PODS)
io.run(5)
back(d, PODS)
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, app_state="paused")))
n = len(calls(io))
drop(io, d, PODS)
io.run(600)
assert len(calls(io)) == n, io.connects

# ---- ROADMAP 8.64: paused as it drops, never played again by anything here
# 14. playing through the AirPods, put away mid-song: the players pause at once
#     (before BlueZ's reason, before the sound reaches the speaker), once
for reason in ("org.bluez.Reason.Remote", "org.bluez.Reason.Timeout", "org.bluez.Reason.Local", None):
    for first in (True, False):
        io, d = world()
        d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
        drop(io, d, PODS, reason=reason, signal_first=first)
        assert io.pauses and io.pauses[0] <= 1000.3, (reason, io.pauses)
        d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))
        io.run(600)
        assert len(io.pauses) == 1, (reason, io.pauses)

# 14b. through ztos-eq too; and the output gone a moment before the link
io, d = world()
d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, eq=True)))
d.audio_text(audiod_state(graph(bt=[], app_to="speaker", eq=True)))
io.run(0.5)
drop(io, d, PODS)
assert len(io.pauses) == 1, io.pauses

# 15. nothing to pause: paused already, playing on the speaker, on another
#     headset, or a device that isn't audio
for g, mac in ((graph(bt=[PODS], app_to=PODS, app_state="paused"), PODS),
               (graph(bt=[PODS], app_to="speaker"), PODS),
               (graph(bt=[PODS, BOOM], app_to=BOOM), PODS),
               (graph(bt=[PODS], app_to=PODS), CHAM)):
    io, d = world()
    d.audio_text(audiod_state(g))
    drop(io, d, mac)
    io.run(600)
    assert io.pauses == [], (mac, io.pauses)

# 16. back, called back or by itself: nothing plays. The only things this daemon
#     does are Connect() and pause() (FakeIO has nothing else, so a play would
#     fail here), and its pause commands can't start a player.
for called_back in (True, False):
    io, d = world()
    d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS)))
    drop(io, d, PODS, reason="org.bluez.Reason.Remote" if called_back else "org.bluez.Reason.Local")
    d.audio_text(audiod_state(graph(bt=[], app_to="speaker")))
    io.run(4 if called_back else 60)
    assert bool(calls(io, PODS)) == called_back, io.connects
    back(d, PODS)
    d.audio_text(audiod_state(graph(bt=[PODS], app_to=PODS, app_state="paused")))
    io.run(600)
    assert len(io.pauses) == 1, io.pauses
    # dropped again, now paused: no second pause, no call back
    n = len(calls(io))
    drop(io, d, PODS)
    io.run(600)
    assert len(io.pauses) == 1 and len(calls(io)) == n, (io.pauses, io.connects)
for cmd in r.PAUSE_COMMANDS:
    assert "pause" == cmd[-1] and not any(w in ("play", "play-pause", "toggle") for w in cmd), cmd
src = open(os.path.join(ROOT, "rootfs/overlay/usr/local/bin/ztos-bt-reconnect")).read()
assert "play-pause" not in src and '"play"' not in src

# 17. Android's side: recover.sh's media action takes only "pause", and the sudoers
#     rule (both copies) allows only that
rec = open(os.path.join(ROOT, "android/waydroid/recover.sh")).read()
assert '[[ $# == 2 && $2 == pause ]] || exit 2' in rec and "cmd media_session dispatch" in rec
for rel in ("packages/ztos-waydroid/sudoers.template", "android/waydroid/install.sh"):
    rule = open(os.path.join(ROOT, rel)).read()
    assert "ztos-waydroid-recover media pause," in rule, rel
    assert "ztos-waydroid-recover media *" not in rule and "ztos-waydroid-recover media play" not in rule, rel

# 13. a state file that can't be read changes nothing
assert r.bt_outputs("{") is None and r.bt_outputs('{"outputs": 3}') is None
assert r.bt_outputs(json.dumps({"outputs": [{"name": sink(PODS), "kind": "bluetooth", "playing": True}]})) == {PODS: True}
assert r.mac_of_path(path(PODS)) == PODS and r.mac_of_path(path(PODS) + "/sep1") is None

# ---- the configuration: nothing else reconnects by itself
conf = configparser.ConfigParser(strict=False)
conf.read(os.path.join(ROOT, "rootfs/overlay/etc/bluetooth/main.conf"))
assert conf.get("Policy", "ReconnectAttempts").strip() == "0"
wp = open(os.path.join(ROOT, "rootfs/overlay/etc/wireplumber/wireplumber.conf.d/52-ztos-bluetooth.conf")).read()
lines = [ln.split("#", 1)[0] for ln in wp.splitlines()]
ac = [ln for ln in lines if "bluez5.auto-connect" in ln]
assert ac and all(ln.split("=", 1)[1].strip() == "[ ]" for ln in ac), ac

print("bt-reconnect: ok")
