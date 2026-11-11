#!/usr/bin/python3
"""On the phone: each volume really sets its own stream's loudness.

Plays a very quiet 440 Hz tone (-54 dBFS before any gain) the way ztphone,
ztos-sounds and ztclockd do (media.role, ztos.stream, ...), records the speaker
chain's monitor (before the output's own volume), and reads the stream's
volumes from pw-dump. Loudness at the speaker = monitor RMS x output gain.
Restores every level it touches."""
import json, math, os, struct, subprocess, sys, time, wave

HOME = os.path.expanduser("~")
RUN = os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000")
RING_CFG = HOME + "/.config/ztmetro/config"
ALARM_CFG = HOME + "/.config/ztclock/settings"
TONE = "/tmp/vs-tone.wav"
AMP = 65 / 32768
results = []


def ok(name, cond, detail=""):
    results.append((name, bool(cond)))
    print("%s %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def tone(seconds):
    w = wave.open(TONE, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<h", int(65 * math.sin(2 * math.pi * 440 * i / 48000)))
                           for i in range(int(48000 * seconds))))
    w.close()


def state():
    return json.load(open(RUN + "/ztos-audio-state.json"))


def cube(p):
    return (p / 100.0) ** 3


def dev_gain():
    a = state()["active"]
    return 0.0 if a["mute"] else cube(a["percent"]), a


def ctl(*words):
    return subprocess.run(["ztos-audiod", "ctl"] + list(words), capture_output=True, text=True).stdout.strip()


def shell_hup():
    subprocess.run(["pkill", "-HUP", "-x", "ztmetro"])


def set_ring(p):
    t = open(RING_CFG).read()
    lines = [l for l in t.splitlines() if not l.strip().startswith("ring_volume")]
    lines.append("ring_volume = %d" % p)
    open(RING_CFG + ".vs", "w").write("\n".join(lines) + "\n")
    os.replace(RING_CFG + ".vs", RING_CFG)
    shell_hup()          # as Settings does: the shell reloads and tells ztos-audiod


def set_alarm(p):
    t = open(ALARM_CFG).read() if os.path.exists(ALARM_CFG) else ""
    lines = [l for l in t.splitlines() if not l.startswith("volume ")]
    lines.append("volume %d" % p)
    open(ALARM_CFG + ".vs", "w").write("\n".join(lines) + "\n")
    os.replace(ALARM_CFG + ".vs", ALARM_CFG)
    ctl("levels")


def stream_gain_of(word):
    """master x channel of the playing ztos.stream=word streams, from pw-dump"""
    d = json.loads(subprocess.run(["pw-dump"], capture_output=True, text=True).stdout)
    out = []
    for o in d:
        p = (o.get("info") or {}).get("props") or {}
        if p.get("ztos.stream") == word and p.get("media.filename") == TONE:
            for pr in (o["info"].get("params") or {}).get("Props", []):
                if "channelVolumes" in pr:
                    out.append((o["id"], pr.get("volume", 1.0) * pr["channelVolumes"][0], p.get("media.role")))
    return out


def play(word, role, level_percent, share=1.0, boost=False, seconds=3.0):
    g, a = dev_gain()
    cap = 1.0 if a["kind"] == "bluetooth" else 100.0
    want = min(cap, cube(level_percent) / g) if g > 0 else cap
    vol = min(10.0, want * share)
    props = "{ ztos.stream = %s ztos.volume.share = %.3f ztos.volume.boost = %s state.restore-props = false }" % (
        word, share, "true" if boost else "false")
    tone(seconds)
    return subprocess.Popen(["pw-play", "--media-role", role, "--volume", "%.4f" % vol, "-P", props, TONE])


