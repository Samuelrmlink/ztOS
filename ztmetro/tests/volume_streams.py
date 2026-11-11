#!/usr/bin/env python3
"""ztos-audiod's side of the three volumes (volume-streams.h): the ring's and
the alarm's gains, the output turned up and back, and the levels read from
their files. On the host, no PipeWire: make check-volume-streams."""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-audiod")
loader = importlib.machinery.SourceFileLoader("ztos_audiod", PATH)
spec = importlib.util.spec_from_loader("ztos_audiod", loader)
a = importlib.util.module_from_spec(spec)
loader.exec_module(a)


def close(x, y, tol=1e-6):
    return abs(x - y) <= tol * max(1.0, abs(y))


SPEAKER = {"name": "ztos-speaker", "percent": 30, "mute": False, "kind": "speaker"}
BT = {"name": "bluez_output.X.1", "percent": 30, "mute": False, "kind": "bluetooth"}

# ---- levels, from where they are kept
lv = a.parse_levels("accent = red\nring_volume = 57\nmedia_volume = 30\n", "volume 35\nautostop 1\n")
assert lv == {"ring": 57, "alarm": 35}, lv
assert a.parse_levels("", "") == {"ring": a.RING_DEFAULT, "alarm": a.ALARM_DEFAULT}
# the alarm never under its floor, whatever the file says; the ring may be 0
assert a.parse_levels("ring_volume = 0", "volume 0")["alarm"] == a.ALARM_MIN
assert a.parse_levels("ring_volume = 0", "volume 5")["ring"] == 0
assert a.parse_levels("ring_volume = 250", "volume 999") == {"ring": 100, "alarm": 100}
assert a.parse_levels("ring_volume = x", "volume y") == {"ring": a.RING_DEFAULT, "alarm": a.ALARM_DEFAULT}

# ---- a ring's gain is its level over the output's: the same loudness at any Media
for media in (100, 60, 30, 20, 13):
    dev = dict(SPEAKER, percent=media)
    g = a.stream_gain(57, dev)
    assert close(g * a.cube(media), a.cube(57)), (media, g)
# Media louder than the ring: the stream turns itself down
assert a.stream_gain(20, dict(SPEAKER, percent=80)) < 1.0
# too low an output: the stream stops at its most, and only a boost helps
assert a.stream_gain(57, dict(SPEAKER, percent=5)) == a.GAIN_MAX
assert a.needs_boost(57, dict(SPEAKER, percent=5))
assert not a.needs_boost(57, SPEAKER)
# muted or at 0: as loud as a stream can be, and the output must come up
assert a.stream_gain(57, dict(SPEAKER, mute=True)) == a.GAIN_MAX
assert a.needs_boost(20, dict(SPEAKER, percent=0))
# Bluetooth: the device's volume comes after the encoder, so never above 1
assert a.stream_gain(57, BT) == 1.0 and a.needs_boost(57, BT)
assert close(a.stream_gain(20, BT), a.cube(20) / a.cube(30))
# nothing known about the output: the level alone, as before
assert close(a.stream_gain(57, None), a.cube(57))
assert a.stream_gain(0, SPEAKER) == 0.0
# the boost is the least that carries the level
for dev in (dict(SPEAKER, percent=5), dict(BT)):
    p = a.boost_percent(57, dev)
    assert not a.needs_boost(57, dict(dev, percent=p)), (dev, p)
    assert a.needs_boost(57, dict(dev, percent=p - 2)), (dev, p)
assert a.boost_percent(57, BT) == 57

# ---- the streams, from pw-dump
def node(i, props, master=1.0, channel=1.0):
    return {"id": i, "type": "PipeWire:Interface:Node",
            "info": {"state": "running", "props": props,
                     "params": {"Props": [{"volume": master, "mute": False, "channelVolumes": [channel, channel]}]}}}


dump = [
    node(10, {"node.name": "ztos-speaker", "media.class": "Audio/Sink"}),
    node(20, {"application.name": "pw-play", "media.class": "Stream/Output/Audio", "media.role": "Alarm",
              "ztos.stream": "alarm", "ztos.volume.share": "0.300", "ztos.volume.boost": True}, master=0.5),
    node(21, {"application.name": "ztOS system sounds", "media.class": "Stream/Output/Audio",
              "media.role": "Notification", "ztos.stream": "ring"}, channel=0.4),
    node(22, {"application.name": "mpv", "media.class": "Stream/Output/Audio", "media.role": "Music"}),
]
g = a.read_graph(dump)
roles = {r["id"]: r for r in g["roles"]}
assert set(roles) == {20, 21}, roles
assert roles[20]["stream"] == "alarm" and close(roles[20]["share"], 0.3) and roles[20]["boost"] and roles[20]["master"] == 0.5
assert roles[21]["stream"] == "ring" and not roles[21]["boost"] and roles[21]["share"] == 1.0 and roles[21]["channel"] == 0.4
# ring and alarm sounds are not apps
assert [s["key"] for s in g["streams"]] == ["Output/Audio:application.name:mpv"], g["streams"]

