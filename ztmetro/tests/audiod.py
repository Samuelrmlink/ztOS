#!/usr/bin/env python3
"""ztos-audiod's decisions, from pw-dump data, on the host: make check-audiod."""
import importlib.machinery
import importlib.util
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-audiod")
loader = importlib.machinery.SourceFileLoader("ztos_audiod", PATH)
spec = importlib.util.spec_from_loader("ztos_audiod", loader)
a = importlib.util.module_from_spec(spec)
loader.exec_module(a)


def node(i, props, state="idle", vols=None, mute=False):
    p = {"mute": mute}
    if vols is not None:
        p["channelVolumes"] = vols
    return {"id": i, "type": "PipeWire:Interface:Node",
            "info": {"state": state, "props": props, "params": {"Props": [p]}}}


def link(o, i):
    return {"id": 900 + o, "type": "PipeWire:Interface:Link", "info": {"output-node-id": o, "input-node-id": i}}


def meta(default, targets=()):
    m = [{"subject": 0, "key": "default.audio.sink", "type": "Spa:String:JSON", "value": {"name": default}}]
    m += [{"subject": s, "key": "target.object", "type": "Spa:Id", "value": v} for s, v in targets]
    return {"id": 1, "type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"}, "metadata": m}


ALSA = "alsa_output.platform-snd-zrn-abox.stereo-fallback"
BT = "bluez_output.00_0D_44_EB_50_54.1"


def world(default="ztos-speaker", targets=(), bt=True):
    d = [meta(default, targets),
         node(10, {"node.name": ALSA, "media.class": "Audio/Sink", "object.serial": 110, "node.description": "Built-in"}, vols=[0.064]),
         node(42, {"node.name": "ztos-speaker", "media.class": "Audio/Sink", "object.serial": 142,
                   "node.description": "Speaker", "node.link-group": "fc-1"}, vols=[0.512]),
         node(43, {"node.name": "ztos-speaker.out", "media.class": "Stream/Output/Audio", "object.serial": 143,
                   "node.link-group": "fc-1", "target.object": ALSA}),
         node(70, {"node.name": "pw-play", "media.class": "Stream/Output/Audio", "object.serial": 170,
                   "application.name": "Music", "media.name": "song"}, "running", vols=[0.125], mute=True),
         node(71, {"media.class": "Stream/Output/Audio", "object.serial": 171,
                   "media.role": "Notification", "application.name": "ztOS system sounds"}),
         link(43, 10), link(70, 42)]
    if bt:
        d.append(node(50, {"node.name": BT, "media.class": "Audio/Sink", "object.serial": 150,
                           "node.description": "UE MINI BOOM", "device.api": "bluez5"}, vols=[0.016]))
    return d


g = a.read_graph(world())
# the raw device behind the speaker's chain isn't offered; the system sounds aren't an app
assert sorted(g["sinks"]) == sorted(["ztos-speaker", BT]), g["sinks"]
assert [s["key"] for s in g["streams"]] == ["Output/Audio:application.name:Music"]
assert g["streams"][0]["percent"] == 50 and g["streams"][0]["mute"]
assert g["sinks"]["ztos-speaker"]["percent"] == 80
assert g["sinks"][BT]["kind"] == "bluetooth" and g["sinks"]["ztos-speaker"]["kind"] == "speaker"

# the active output, plain and through the EQ chain (hardware volume stays on Bluetooth)
assert a.real_output("ztos-speaker", g["sinks"], None) == "ztos-speaker"
assert a.real_output(a.EQ_SINK, g["sinks"], {"target": BT}) == BT
assert a.real_output(a.EQ_SINK, g["sinks"], None) is None
assert a.real_output("gone", g["sinks"], None) is None

# a stream's output, seen through a chain
ids = {s["id"]: s["name"] for s in g["sinks"].values()}
assert a.playing_on(70, g["links"], ids, g["chains"]) == "ztos-speaker"
assert a.playing_on(71, g["links"], ids, g["chains"]) is None