def record(seconds):
    raw = "/tmp/vs-rec.raw"
    r = subprocess.Popen(["pw-record", "-a", "--format", "f32", "--channels", "1", "-P",
                          "{ stream.capture.sink = true }", "--target", "ztos-speaker", raw])
    time.sleep(seconds)
    r.terminate(); r.wait()
    d = open(raw, "rb").read()
    s = struct.unpack("<%df" % (len(d) // 4), d[:len(d) // 4 * 4])
    win = 4800
    rms = [math.sqrt(sum(x * x for x in s[i:i + win]) / win) for i in range(0, len(s) - win, win)]
    if not rms:
        return 0.0
    top = max(rms)
    loud = [r for r in rms if r > 0.6 * top]
    return sum(loud) / len(loud)


def loudness(rec_rms):
    """the tone's level at the speaker, relative to the tone itself"""
    g, _ = dev_gain()
    return rec_rms / (AMP / math.sqrt(2)) * g


def near(x, y, tol=0.12):
    return y > 0 and abs(x - y) / y <= tol


def main():
    st = state()
    a = st["active"]
    print("output:", a["name"], a["kind"], "%d%%" % a["percent"], "streams:", st.get("streams"))
    if a["kind"] == "bluetooth":
        print("Bluetooth is the output: these measurements are for the speaker")
        return 2
    ring0 = st["streams"]["ring"]["percent"]
    alarm_text0 = open(ALARM_CFG).read() if os.path.exists(ALARM_CFG) else None
    media0 = a["percent"]
    try:
        # 1. the ring at two levels: the loudness follows the level, not Media
        for ring in (40, 70):
            set_ring(ring)
            time.sleep(0.8)
            p = play("ring", "Notification", ring, seconds=2.5)
            time.sleep(0.6)
            got = stream_gain_of("ring")
            l = loudness(record(1.4))
            p.wait()
            ok("ring %d%%: stream gain" % ring, got and near(got[0][1] * dev_gain()[0], cube(ring), 0.05),
               "pw-dump %s, x output = %.4f, want %.4f" % (got, got[0][1] * dev_gain()[0] if got else -1, cube(ring)))
            ok("ring %d%%: measured loudness" % ring, near(l, cube(ring)), "measured %.4f want %.4f" % (l, cube(ring)))
        # 2. the level moved while it plays: ztos-audiod follows
        p = play("ring", "Notification", 70, seconds=5)
        time.sleep(0.8)
        set_ring(35)
        time.sleep(1.5)
        got = stream_gain_of("ring")
        l = loudness(record(1.2))
        p.wait()
        ok("ring moved to 35% while playing", got and near(got[0][1] * dev_gain()[0], cube(35), 0.05) and near(l, cube(35)),
           "stream x output %.4f, measured %.4f, want %.4f" % (got[0][1] * dev_gain()[0] if got else -1, l, cube(35)))
        # 3. Media moved while the ring plays: the ring stays where it is
        set_ring(50)
        time.sleep(0.8)
        p = play("ring", "Notification", 50, seconds=6)
        time.sleep(0.8)
        ctl("volume", str(max(15, media0 - 15)))
        time.sleep(2.0)
        l = loudness(record(1.2))
        got = stream_gain_of("ring")
        p.wait()
        ok("Media %d%% -> %d%%, ring still 50%%" % (media0, max(15, media0 - 15)), near(l, cube(50)),
           "measured %.4f want %.4f (stream %s, output %d%%)" % (l, cube(50), got, state()["active"]["percent"]))
        ctl("volume", str(media0))
        time.sleep(1.5)
        # 4. the alarm, at its level, on its own role
        set_alarm(60)
        time.sleep(0.5)
        p = play("alarm", "Alarm", 60, boost=True, seconds=2.5)
        time.sleep(0.6)
        got = stream_gain_of("alarm")
        l = loudness(record(1.4))
        p.wait()
        ok("alarm 60%", got and got[0][2] == "Alarm" and near(l, cube(60)),
           "role %s, measured %.4f want %.4f" % (got[0][2] if got else None, l, cube(60)))
        # 5. its floor: the file says 0, it plays at 20
        set_alarm(0)
        time.sleep(1.0)
        st2 = state()
        ok("alarm floor", st2["streams"]["alarm"]["percent"] == 20, "state says %s" % st2["streams"]["alarm"])
        # 6. Media very low: the alarm turns the output up, and it goes back
        set_alarm(60)
        ctl("volume", "3")
        time.sleep(2.0)
        p = play("alarm", "Alarm", 60, boost=True, seconds=4)
        time.sleep(2.5)
        mid = state()
        l = loudness(record(1.0))
        p.wait()
        ok("Media 3%: output turned up for the alarm", mid["streams"]["boost"] and mid["active"]["percent"] > 3 and near(l, cube(60), 0.15),
           "output %d%%, boost %s, measured %.4f want %.4f" % (mid["active"]["percent"], mid["streams"]["boost"], l, cube(60)))
        time.sleep(5.0)
        after = state()
        ok("and back to 3% after it", after["active"]["percent"] == 3 and not after["streams"]["boost"],
           "output %d%%, boost %s" % (after["active"]["percent"], after["streams"]["boost"]))
        # 7. none of these counted as apps
        ok("ring and alarm sounds not listed as apps", not any(x["name"] == "pw-play" and x["running"] for x in state()["apps"]))
    finally:
        ctl("volume", str(media0))
        set_ring(ring0)
        if alarm_text0 is not None:
            open(ALARM_CFG + ".vs", "w").write(alarm_text0)
            os.replace(ALARM_CFG + ".vs", ALARM_CFG)
            ctl("levels")
        time.sleep(1.5)
        s = state()
        print("restored: media %d%%, ring %s, alarm %s" % (s["active"]["percent"], s["streams"]["ring"], s["streams"]["alarm"]))
    bad = [n for n, good in results if not good]
    print("%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