levels = {"ring": 57, "alarm": 40}
acts = dict(a.role_actions(g["roles"], levels, SPEAKER))
# the alarm: master 0.5 already, so the channel takes the rest of level*share/output
assert close(acts[20] * 0.5, a.stream_gain(40, SPEAKER) * 0.3), acts
assert close(acts[21], a.stream_gain(57, SPEAKER)), acts
# right already: nothing to do (the program started it at its level)
right = [dict(roles[21], master=a.stream_gain(57, SPEAKER), channel=1.0)]
assert a.role_actions(right, levels, SPEAKER) == []
# a stream that can't come all the way is set once to its most, then left alone
low = [dict(roles[21], master=0.01, channel=1.0)]
acts3 = a.role_actions(low, levels, dict(SPEAKER, percent=5))
assert acts3 == [(21, a.VOLUME_MAX)], acts3
assert a.role_actions([dict(low[0], channel=a.VOLUME_MAX)], levels, dict(SPEAKER, percent=5)) == []
# Media moved: the ring stream follows, the other way
acts2 = dict(a.role_actions(right, levels, dict(SPEAKER, percent=60)))
assert close(acts2[21] * right[0]["master"] * a.cube(60), a.cube(57)), acts2


# ---- the daemon: levels from files, the boost and its undoing
class Fake(a.Daemon):
    def __init__(self):
        self.calls = []
        super().__init__()

    def tool(self, *argv, out=False):
        self.calls.append(argv)
        return "" if out else True

    def sound_state(self):
        return None


with tempfile.TemporaryDirectory() as tmp:
    a.RING_CONFIG = os.path.join(tmp, "config")
    a.ALARM_CONFIG = os.path.join(tmp, "settings")
    a.BOOST_FILE = os.path.join(tmp, "boost.json")
    a.STATE = os.path.join(tmp, "state.json")
    a.OUTPUTS = os.path.join(tmp, "outputs")
    a.CONFIG = os.path.join(tmp, "apps.json")
    a.SHELL_CTL = os.path.join(tmp, "nope")
    a.WP_STREAMS = os.path.join(tmp, "wp")
    with open(a.RING_CONFIG, "w") as f:
        f.write("ring_volume = 57\n")
    with open(a.ALARM_CONFIG, "w") as f:
        f.write("volume 50\n")
    d = Fake()
    d.read_levels()
    assert d.levels == {"ring": 57, "alarm": 50}
    # a change on disk is seen; the same file isn't read twice
    with open(a.ALARM_CONFIG, "w") as f:
        f.write("volume 10\nautostop 1\n")
    os.utime(a.ALARM_CONFIG, ns=(1, 1))
    d.read_levels()
    assert d.levels["alarm"] == a.ALARM_MIN, d.levels

    sink = {"id": 10, "serial": 110, "name": "ztos-speaker", "desc": "Speaker", "kind": "speaker", "usb": False,
            "percent": 2, "mute": False}
    graph = {"sinks": {"ztos-speaker": sink}, "default": "ztos-speaker", "streams": [], "targets": {}, "links": [],
             "chains": {}, "roles": [roles[20]]}
    d.publish(graph)
    d.calls = []
    d.apply_roles(graph)
    # an alarm at 20 % through an output at 2 %: turned up, and remembered
    ups = [c for c in d.calls if c[:3] == ("wpctl", "set-volume", "10")]
    assert ups and ups[0][3] == "%d%%" % a.boost_percent(a.ALARM_MIN, dict(sink)), d.calls
    assert json.load(open(a.BOOST_FILE))["percent"] == 2
    st = json.load(open(a.STATE))
    assert st["streams"]["boost"] and st["streams"]["alarm"] == {"percent": a.ALARM_MIN, "playing": 1}, st
    # a restarted daemon finds it
    assert Fake().boost["percent"] == 2
    # the alarm stops: back after BOOST_HOLD, not before
    graph["roles"] = []
    sink["percent"] = ups[0][3]
    d.calls = []
    d.apply_roles(graph)
    assert not [c for c in d.calls if c[:3] == ("wpctl", "set-volume", "10")], d.calls
    d.boost_seen -= a.BOOST_HOLD + 1
    d.apply_roles(graph)
    assert ("wpctl", "set-volume", "10", "2%") in d.calls, d.calls
    assert d.boost is None and not os.path.exists(a.BOOST_FILE)
    # Media set by hand during a boost: that level stays
    d.boost = {"name": "ztos-speaker", "percent": 2, "mute": False}
    d.drop_boost()
    assert d.boost is None

print("volume-streams (audiod): ok")
