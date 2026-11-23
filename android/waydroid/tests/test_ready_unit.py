"""ztos-waydroid-ready: marks Android ready for any session start, waiting as
long as the session runs (not a unit's 50 s). Runs on a host: systemctl, sudo,
logger and ztos-waydroid-power are stubs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    # the session is active while $STATE/session exists
    'systemctl': '#!/bin/bash\n[[ $* == "--user -q is-active ztos-waydroid-session.service" ]] || exit 9\n'
                 '[[ -e $STATE/session ]]\n',
    # `recover ready` fails until it has been asked $READY_AFTER times
    'sudo': '#!/bin/bash\n[[ $3 == ready ]] || exit 9\nn=$(( $(cat "$STATE/asked" 2>/dev/null || echo 0) + 1 ))\n'
            'echo $n > "$STATE/asked"\n(( n >= READY_AFTER )) || exit 1\ntouch "$STATE/marker"\n',
    'logger': '#!/bin/bash\nprintf "%s\\n" "$*" >> "$STATE/log"\n',
    'ztos-waydroid-power': '#!/bin/bash\necho "$*" >> "$STATE/power"\n',
    'sleep': '#!/bin/bash\nexit 0\n',
}


class ReadyUnitTests(unittest.TestCase):
    def run_ready(self, session=True, ready_after=1, limit='600'):
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            commands = state / 'bin'
            commands.mkdir()
            for name, text in STUBS.items():
                (commands / name).write_text(text)
                (commands / name).chmod(0o755)
            if session:
                (state / 'session').touch()
            env = dict(os.environ, STATE=str(state), READY_AFTER=str(ready_after),
                       PATH=str(commands) + ':' + os.environ['PATH'], ZTOS_WAYDROID_READY_MAX=limit)
            result = subprocess.run(['bash', str(ROOT / 'ztos-waydroid-ready')], env=env, timeout=20)
            read = lambda name: (state / name).read_text().splitlines() if (state / name).exists() else []
            return (result.returncode, (state / 'marker').exists(), read('asked'), read('power'), read('log'))

    def test_ready_first_time(self):
        code, marker, asked, power, log = self.run_ready()
        self.assertEqual((code, marker, asked, power), (0, True, ['1'], ['sync']))
        self.assertIn('marker set', log[-1])

    def test_slow_android_is_waited_for(self):
        # each `ready` waits 90 s itself: four of them is past the old 50 s unit limit
        code, marker, asked, power, _ = self.run_ready(ready_after=4)
        self.assertEqual((code, marker, asked[-1], power), (0, True, '4', ['sync']))

    def test_no_session_no_wait(self):
        code, marker, asked, power, log = self.run_ready(session=False)
        self.assertEqual((code, marker, asked, power), (0, False, [], []))
        self.assertIn('no marker', log[-1])

    def test_gives_up_at_its_limit(self):
        code, marker, _, power, log = self.run_ready(ready_after=10**9, limit='1')
        self.assertEqual((code, marker, power), (0, False, []))
        self.assertIn('no marker', log[-1])


if __name__ == '__main__':
    unittest.main()
