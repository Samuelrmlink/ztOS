"""ztos-waydroid-power: workers obey current focus, the user's background
choices keep Android running, and Android playing sound is not frozen until it
stops (ROADMAP 8.54). Runs on a host: sudo and pactl are stubs."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
USER_SH = ROOT.parents[1] / 'os/overlay/usr/lib/ztos/user.sh'

SUDO = '''#!/bin/bash
printf '%s\\n' "$3${4:+ $4}" >> "$XDG_RUNTIME_DIR/events"
if [[ $3 == unfreeze && $CHANGE_POLICY == 1 ]]; then
    printf 'idle\\n' > "$XDG_RUNTIME_DIR/ztos-waydroid-power-state"
fi
'''
# `pactl -f json list sink-inputs` prints sinks.json; `pactl subscribe` prints
# an event now and then, as the real one does when a stream changes.
PACTL = '''#!/bin/bash
if [[ $* == '-f json list sink-inputs' ]]; then
    cat "$XDG_RUNTIME_DIR/sinks.json" 2>/dev/null || echo '[]'
elif [[ $1 == subscribe ]]; then
    while sleep 0.3; do echo "Event 'change' on sink-input #93"; done
fi
'''


def stream(name, corked):
    return {'index': 93, 'corked': corked, 'properties': {'application.name': name}}


class Phone:
    def __init__(self, directory, state):
        self.root = Path(directory)
        self.runtime = self.root / 'run'
        self.home = self.root / 'home'
        self.runtime.mkdir()
        (self.home / '.config/ztos').mkdir(parents=True)
        (self.home / '.local/share/ztos/android').mkdir(parents=True)
        (self.home / '.local/share/ztos/android/applications.json').write_text(json.dumps({
            'com.spotify.music@waydroid': {'runtime': 'waydroid', 'name': 'Spotify'},
            'com.example.atl': {'runtime': 'atl'}}))
        commands = self.root / 'bin'
        commands.mkdir()
        for name, text in (('sudo', SUDO), ('pactl', PACTL)):
            (commands / name).write_text(text)
            (commands / name).chmod(0o755)
        conf = self.root / 'user.conf'
        conf.write_text(f'ZTOS_USER=nobody\nZTOS_UID={os.getuid()}\nZTOS_HOME={self.home}\n')
        cfg = self.root / 'waydroid.cfg'
        cfg.write_text('[waydroid]\n')
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.runtime),
                        PATH=str(commands) + ':' + os.environ['PATH'],
                        ZTOS_USER_SH=str(USER_SH), ZTOS_USER_CONF=str(conf),
                        ZTOS_WAYDROID_CFG=str(cfg), ZTOS_WAYDROID_AUDIO_HOLD='1',
                        ZTOS_WAYDROID_AUDIO_WATCH_MAX='20', CHANGE_POLICY='0')
        self.policy(state)

    def policy(self, state):
        (self.runtime / 'ztos-waydroid-power-state').write_text(state + '\n')

    def sinks(self, *streams):
        (self.runtime / 'sinks.json').write_text(json.dumps(list(streams)))

    def sync(self, **env):
        subprocess.run(['bash', str(ROOT / 'power.sh'), 'sync'], env={**self.env, **env},
                       check=True, timeout=10)

    def events(self):
        path = self.runtime / 'events'
        return path.read_text().splitlines() if path.exists() else []

    def watching(self):
        """The watcher holds its lock while it runs."""
        lock = self.runtime / 'ztos-waydroid-audio-watch.lock'
        if not lock.exists():
            return False
        return subprocess.run(['flock', '-n', str(lock), 'true']).returncode != 0

    def wait_watcher(self, timeout=8):
        end = time.monotonic() + timeout
        while self.watching() and time.monotonic() < end:
            time.sleep(0.1)
        return not self.watching()


class PowerOrderingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def phone(self, state):
        return Phone(self.directory.name, state)

    def test_stale_worker_uses_latest_idle_state(self):
        phone = self.phone('idle')
        phone.sync()
        self.assertEqual(phone.events(), ['display off', 'freeze'])

    def test_home_during_thaw_finishes_frozen(self):
        phone = self.phone('active')
        phone.sync(CHANGE_POLICY='1')
        self.assertEqual(phone.events(), ['unfreeze', 'display on', 'display off', 'freeze'])

    def test_marker_cleanup_reconciles_deferred_idle(self):
        phone = self.phone('idle')
        marker = phone.runtime / 'ztos-waydroid-launching'
        marker.write_text(str(os.getpid()))
        phone.sync()
        self.assertEqual(phone.events(), [])
        marker.unlink()
        phone.sync()
        self.assertEqual(phone.events(), ['display off', 'freeze'])


class BackgroundChoiceTests(unittest.TestCase):
    """Settings > Background apps keeps Android running, as keep_awake does."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def trial(self, state, background=None, conf=None):
        phone = Phone(self.directory.name, state)
        if background is not None:
            (phone.home / '.config/ztos/background-apps').write_text(background)
        if conf is not None:
            (phone.home / '.config/ztos/android.conf').write_text(conf)
        phone.sync()
        return phone.events()

    def test_settings_choice_keeps_android_running(self):
        self.assertEqual(self.trial('idle', 'waydroid.com.spotify.music\n'), ['display off'])

    def test_desktop_id_form(self):
        self.assertEqual(self.trial('screen-off', 'ztos-android-com.spotify.music-waydroid.desktop\n'),
                         ['display off'])

    def test_keep_awake_still_works(self):
        self.assertEqual(self.trial('idle', conf='keep_awake = com.spotify.music\n'), ['display off'])

    def test_unrelated_or_atl_choice_freezes(self):
        self.assertEqual(self.trial('idle', 'waydroid.com.other\ncom.example.atl\n# x\n'),
                         ['display off', 'freeze'])


class PlayingTests(unittest.TestCase):
    """Android playing out of sight runs until it has been quiet for the hold."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.phone = Phone(self.directory.name, 'idle')
        self.addCleanup(self.phone.wait_watcher)

    def test_corked_or_other_streams_do_not_hold(self):
        self.phone.sinks(stream('Waydroid', True), stream('ztOS system sounds', False))
        self.phone.sync()
        self.assertEqual(self.phone.events(), ['display off', 'freeze'])
        self.assertFalse(self.phone.watching())

    def test_playing_is_not_frozen_until_it_stops(self):
        self.phone.env['ZTOS_WAYDROID_AUDIO_HOLD'] = '3'
        self.phone.sinks(stream('Waydroid', False))
        self.phone.sync()
        self.assertEqual(self.phone.events(), ['display off'])
        time.sleep(0.2)
        self.assertTrue(self.phone.watching())
        # a second policy event while it plays starts no second watcher
        self.phone.sync()
        self.assertEqual(self.phone.events(), ['display off', 'display off'])
        self.phone.sinks(stream('Waydroid', True))   # paused, or between tracks
        time.sleep(0.6)
        # a focus change during the hold leaves the freeze to the watcher
        self.phone.sync()
        self.assertNotIn('freeze', self.phone.events())
        self.assertTrue(self.phone.wait_watcher())
        deadline = time.monotonic() + 5
        while 'freeze' not in self.phone.events() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertEqual(self.phone.events(), ['display off'] * 4 + ['freeze'])

    def test_back_in_front_ends_the_watch_without_freezing(self):
        self.phone.sinks(stream('Waydroid', False))
        self.phone.sync()
        time.sleep(0.1)
        self.phone.policy('active')
        self.assertTrue(self.phone.wait_watcher())
        self.assertNotIn('freeze', self.phone.events())


if __name__ == '__main__':
    unittest.main()