# WirePlumber's keys, and its state file
KEY = "Output/Audio:application.name:Music"
assert a.form_key({"application.id": "a", "application.name": "b"}) == "Output/Audio:application.id:a"
assert a.form_key({"media.role": "Notification", "application.name": "b"}) == "Output/Audio:media.role:Notification"
assert a.form_key({"node.name": "n"}) == "Output/Audio:node.name:n"
assert a.form_key({}) is None
assert a.wp_unescape(r"a\sb\ec\od\ce\\f") == "a b=c[d]e\\f"
st = a.parse_stream_state(
    "[stream-properties]\n"
    r'Output/Audio:application.name:My\sPlayer={"channelVolumes":[0.125, 0.125], "mute":true, "target":"t"}' "\n"
    'Output/Audio:media.name:Speaker={"mute":false, "channelVolumes":[1.0, 1.0]}\n'
    'Input/Audio:application.name:rec={"mute":false}\n'
    "Output/Audio:application.name:broken={nope\n")
assert sorted(st) == ["Output/Audio:application.name:My Player", "Output/Audio:media.name:Speaker"]
assert st["Output/Audio:application.name:My Player"] == {"percent": 50, "mute": True, "target": "t"}

# routing: nothing asked, nothing done, whatever another tool did
sinks = g["sinks"]
streams = g["streams"]
assert a.target_actions(streams, {}, {}, {}, sinks) == []
assert a.target_actions(streams, {70: {"target.object": 150}}, {}, {}, sinks) == []
# a held choice is enforced, once
held = {KEY: {"output": BT}}
assert a.target_actions(streams, {}, {}, held, sinks) == [("set", 70, "150")]
assert a.target_actions(streams, {70: {"target.object": 150}}, {}, held, sinks) == []
assert a.target_actions(streams, {70: {"target.object": "150"}}, {}, held, sinks) == []
# ... not while that output is away, and it wins over what is saved
away = dict(sinks)
del away[BT]
assert a.target_actions(streams, {}, {}, held, away) == []
assert a.target_actions(streams, {70: {"target.object": 142}}, {KEY: {"target": "ztos-speaker"}}, held, sinks) == [("set", 70, "150")]
# back to Default
assert a.target_actions(streams, {70: {"target.object": 150}}, {}, {KEY: {"output": "default"}}, sinks) == [("clear", 70)]
assert a.target_actions(streams, {}, {KEY: {"target": BT}}, {KEY: {"output": "default"}}, sinks) == []
# a saved target returns when its output comes back with a new serial
saved = {KEY: {"target": BT}}
assert a.target_actions(streams, {}, saved, {}, sinks) == [("set", 70, "150")]
assert a.target_actions(streams, {70: {"target.object": 99}}, saved, {}, sinks) == [("set", 70, "150")]
assert a.target_actions(streams, {70: {"target.object": 150}}, saved, {}, sinks) == []
# but a stream moved elsewhere by hand stays where it was moved
assert a.target_actions(streams, {70: {"target.object": 142}}, saved, {}, sinks) == []
assert a.target_actions(streams, {70: {"target.node": 42}}, saved, {}, sinks) == []

# what the UI gets
apps = a.build_apps(streams, {"Output/Audio:application.id:old": {"percent": 30, "mute": False, "target": BT},
                              "Output/Audio:media.name:Speaker": {"percent": 100, "mute": False, "target": None}},
                    {"Output/Audio:application.name:Later": {"percent": 20}}, away, {70: "ztos-speaker"})
assert [x["name"] for x in apps] == ["Music", "Later", "old"], apps
assert apps[0]["running"] and apps[0]["streams"] == [70] and apps[0]["output"] == "default"
assert apps[0]["output_present"] and apps[0]["playing_on"] == "ztos-speaker" and apps[0]["percent"] == 50
assert not apps[1]["running"] and apps[1]["percent"] == 20 and apps[1]["output_present"]
assert apps[2]["output"] == BT and not apps[2]["output_present"]

