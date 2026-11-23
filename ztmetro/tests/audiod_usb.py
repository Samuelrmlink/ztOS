#!/usr/bin/env python3
"""ztos-audiod and USB audio (ROADMAP 8.62), on the host: make check-audiod."""
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


SP = {"kind": "speaker"}
def BTS(n): return {"name": n, "kind": "bluetooth"}


# USB audio (8.62): Apple's USB-C EarPods (05ac:110b), props from the phone's pw-dump
USB_OUT = "alsa_output.usb-Apple__Inc._EarPods_GP2H5W3HFR-00.analog-stereo"
USB_MIC = "alsa_input.usb-Apple__Inc._EarPods_GP2H5W3HFR-00.mono-fallback"


def usb_world(source="s9-microphone-agc", desc_props=None, usb=True):
    m = meta("ztos-speaker")
    m["metadata"].append({"subject": 0, "key": "default.audio.source", "type": "Spa:String:JSON",
                          "value": {"name": source}})
    w = [x for x in world(bt=False) if x.get("type") != "PipeWire:Interface:Metadata"] + [m]
    w.append(node(33, {"node.name": "s9-microphone", "media.class": "Audio/Source", "node.description": "Microphone"}))
    if not usb:
        return w
    props = {"node.name": USB_OUT, "media.class": "Audio/Sink", "object.serial": 173, "device.id": 58,
             "node.description": "EarPods Analog Stereo", "node.nick": "EarPods", "device.bus": "usb",
             "alsa.card_name": "EarPods", "alsa.name": "USB Audio", "priority.session": 1109}  # as pw-dump shows them
    props.update(desc_props or {})
    w.append(node(73, {k: v for k, v in props.items() if v is not None}, vols=[0.064]))
    w.append(node(74, {"node.name": USB_MIC, "media.class": "Audio/Source", "object.serial": 174, "device.id": 58,
                       "node.description": "EarPods Mono", "device.bus": "usb"}))
    return w


gu = a.read_graph(usb_world())
u = gu["sinks"][USB_OUT]
assert u["kind"] == "headphones" and u["usb"] and not u["hdmi"], u
assert u["desc"] == "EarPods", u                      # the product, not "EarPods Analog Stereo"
assert gu["default_source"] == "s9-microphone-agc"
assert list(gu["sources"]) == [USB_MIC]               # the phone's own microphone isn't USB
assert a.mic_of(u, gu["sources"]) == USB_MIC
assert a.mic_of(gu["sinks"]["ztos-speaker"], gu["sources"]) is None
outs = {o["name"]: o for o in a.output_entries(gu["sinks"], "ztos-speaker", set(), gu["sources"])}
assert outs[USB_OUT]["mic"] == USB_MIC and outs[USB_OUT]["desc"] == "EarPods"
assert "mic" not in outs["ztos-speaker"]
assert set(outs[USB_OUT]) == {"name", "desc", "kind", "percent", "mute", "current", "playing", "mic"}
# without sources (an older caller) the entries are as before: backwards compatible
assert all("mic" not in o for o in a.output_entries(gu["sinks"], "ztos-speaker", set()))
# a generic dongle has no useful product name: "USB headphones"
for generic in ({"alsa.card_name": "USB Audio Device", "node.nick": "USB Audio Device"},
                {"alsa.card_name": None, "node.nick": None}):
    gg = a.read_graph(usb_world(desc_props=generic))
    assert gg["sinks"][USB_OUT]["desc"] == a.USB_DESC, gg["sinks"][USB_OUT]
assert a.usb_desc({"device.product.name": "Pixel USB-C earbuds"}) == "Pixel USB-C earbuds"
assert a.usb_desc({"alsa.card_name": "USB2.0 Device"}) == a.USB_DESC

# USB headphones plugged in take the audio, as the jack does; not over Bluetooth or another USB
US = {"name": USB_OUT, "kind": "headphones", "usb": True}
assert a.takeover_target({"sp"}, {"sp": SP, USB_OUT: US}) == USB_OUT
assert a.takeover_target({"sp", USB_OUT}, {"sp": SP, USB_OUT: US}) is None                 # already there
assert a.takeover_target({"sp", "b1"}, {"sp": SP, "b1": BTS("b1"), USB_OUT: US}) is None   # AirPods on
assert a.takeover_target({"sp", "u2"}, {"sp": SP, "u2": dict(US, name="u2"), USB_OUT: US}) is None
assert a.takeover_target({"sp"}, {"sp": SP, "dock": {"name": "dock", "kind": "headphones", "usb": True, "hdmi": True}}) is None
assert a.takeover_target(None, {"sp": SP, USB_OUT: US}) is None                           # first look


# the daemon itself: plugged in, the output and the microphone move; unplugged, it
# forces nothing and WirePlumber goes back to the ones before
class FakeDaemon(a.Daemon):
    def __init__(self, dumps):
        super().__init__()
        self.dumps, self.calls = list(dumps), []

    def tool(self, *argv, out=False):
        self.calls.append(argv)
        if argv[0] == "pw-dump":
            return json.dumps(self.dumps.pop(0))
        if argv[:2] == ("wpctl", "get-volume"):
            return "Volume: 0.40"
        return "" if out else True

    def publish(self, g):
        pass

    def apply_roles(self, g):
        pass

    def apply_intents(self, g):
        pass


import json
import tempfile
a.CONFIG = os.path.join(tempfile.mkdtemp(), "audio-apps.json")
a.BOOST_FILE = os.path.join(tempfile.mkdtemp(), "boost.json")
a.WP_STREAMS = os.path.join(tempfile.mkdtemp(), "stream-properties")
fd = FakeDaemon([usb_world(usb=False), usb_world(), usb_world(usb=False)])
defaults = lambda: [c for c in fd.calls if c[:2] == ("wpctl", "set-default")]
fd.evaluate()
assert not defaults(), fd.calls
fd.calls.clear()
fd.evaluate()                                                     # plugged in
assert defaults() == [("wpctl", "set-default", "73"), ("wpctl", "set-default", "74")], fd.calls
fd.calls.clear()
fd.evaluate()                                                     # unplugged
assert not defaults(), fd.calls
# in at start (no takeover on the first look), then chosen in the shell: its mic comes too
fd = FakeDaemon([usb_world(), usb_world()])
fd.evaluate()
assert not defaults(), fd.calls
fd.cmd_output(USB_OUT)
assert defaults() == [("wpctl", "set-default", "73"), ("wpctl", "set-default", "74")], fd.calls
fd.calls.clear()
fd.cmd_output("ztos-speaker")                                     # back to the speaker: the input stays
assert defaults() == [("wpctl", "set-default", "42")], fd.calls
# a USB mic that is the input already is left alone
fd = FakeDaemon([usb_world(source=USB_MIC)])
fd.evaluate()
fd.cmd_output(USB_OUT)
assert defaults() == [("wpctl", "set-default", "73")], fd.calls

print("audiod usb: ok")
