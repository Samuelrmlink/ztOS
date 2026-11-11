# Authentication and date settings

ztMETRO owns the top password sheet and captures keyboard input while it is
open. The focused application receives no password keystrokes. Text is masked,
key previews and prediction are disabled, and the input buffer is cleared on
submission, retry, cancellation, disconnect, or screen-off. The Unix socket
lives in the user's private runtime directory, is mode 0600, and accepts only
clients with the same UID. Requests contain a message; they cannot execute a
command in the compositor.

`ztos-auth COMMAND ARG...` uses sudo's authentication and policy, with up to
three attempts and explicit incorrect/accepted feedback. Valid sudo tickets
are reused. Secrets are passed through stdin, never command arguments or a
password file. `ztos-askpass` is also provided for `sudo -A`; bare terminal sudo
continues to use its terminal prompt. The askpass adapter supplies input;
sudo remains responsible for checking it.

The native `ztos-polkit-agent` registers for the graphical login session and
uses the same sheet for polkit requests, including pkexec. Verification and
permissions remain with polkit. The agent builds against polkit and GLib,
already provided by the base package set; it does not need Python GI bindings.
It starts with the compositor and exits when its parent exits.

## Bluetooth pairing

`ztos-bt-agent` (rootfs/overlay/usr/local/bin, a user service) is BlueZ's default
agent, with capability KeyboardDisplay. It registers again whenever bluetoothd
restarts. Settings pairs with plain `bluetoothctl pair`, which uses it.

The agent asks through a sheet (`auth/btpair.c`). The sheet is the password sheet
with another field. While it shows, `auth_active` is set too, so its keys take
the same path and no other dialog or touch gets past it. Polkit's prompt is told
BUSY while the sheet is up; if polkit got there first, the request waits for it.

| BlueZ asks | Sheet | Buttons |
|---|---|---|
| RequestConfirmation | a number: does the other device show it? | yes, no |
| RequestPinCode | type a PIN (1 to 16 letters and digits, on the number pad, with abc for letters) | pair, cancel |
| RequestPasskey | type a passkey (6 digits, on the digits pad) | pair, cancel |
| DisplayPinCode, DisplayPasskey | a number to type on the other device | cancel |
| RequestAuthorization | pairing with no code: allow it? | pair, cancel |
| AuthorizeService | a paired device wants to use a service | allow, deny |

A typed PIN is shown, not starred: it is short and copied from the other device,
and it has no use once paired. It stays out of logs and is wiped from the shell
on answer, cancel and sleep.

**When it asks:** only while the adapter is discoverable, which Settings keeps on
while its Bluetooth page is open. BlueZ doesn't say which side started a pairing,
so everything else is refused with no sheet. A pairing started in a terminal
needs `bluetoothctl discoverable on`.

**Remembered codes:** the PIN and passkey sheets have a tick box, "Use this PIN
(passkey) for similar devices for 3 hours", off each time. Ticked, the answer
line ends in `\tremember`, and the agent keeps the code for 3 hours, in memory
only (never on disk or in a log; gone on restart), keyed by the set of service
UUIDs the device advertised. It then answers a PIN or passkey request by itself
only when the phone started the pairing (an Android app is using the device, or
Settings' `bluetoothctl pair` for it is running), the device advertises exactly
the same set, and that device hasn't failed with the code; each time, a
notification says "Paired NAME with the remembered PIN". A pairing that doesn't
end Paired marks the device failed for that code, and the sheet comes back. Never
for a confirmation, an authorization or an unsolicited request. The agent's
docstring has the details.

**Never by itself:** nothing answers yes without a person, with one exception:
AuthorizeService is allowed for a device whose `Trusted` is set. The agent looks
twice, 1.5 s apart, because Settings trusts a device just after pairing it and a
speaker may connect back before that. Paired alone isn't enough.

**How it ends:** the agent ends a question at 55 s, before bluetoothd's 60 s, and
a number at 60 s. Refusing raises `org.bluez.Error.Rejected`; BlueZ's Cancel and
the timeout raise `org.bluez.Error.Canceled`. A number on the screen has no reply
to wait for, so the agent watches the device and closes when it's `Paired` or no
longer `Connected`. Its cancel button calls `CancelPairing`, then `Disconnect`
(a pairing from the other side has nothing to cancel). It never does this to a
device that's already paired, because `CancelPairing` removes the bond if no pairing
is in progress. If the agent dies, the shell closes the sheet 5 s after its time
is up.

**Locked or asleep:** a request wakes the screen but doesn't show over the lock.
It adds a silent notification, which the lock screen shows only as a "B" badge,
and the sheet appears on unlocking if the request is still open. The sheet isn't left on a screen going to sleep. Typing is lost then, but
the question comes back. While the sheet is up the screen doesn't time out.

**The channel:** requests are lines on the control pipe (0600), answers are
connections to `$XDG_RUNTIME_DIR/ztos-bt-agent.sock` (0600, peer UID checked by
both). Device names come over the air, so both ends clean them: UTF-8 only, no
controls, no bidirectional or zero-width marks, `|` becomes `/`, at most 96 bytes.
`auth/btpair-proto.h` has the format. Answers can only reach BlueZ if they fit
the mode; a request that doesn't parse shows nothing.

Host tests: `make check-bt-pair`. On the phone: `make check-bt-pair-sheet`, and
`tests/bt-agent-call.py` calls the agent as BlueZ would, with sudo, since BlueZ's
bus policy lets only root call `org.bluez.Agent1`. The agent checks that too.

Settings → date + region exposes automatic time, manual date/time, IANA time
zone, regional formats, NTP servers, and sync now. Choosing a region suggests
its country pool; the server field remains editable. Sync now enables NTP,
restarts systemd-timesyncd, and checks for a newly updated synchronization
marker. If a server does not respond within 20 seconds, the UI reports that
synchronization is still pending. Saving regional formats affects newly
opened applications and does not change their display language. Date changes
use the interactive handler; there is no passwordless date-setting rule.

The installer builds and installs the native agent with ztMETRO, then installs
the Python wrappers, backend and shared transport from the OS overlay. Fresh
builds include these components. Verify with `make check-auth check-launch
check-keyboard` on the phone and `python ztmetro/tests/date-region.py` on the
host. Real sudo and pkexec authentication, wrong-password retry, root command
execution, live compositor cancellation, and a live NTP sync were also tested.
