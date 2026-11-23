#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests for ztos-soundd, the equalizer's engine (ROADMAP 8.67).

    python3 audio/tests/soundd-test.py
"""
import importlib.machinery
import importlib.util
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-soundd")
loader = importlib.machinery.SourceFileLoader("soundd", PATH)
spec = importlib.util.spec_from_loader("soundd", loader)
sd = importlib.util.module_from_spec(spec)
loader.exec_module(sd)

ABOX = "alsa_output.platform-snd-zrn-abox.stereo-fallback"
BT = "bluez_output.40_DA_5C_62_CA_1B.1"
USB = "alsa_output.usb-Apple__Inc._EarPods_GP2H5W3HFR-00.analog-stereo"


def node(i, **props):
    return {"id": i, "type": "PipeWire:Interface:Node", "info": {"props": props}}


# the phone's graph with the chain up: ztos-speaker (a filter chain) in front
# of the ABOX sink, and ztos-eq in front of ztos-speaker
DUMP = [
    node(40, **{"node.name": "ztos-speaker", "media.class": "Audio/Sink", "priority.session": 1009,
                "node.link-group": "filter-chain-1737-29", "object.serial": 40}),
    node(41, **{"node.name": "ztos-speaker.out", "media.class": "Stream/Output/Audio",
                "node.link-group": "filter-chain-1737-29", "target.object": ABOX}),
    node(72, **{"node.name": ABOX, "media.class": "Audio/Sink", "priority.session": 1000, "object.serial": 72}),
    node(91, **{"node.name": "ztos-eq", "media.class": "Audio/Sink", "node.link-group": "fc-2"}),
    node(98, **{"node.name": "ztos-eq.out", "media.class": "Stream/Output/Audio",
                "node.link-group": "fc-2", "target.object": "ztos-speaker"}),
    node(120, **{"node.name": BT, "media.class": "Audio/Sink", "priority.session": 1010,
                 "device.api": "bluez5"}),
    node(130, **{"node.name": USB, "media.class": "Audio/Sink", "priority.session": 1009,
                 "device.bus": "usb"}),
]


def test_sinks():
    s = {p["node.name"]: p for p in sd.read_sinks(DUMP)}
    # the raw speaker device is behind the speaker's protection: never a target
    assert ABOX not in s, s.keys()
    # and ztos-eq is never its own output (ztos-speaker stays: ztos-eq plays into it)
    assert "ztos-eq" not in s and "ztos-speaker" in s
    assert s["ztos-speaker"]["chain"] and not s[BT]["chain"] and not s[USB]["chain"]
    assert sd.classify(s[BT], {}) == "bluetooth"
    assert sd.classify(s[USB], {}) == "headphones"
    assert sd.classify(s["ztos-speaker"], {}) == "speaker"


def outs(*names):
    pr = {"ztos-speaker": 1009, BT: 1010, USB: 1009}
    return [{"name": n, "priority": pr[n]} for n in names]


def test_target():
    # asked for and there
    assert sd.choose_target(outs("ztos-speaker", BT), BT, []) == BT
    # asked-for output gone: back to the one chosen before it, not by priority
    assert sd.choose_target(outs("ztos-speaker", BT), USB, [USB, "ztos-speaker", BT]) == "ztos-speaker"
    assert sd.choose_target(outs("ztos-speaker", BT), USB, [USB, BT, "ztos-speaker"]) == BT
    # nothing remembered: the highest priority
    assert sd.choose_target(outs("ztos-speaker", BT), "auto", []) == BT
    assert sd.choose_target([], "auto", []) is None
    r = sd.remember([], "ztos-speaker")
    r = sd.remember(r, BT)
    r = sd.remember(r, "ztos-speaker")
    assert r == ["ztos-speaker", BT]
    assert len(sd.remember(["x%d" % i for i in range(20)], "y")) == sd.RECENT_MAX


def test_merge():
    cfg = sd.merge(sd.default_config(), {"recent": [BT, 3, "ztos-speaker"], "output": BT})
    assert cfg["recent"] == [BT, "ztos-speaker"] and cfg["output"] == BT
    assert sd.merge(sd.default_config(), {})["recent"] == []


def test_controls():
    cfg = sd.default_config()
    # in front of ztos-speaker (its own limiter after its volume) this limiter
    # stands aside; in front of Bluetooth or USB it holds -1 dBFS
    assert sd.controls(cfg, "speaker", 1.0, True)["lim:Bypass"] == 1.0
    c = sd.controls(cfg, "bluetooth", 1.0, False)
    assert c["lim:Bypass"] == 0.0 and abs(c["lim:Threshold (G)"] - 10 ** (-1 / 20)) < 1e-9
    # a band's gain reaches its control; flat at 0 is unity preamp
    assert c["preL:Mult"] == 1.0
    cfg["profiles"]["bluetooth"]["bands"][5]["g"] = -12.0
    c = sd.controls(cfg, "bluetooth", 1.0)
    assert c["eqL5:Gain"] == -12.0 and c["eqR5:Gain"] == -12.0
    assert c["preL:Mult"] == 1.0      # a cut needs no headroom
    # a boost takes its peak off the preamp (auto headroom)
    cfg["profiles"]["bluetooth"]["bands"][5]["g"] = 12.0
    c = sd.controls(cfg, "bluetooth", 1.0)
    assert abs(20 * math.log10(c["preL:Mult"]) + 12.0) < 0.3
    # off is the exact dry path
    c = sd.controls(cfg, "speaker", 0.0, True)
    assert c["oL:Gain 1"] == 1.0 and c["oL:Gain 2"] == 0.0


def test_band_response():
    # the RBJ peaking band at its centre is its gain (what eq-measure.py sees)
    b, a = sd._bq("peak", 1000.0, -12.0, 1.0)
    assert abs(sd._mag_db(b, a, 1000.0) + 12.0) < 0.01
    assert abs(sd._mag_db(b, a, 4000.0)) < 1.5


def test_graph():
    # every link joins nodes the graph has, the limiter is in it, and each
    # control names a node of the graph
    for mode in sd.SPATIAL:
        g = sd.Graph(mode)
        names = {n[0] for n in g.nodes}
        for a, b in g.links:
            assert a.split(":")[0] in names and b.split(":")[0] in names, (mode, a, b)
        assert "lim" in names
        cfg = sd.default_config()
        cfg["profiles"]["speaker"]["spatial"] = mode
        for k in sd.controls(cfg, "speaker", 1.0, True):
            assert k.split(":")[0] in names, (mode, k)
        text = sd.conf_text(g, sd.controls(cfg, "speaker", 1.0, True), "ztos-speaker", "ztOS Sound")
        assert '"Bypass" = 1.0' in text
        assert 'target.object = "ztos-speaker"' in text


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
    print("soundd: %d tests ok" % len(tests))


if __name__ == "__main__":
    sys.exit(main())