# naming: what real apps report (captured on the phone, in a private PipeWire)
BRAVE = {"application.name": "Brave", "application.process.binary": "brave", "application.icon-name": "brave-browser",
         "media.class": "Stream/Output/Audio", "media.name": "Playback", "node.name": "Brave", "node.latency": "512/48000"}
GRAYJAY = {"application.name": "Chromium", "application.process.binary": "justcefnative",
           "application.icon-name": "chromium-browser", "media.class": "Stream/Output/Audio", "media.name": "Playback",
           "node.name": "Chromium", "node.latency": "512/48000", "pipewire.access.portal.app_id": "app.grayjay.Grayjay",
           "pipewire.client.access": "flatpak"}
assert a.form_key(BRAVE) == "Output/Audio:application.name:Brave"
assert a.form_key(GRAYJAY) == "Output/Audio:application.name:Chromium"
assert a.is_app_stream(BRAVE) and a.is_app_stream(GRAYJAY)
assert a.app_title(BRAVE) == "Brave"
assert a.app_title(GRAYJAY, lambda i: i) == "app.grayjay.Grayjay"
assert a.app_title({"application.name": "Chromium", "application.process.binary": "justcefnative"}) == "justcefnative"
assert a.app_title({"application.name": "Chromium", "application.process.binary": "chromium"}) == "Chromium"
assert a.app_title({"application.name": "ALSA plug-in [foo]", "application.process.binary": "foo"}) == "foo"
assert a.app_title({"application.name": "Music"}) == "Music"
assert a.app_title({"application.process.binary": "foo"}) == "foo"
assert a.app_title({}) is None
assert a.desktop_entry_name("[Other]\nName=No\n[Desktop Entry]\nName[de]=Nein\nName=Grayjay\n[Action]\nName=x\n") == "Grayjay"
assert a.desktop_entry_name("[Action]\nName=x\n") is None
assert a.flatpak_title("no.such.App", ()) == "App"
assert a.flatpak_title("../../etc/passwd", ("/",)) == "passwd"

KG = "Output/Audio:application.name:Chromium"
def sg(i, name): return {"id": i, "key": KG, "name": name, "percent": 100, "mute": False}
# the name is remembered, so the app is still itself once it stops
names = {}
ap = a.build_apps([sg(1, "Grayjay")], {}, {}, sinks, {})
assert ap[0]["name"] == "Grayjay" and ap[0]["key"] == KG
assert a.remember_names(names, ap) and names == {KG: "Grayjay"}
assert not a.remember_names(names, ap)
ap = a.build_apps([], {KG: {"percent": 40, "mute": False, "target": None}}, {}, sinks, {}, names)
assert ap[0]["name"] == "Grayjay" and not ap[0]["running"] and ap[0]["percent"] == 40
assert not a.remember_names(names, ap)
# an app whose key says it needs nothing remembered, and gets over a stale name
assert a.remember_names(names, a.build_apps([dict(sg(1, "Chromium"))], {}, {}, sinks, {})) and names == {}
# two apps under one key share WirePlumber's one setting: the row says so
ap = a.build_apps([sg(1, "Grayjay"), sg(2, "Other"), sg(3, "Grayjay")], {}, {}, sinks, {})
assert len(ap) == 1 and ap[0]["name"] == "Grayjay + Other" and ap[0]["streams"] == [1, 2, 3]
# sound tools aren't kept in the list once they stop, unless something is held for them
TK = "Output/Audio:application.name:pw-play"
quiet = {"percent": 100, "mute": False, "target": None}
assert a.build_apps([], {TK: quiet}, {}, sinks, {}) == []
assert [x["key"] for x in a.build_apps([], {TK: quiet}, {TK: {"percent": 5}}, sinks, {})] == [TK]
# an output's name for people, there or away
ap = a.build_apps(streams, {KEY: {"percent": 50, "mute": False, "target": BT}}, {}, sinks, {})
assert ap[0]["output_desc"] == "UE MINI BOOM" and ap[0]["output_present"]
ap = a.build_apps(streams, {KEY: {"percent": 50, "mute": False, "target": BT}}, {}, away, {}, {}, {BT: "UE MINI BOOM"})
assert ap[0]["output_desc"] == "UE MINI BOOM" and not ap[0]["output_present"]
ap = a.build_apps(streams, {KEY: {"percent": 50, "mute": False, "target": BT}}, {}, away, {})
assert ap[0]["output_desc"] == BT
assert "output_desc" not in a.build_apps(streams, {}, {}, sinks, {})[0]

