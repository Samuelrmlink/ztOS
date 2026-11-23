#!/usr/bin/env python3
"""ztos-headsetd's decisions on the host: python3 -I audio/tests/headsetd-test.py"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "../../devices/samsung-starlte/overlay/usr/local/bin/ztos-headsetd")
loader = importlib.machinery.SourceFileLoader("ztos_headsetd", PATH)
spec = importlib.util.spec_from_loader("ztos_headsetd", loader)
h = importlib.util.module_from_spec(spec)
loader.exec_module(h)

# the switch bitmap EVIOCGSW returns
bits = bytearray(8)
assert h.switches_from_bits(bits) == (False, False, False)
bits[0] = (1 << h.SW_HEADPHONE_INSERT) | (1 << h.SW_JACK_PHYSICAL_INSERT)
assert h.switches_from_bits(bits) == (True, False, True)
bits[0] |= 1 << h.SW_MICROPHONE_INSERT
assert h.switches_from_bits(bits) == (True, True, True)
assert h.switches_from_bits(b"") == (False, False, False)
# EVIOCGSW(8) as the kernel defines it: _IOC(_IOC_READ, 'E', 0x1b, 8)
assert h.eviocgsw(8) == 0x8008451b

# the device is found by its name in sysfs
with tempfile.TemporaryDirectory() as d:
    for n, name in ((0, "gpio-keys"), (3, "S9 Headset Jack"), (7, "s6sy761")):
        os.makedirs(os.path.join(d, "event%d/device" % n))
        with open(os.path.join(d, "event%d/device/name" % n), "w") as f:
            f.write(name + "\n")
    assert h.find_device(d) == "/dev/input/event3"
    os.unlink(os.path.join(d, "event3/device/name"))
    assert h.find_device(d) is None

# a headphone output only while the driver plays to the jack
with tempfile.TemporaryDirectory() as d:
    assert not h.driver_routes_jack(d)           # no driver
    with open(os.path.join(d, "hp_route"), "w") as f:
        f.write("N\n")
    assert not h.driver_routes_jack(d)
    with open(os.path.join(d, "hp_route"), "w") as f:
        f.write("Y\n")
    assert h.driver_routes_jack(d)

# WirePlumber's saved volume: only the first time is the volume set
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "stream-properties")
    assert not h.known_volume(p)
    with open(p, "w") as f:
        f.write("[stream-properties]\nAudio/Sink:media.name:Speaker={\"volume\": 0.5}\n")
    assert not h.known_volume(p)
    with open(p, "a") as f:
        f.write("Audio/Sink:media.name:Headphones={\"volume\": 0.3}\n")
    assert h.known_volume(p)
assert h.FIRST_PERCENT <= 50

# the loopback: a headphone sink over the speaker chain's priority, into the card
argv = h.loopback_argv()
assert argv[0] == "pw-loopback"
cap = json.loads(argv[argv.index("-i") + 1])
play = json.loads(argv[argv.index("-o") + 1])
assert cap["media.class"] == "Audio/Sink" and cap["node.name"] == h.SINK
assert cap["device.form-factor"] == "headphone"   # ztos-audiod: kind "headphones", gain capped at 1
assert cap["priority.session"] > 1009             # the speaker chain's
assert play["target.object"] == h.CARD_SINK and play["node.passive"] is True

# the jack's sink name is the one ztos-audiod takes over to
AUDIOD = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-audiod")
with open(AUDIOD, encoding="utf-8") as f:
    assert 'JACK_SINK = "%s"' % h.SINK in f.read()

print("headsetd: ok")
