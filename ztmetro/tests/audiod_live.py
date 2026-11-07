#!/usr/bin/env python3
"""ztos-audiod against a real PipeWire and WirePlumber: make check-audiod-live.

Everything runs in a private instance: its own runtime, state and config
directories, null sinks only, no D-Bus, and WirePlumber's ALSA and Bluetooth
monitors off. It can't reach a live session or a sound card, so no sound comes
out and it's safe on a phone somebody is using. The streams send silence.

The "Bluetooth" output here is a null sink with a bluez name, which is all the
daemon looks at.
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wave

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-audiod")

SPEAKER = "ztos-speaker"
BT = "bluez_output.AA_BB_CC_DD_EE_FF.1"
BT2 = "bluez_output.11_22_33_44_55_66.1"

PIPEWIRE_CONF = """
context.properties = {
    core.daemon = true
    core.name = pipewire-0
    default.clock.rate = 48000
    default.clock.quantum = 1024
    default.clock.min-quantum = 32
    default.clock.max-quantum = 2048
    link.max-buffers = 16
    settings.check-quantum = false
    settings.check-rate = false
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-client-device }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-session-manager }
]
context.objects = [
    { factory = spa-node-factory
      args = { factory.name = support.node.driver node.name = Dummy-Driver
               node.group = pipewire.dummy priority.driver = 20000 } }
    { factory = adapter
      args = { factory.name = support.null-audio-sink node.name = ztos-speaker
               node.description = Speaker media.class = Audio/Sink
               audio.position = [ FL FR ] priority.session = 1000 } }
]
"""

WIREPLUMBER_CONF = """
wireplumber.profiles = {
    main = {
        hardware.audio = disabled
        hardware.bluetooth = disabled
        hardware.video-capture = disabled
        monitor.alsa = disabled
        monitor.alsa-midi = disabled
        monitor.bluez = disabled
        monitor.bluez-midi = disabled
        monitor.libcamera = disabled
        monitor.v4l2 = disabled
        support.dbus = disabled
        support.reserve-device = disabled
        support.portal-permissionstore = disabled
        support.logind = disabled
    }
}
"""


class Private:
    """A PipeWire, WirePlumber and pipewire-pulse nobody else can see."""

    def __init__(self, keep=False):
        self.dir = tempfile.mkdtemp(prefix="ztos-audio-test-", dir=os.environ.get("ZTOS_AUDIO_TEST_DIR"))
        self.keep = keep
        self.run = os.path.join(self.dir, "run")
        os.makedirs(self.run, mode=0o700)
        os.makedirs(os.path.join(self.dir, "config/wireplumber/wireplumber.conf.d"))
        os.makedirs(os.path.join(self.dir, "state"))
        self.conf = os.path.join(self.dir, "pipewire-private.conf")
        with open(self.conf, "w") as f:
            f.write(PIPEWIRE_CONF)
        with open(os.path.join(self.dir, "config/wireplumber/wireplumber.conf.d/99-private.conf"), "w") as f:
            f.write(WIREPLUMBER_CONF)
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("PIPEWIRE_", "PULSE_", "WAYLAND_", "DISPLAY", "XAUTHORITY", "DBUS_"))}
        self.env.update({
            "HOME": self.dir, "XDG_RUNTIME_DIR": self.run, "PIPEWIRE_RUNTIME_DIR": self.run,
            "PULSE_RUNTIME_PATH": os.path.join(self.run, "pulse"),
            "XDG_CONFIG_HOME": os.path.join(self.dir, "config"),
            "XDG_STATE_HOME": os.path.join(self.dir, "state"),
            "XDG_DATA_HOME": os.path.join(self.dir, "data"),
            "XDG_CACHE_HOME": os.path.join(self.dir, "cache"),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent/ztos-audio-test",
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent/ztos-audio-test",
            "PIPEWIRE_LOG_SYSTEMD": "false",
        })
        self.procs = {}
        self.wav = os.path.join(self.dir, "silence.wav")
        with wave.open(self.wav, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(b"\0" * (48000 * 4 * 300))

    # ---- processes
    def spawn(self, name, *argv, env=None):
        log = open(os.path.join(self.dir, name.replace("/", "_") + ".log"), "ab")
        self.procs[name] = subprocess.Popen(argv, env=dict(self.env, **(env or {})), stdin=subprocess.DEVNULL,
                                            stdout=log, stderr=log)
        log.close()
        return self.procs[name]

    def stop(self, name):
        p = self.procs.pop(name, None)
        if p is None:
            return
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()

    def start_audio(self):
        self.spawn("pipewire", "pipewire", "-c", self.conf)
        wait_for(lambda: os.path.exists(os.path.join(self.run, "pipewire-0")), "the private PipeWire")
        self.spawn("wireplumber", "wireplumber")
        self.spawn("pulse", "pipewire-pulse")
        wait_for(lambda: self.tool("pactl", "info") is not None, "pipewire-pulse")
        wait_for(lambda: self.default_sink() is not None, "a default sink")

    def stop_audio(self):
        for n in ("pulse", "wireplumber", "pipewire"):
            self.stop(n)

    def start_daemon(self):
        self.spawn("audiod", sys.executable, "-I", DAEMON)
        wait_for(lambda: os.path.exists(os.path.join(self.run, "ztos-audio.sock")), "ztos-audiod")

    def close(self):
        for n in list(self.procs):
            self.stop(n)
        if not self.keep:
            shutil.rmtree(self.dir, ignore_errors=True)

    # ---- tools, all inside
    def tool(self, *argv):
        try:
            r = subprocess.run(argv, env=self.env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            return None
        return r.stdout if r.returncode == 0 else None

    def dump(self):
        text = self.tool("pw-dump")
        return json.loads(text) if text else []

    def ctl(self, *words):
        r = subprocess.run([sys.executable, "-I", DAEMON, "ctl", *words], env=self.env,
                           capture_output=True, text=True, timeout=15)
        assert r.stdout.strip() == "ok", (words, r.stdout, r.stderr)

    def state(self):
        try:
            with open(os.path.join(self.run, "ztos-audio-state.json")) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def app(self, key):
        return next((a for a in self.state().get("apps", []) if a["key"] == key), None)

    def default_sink(self):
        for o in self.dump():
            if o.get("type", "").endswith(":Metadata") and o.get("props", {}).get("metadata.name") == "default":
                for m in o.get("metadata") or []:
                    if m.get("key") == "default.audio.sink" and isinstance(m.get("value"), dict):
                        return m["value"].get("name")
        return None

    def node_by_name(self, name):
        for o in self.dump():
            if o.get("type", "").endswith(":Node") and o["info"]["props"].get("node.name") == name:
                return o
        return None

    def add_sink(self, name, desc="Headset", bt=True):
        props = ["factory.name=support.null-audio-sink", "node.name=%s" % name, "node.description=\"%s\"" % desc,
                 "media.class=Audio/Sink", "object.linger=true", "audio.position=[FL,FR]"]
        if bt:
            props.append("device.api=bluez5")
        self.tool("pw-cli", "create-node", "adapter", "{ %s }" % " ".join(props))
        wait_for(lambda: self.node_by_name(name), "sink %s" % name)

    def remove_sink(self, name):
        n = self.node_by_name(name)
        self.tool("pw-cli", "destroy", str(n["id"]))
        wait_for(lambda: not self.node_by_name(name), "sink %s to go" % name)

    def play(self, name, app, **props):
        """Silence, as one stream of a libpulse client under application.name = app,
        as a browser is. pw-cat would do, but it always sets media.role, and older
        WirePlumbers key on that."""
        self.spawn(name, "paplay", "--client-name=%s" % app, "--property=application.name=%s" % app,
                   *["--property=%s=%s" % kv for kv in props.items()], self.wav)

    def stream(self, app):
        for o in self.dump():
            if o.get("type", "").endswith(":Node"):
                p = o["info"]["props"]
                if p.get("media.class") == "Stream/Output/Audio" and p.get("application.name") == app:
                    return o
        return None

    def sink_of(self, app):
        """The sink name a stream is really linked to, from the links themselves."""
        d = self.dump()
        s = next((o for o in d if o.get("type", "").endswith(":Node")
                  and o["info"]["props"].get("media.class") == "Stream/Output/Audio"
                  and o["info"]["props"].get("application.name") == app), None)
        if not s:
            return None
        names = {o["id"]: o["info"]["props"].get("node.name") for o in d if o.get("type", "").endswith(":Node")}
        for o in d:
            if o.get("type", "").endswith(":Link") and o["info"].get("output-node-id") == s["id"]:
                return names.get(o["info"].get("input-node-id"))
        return None

    def stored(self):
        path = os.path.join(self.env["XDG_STATE_HOME"], "wireplumber/stream-properties")
        loader = importlib.machinery.SourceFileLoader("ztos_audiod", DAEMON)
        spec = importlib.util.spec_from_loader("ztos_audiod", loader)
        a = importlib.util.module_from_spec(spec)
        loader.exec_module(a)
        return a.read_stream_state(path)


def wait_for(cond, what, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.1)
    raise AssertionError("waited for %s" % what)


def vol_of(o):
    for p in (o["info"].get("params") or {}).get("Props", []):
        if "channelVolumes" in p:
            return round(p["channelVolumes"][0], 3)
    return None


KEY = "Output/Audio:application.name:%s"


def main():
    for tool in ("pipewire", "wireplumber", "pipewire-pulse", "paplay", "pactl", "wpctl", "pw-dump", "pw-cli", "pw-metadata"):
        if not shutil.which(tool):
            print("audiod-live: skipped, no %s here" % tool)
            return 0
    t = Private(keep="--keep" in sys.argv)
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(1))
    try:
        run_all(t)
    except Exception:
        print("audiod-live: failed; logs in %s" % t.dir, file=sys.stderr)
        t.keep = True
        for n in ("audiod", "wireplumber"):
            try:
                with open(os.path.join(t.dir, n + ".log"), errors="replace") as f:
                    print("--- %s\n%s" % (n, f.read()[-1500:]), file=sys.stderr)
            except OSError:
                pass
        raise
    finally:
        t.close()
    print("audiod-live: ok")
    return 0


def run_all(t):
    t.start_audio()
    t.start_daemon()
    assert t.default_sink() == SPEAKER, t.default_sink()

    # ---- one app on Default, one to be pinned
    t.play("s-a", "AppA")
    t.play("s-b", "AppB")
    wait_for(lambda: t.sink_of("AppA") == SPEAKER and t.sink_of("AppB") == SPEAKER, "streams on the speaker")
    wait_for(lambda: t.app(KEY % "AppA") and t.app(KEY % "AppB"), "the apps in the state file")
    id_a, id_b = t.stream("AppA")["id"], t.stream("AppB")["id"]

    # ---- volume on a running app: WirePlumber saves it
    t.ctl("app-volume", KEY % "AppA", "50")
    wait_for(lambda: vol_of(t.stream("AppA")) == 0.125, "AppA at 50%")
    wait_for(lambda: (t.stored().get(KEY % "AppA") or {}).get("percent") == 50, "WirePlumber saving AppA")
    wait_for(lambda: t.app(KEY % "AppA")["percent"] == 50, "the state file to show 50")
    print("  volume: applied and saved by WirePlumber")

    # ---- a slider being dragged: every step lands, the last one stays, nothing restarts
    for p in range(60, 91, 2):
        t.ctl("app-volume", KEY % "AppB", str(p))
        time.sleep(0.12)
    wait_for(lambda: vol_of(t.stream("AppB")) == 0.729, "AppB at the slider's last step")
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("percent") == 90, "the last step saved")
    wait_for(lambda: t.app(KEY % "AppB")["percent"] == 90, "the state to show it")
    assert t.stream("AppB")["id"] == id_b
    t.ctl("app-mute", KEY % "AppB", "1")
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("mute") is True, "the mute saved")
    t.ctl("app-mute", KEY % "AppB", "0")
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("mute") is False, "the unmute saved")
    print("  slider and mute: applied and saved, no stream restarted")

    # ---- a name with a space and a quote, as the Sound app sends it
    t.play("s-q", "Bob's Player")
    wait_for(lambda: t.app(KEY % "Bob's Player"), "an app with a quote in its name")
    t.ctl("app-volume", KEY % "Bob's Player", "20")
    wait_for(lambda: vol_of(t.stream("Bob's Player")) == 0.008, "its volume")
    t.stop("s-q")
    print("  a name with a quote: fine")

    # ---- pin AppB to the speaker; Bluetooth connects
    t.ctl("app-output", KEY % "AppB", SPEAKER)
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("target") == SPEAKER, "WirePlumber saving the pin")
    wait_for(lambda: t.app(KEY % "AppB")["output"] == SPEAKER, "the state to show the pin")
    assert t.app(KEY % "AppB")["output_desc"] == "Speaker"
    t.add_sink(BT, "UE MINI BOOM")
    wait_for(lambda: t.default_sink() == BT, "Bluetooth taking over")
    wait_for(lambda: t.sink_of("AppA") == BT, "the Default app to follow Bluetooth")
    time.sleep(1.0)
    assert t.sink_of("AppB") == SPEAKER, "a pinned app stays: %s" % t.sink_of("AppB")
    assert t.stream("AppA")["id"] == id_a and t.stream("AppB")["id"] == id_b, "no stream restarted"
    assert t.app(KEY % "AppA")["output"] == "default"
    assert t.app(KEY % "AppA")["playing_on"] == BT
    print("  takeover: Default follows, the pin stays, no stream restarted")

    # ---- pin an app to Bluetooth; it goes away and returns
    t.ctl("app-output", KEY % "AppB", BT)
    wait_for(lambda: t.sink_of("AppB") == BT, "AppB moving to Bluetooth")
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("target") == BT, "the new pin saved")
    t.remove_sink(BT)
    wait_for(lambda: t.sink_of("AppA") == SPEAKER, "the Default app back on the speaker")
    wait_for(lambda: t.sink_of("AppB") == SPEAKER, "the pinned app falling back to the speaker")
    wait_for(lambda: not t.app(KEY % "AppB")["output_present"], "the state saying the output is away")
    assert t.app(KEY % "AppB")["output"] == BT, "the pin is remembered while it is away"
    assert t.app(KEY % "AppB")["output_desc"] == "UE MINI BOOM", "and what it is called"
    print("  output away: both play on the speaker, the pin is remembered")
    t.add_sink(BT, "UE MINI BOOM")
    wait_for(lambda: t.sink_of("AppB") == BT, "the pin taking effect when Bluetooth returns")
    assert t.stream("AppB")["id"] == id_b, "no stream restarted"
    print("  output returns: the pinned app goes back without restarting")

    # ---- back to Default
    t.ctl("app-output", KEY % "AppB", "default")
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("target") is None, "the pin forgotten")
    wait_for(lambda: t.app(KEY % "AppB")["output"] == "default", "the state saying Default")
    t.ctl("output", SPEAKER)
    wait_for(lambda: t.sink_of("AppB") == SPEAKER, "AppB following the global output again")
    t.ctl("output", BT)
    wait_for(lambda: t.sink_of("AppB") == BT, "AppB following it both ways")
    print("  Default: the app follows the global output again")

    # ---- app restart: pin AppA to the speaker, set a volume, restart it
    t.ctl("output", SPEAKER)
    t.ctl("app-output", KEY % "AppA", BT)
    wait_for(lambda: (t.stored().get(KEY % "AppA") or {}).get("target") == BT, "the pin saved")
    time.sleep(1.2)  # WirePlumber saves after a timeout
    t.stop("s-a")
    wait_for(lambda: not t.stream("AppA"), "AppA gone")
    t.play("s-a", "AppA")
    wait_for(lambda: t.sink_of("AppA") == BT, "AppA restarted, on its pin")
    assert vol_of(t.stream("AppA")) == 0.125, vol_of(t.stream("AppA"))
    print("  app restart: volume and output restored")

    # ---- a change for an app that is not running waits for it
    t.stop("s-a")
    wait_for(lambda: not t.stream("AppA"), "AppA gone")
    wait_for(lambda: not t.app(KEY % "AppA")["running"], "AppA not running in the state")
    t.ctl("app-volume", KEY % "AppA", "80")
    t.ctl("app-output", KEY % "AppA", "default")
    wait_for(lambda: t.app(KEY % "AppA")["percent"] == 80, "the held volume in the state")
    wait_for(lambda: t.app(KEY % "AppA")["output"] == "default", "the held output in the state")
    t.play("s-a", "AppA")
    wait_for(lambda: t.stream("AppA") and vol_of(t.stream("AppA")) == 0.512, "the held volume on the new stream")
    wait_for(lambda: t.sink_of("AppA") == SPEAKER, "the held output on the new stream")
    print("  held change: applied when the app next plays")

    # ---- shell restart is nothing: the daemon's restart loses nothing either
    t.ctl("app-volume", KEY % "AppA", "30")
    wait_for(lambda: (t.stored().get(KEY % "AppA") or {}).get("percent") == 30, "AppA at 30 saved")
    t.ctl("app-output", KEY % "AppB", BT)
    wait_for(lambda: (t.stored().get(KEY % "AppB") or {}).get("target") == BT, "AppB's pin saved")
    t.stop("audiod")
    t.start_daemon()
    wait_for(lambda: t.app(KEY % "AppB") and t.app(KEY % "AppB")["output"] == BT, "the state after a daemon restart")
    assert t.sink_of("AppB") == BT
    assert t.stream("AppB")["id"] == id_b, "the daemon's restart left the stream alone"
    print("  daemon restart: nothing moves")

    # ---- reboot: everything stops, then comes back, the Bluetooth output later
    time.sleep(1.5)  # let WirePlumber's save timer run
    for s in ("s-a", "s-b", "audiod"):
        t.stop(s)
    t.stop_audio()
    t.start_audio()
    t.start_daemon()
    t.play("s-a", "AppA")
    t.play("s-b", "AppB")
    wait_for(lambda: t.stream("AppA") and vol_of(t.stream("AppA")) == 0.027, "AppA's volume after a reboot")
    wait_for(lambda: t.stream("AppB") and vol_of(t.stream("AppB")) == 0.729, "AppB's volume after a reboot")
    wait_for(lambda: t.sink_of("AppB") == SPEAKER, "AppB falling back while its output is away")
    wait_for(lambda: t.app(KEY % "AppB") and t.app(KEY % "AppB")["output"] == BT, "AppB's pin after a reboot")
    t.add_sink(BT, "UE MINI BOOM")
    wait_for(lambda: t.sink_of("AppB") == BT, "AppB's pin taking effect when its output connects")
    print("  reboot: volume restored; the pin returns once its output connects")

    assert t.procs["audiod"].poll() is None, "the daemon is still up"


if __name__ == "__main__":
    sys.exit(main())