# what the Sound app sends (tests/audio-apps.c) is what the daemon reads
assert a.shlex.split("app-volume 'Output/Audio:application.name:Bob'\\''s Player' '5'") == \
    ["app-volume", "Output/Audio:application.name:Bob's Player", "5"]

# small parsers
assert a.parse_wpctl_volume("Volume: 0.40\n") == (40, False)
assert a.parse_wpctl_volume("Volume: 0.25 [MUTED]\n") == (25, True)
assert a.parse_wpctl_volume("") is None
assert a.clamp_percent("140") == 100 and a.clamp_percent("-1") == 0 and a.clamp_percent("33.4") == 33
for bad in ("nan", "x"):
    try:
        a.clamp_percent(bad)
        assert False
    except ValueError:
        pass
assert a.outputs_file_text([{"current": True, "name": "n", "desc": "D"}, {"current": False, "name": "m", "desc": "E"}]) == "*|n|D\n-|m|E\n"

# the command parser's refusals reach the sender as text
d = a.Daemon.__new__(a.Daemon)
for bad in ("", "nonsense", "volume", "volume 1 2 3", "output", "app-volume x", "app-volume Input/Audio:x:y 3"):
    try:
        d.graph = {"streams": [], "sinks": {}}
        d.active = None
        d.command(bad)
        assert False, bad
    except ValueError:
        pass


# a Bluetooth output that connects takes the audio, only if it is the first such
SP = {"kind": "speaker"}
def BTS(n): return {"name": n, "kind": "bluetooth"}
assert a.takeover_target(None, {"sp": SP, "b1": BTS("b1")}) is None          # first look
assert a.takeover_target({"sp"}, {"sp": SP}) is None                         # nothing new
assert a.takeover_target({"sp"}, {"sp": SP, "b1": BTS("b1")}) == "b1"        # takes over
assert a.takeover_target({"sp", "b1"}, {"sp": SP, "b1": BTS("b1"), "b2": BTS("b2")}) is None  # another BT
assert a.takeover_target({"sp", "u"}, {"sp": SP, "u": {"name": "u", "kind": "headphones", "usb": True}, "b1": BTS("b1")}) is None  # USB there
assert a.takeover_target({"sp", "h"}, {"sp": SP, "h": {"name": "h", "kind": "headphones"}, "b1": BTS("b1")}) == "b1"  # a plain jack doesn't count
assert a.takeover_target({"old"}, {"sp": SP, "b1": BTS("b1")}) is None       # all new: a restart
assert a.takeover_target({"sp"}, {"sp": SP, "b1": BTS("b1"), "b2": BTS("b2")}) is None  # two at once: ask
# headphones plugged into the jack (ztos-headsetd's sink) take the audio as Bluetooth does
JK = {"name": a.JACK_SINK, "kind": "headphones"}
assert a.takeover_target({"sp"}, {"sp": SP, a.JACK_SINK: JK}) == a.JACK_SINK
assert a.takeover_target({"sp", "b1"}, {"sp": SP, "b1": BTS("b1"), a.JACK_SINK: JK}) is None  # AirPods on: the user's choice
assert a.takeover_target({"sp", a.JACK_SINK}, {"sp": SP, a.JACK_SINK: JK}) is None        # already there

print("audiod: ok")
