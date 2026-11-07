# Keyboard and sound controls

Settings → keyboard exposes key height (42–72 logical points; default 56)
and Windows Phone key sounds. Height changes apply to the visible keyboard,
its popups, touch geometry, and the area kept clear for the focused app.

All letter keys have the same width and height. The middle letter rows use a centered nine-column grid with the top row’s
key width. Shift and backspace extend to the side edges. The bottom function
row fills the keyboard width. Emoji, comma, and dot match the widths and positions of Z, X, and M
respectively. Space fills the span between comma and dot; number and enter
absorb the side margins. The accent
dot at the shift/symbol corner has a 28-point invisible touch target. Hold it
for 450 ms to hide the keys and use the keyboard as a cursor trackpad: drag
horizontally to move by characters and vertically to move by lines. Lifting
or cancelling restores the keys without typing anything. Trackpad motion
sends unmodified arrow keys to the focused application, so cursor support
follows that application's arrow-key handling. Extra fingers are ignored
while holding the dot; movement is silent and retains fractional steps.

The smile key opens 24 emoji; `abc` returns to letters. Unicode is delivered
through the virtual keyboard's XKB map, including to applications without a
text-input protocol. ZrnMetroCore clients also decode that keymap as UTF-8.

## Number pads

A numbers-only field gets a number pad instead of the letters, in the same four
rows of the key height setting, so the app's clear area does not move, with the
same sounds, haptics, key pop and touch targets. There is no cursor dot.

| Pad | Keys | Used for (text-input-v3 purpose) |
| --- | --- | --- |
| digits | 0-9, backspace, enter | `digits`, `pin` |
| number | 0-9, `-`, `.`, backspace, enter, abc | `number` |
| phone | 0-9, `*`, `#`, `+`, backspace, enter, abc | `phone` |

Every other purpose has the full keyboard. On the number and phone pads, `abc`
goes to the letters, where the `123` key goes back. The keyboard masks nothing,
for a PIN or anything else. The pad follows a field whose purpose or focus changes
while the keyboard is up; a finger on a key at that moment presses nothing.

Where it comes from, first match wins (`osk_mode_choose` in `osk-numeric.h`):

1. A field of the shell's own: what it asked for with `osk_show_mode`. The app
   list's search, Hyperscan and the password prompt use plain `osk_show`, so they
   always have the letters, whatever the app behind them has focused.
2. The focused text-input-v3 field's content purpose.
3. What an app of ours asked for on the control pipe: `osk numeric digits|number|phone [Y]`
   (`zmc_keyboard_numeric` in ZrnMetroCore; `TextBox(numbers=...)` or a field's
   `inputMethodHints` in the SDK; the browser, from WebKit's input purpose for the
   page's field). `osk show` without a position, or `osk hide`, goes back to the letters.

Android apps under Waydroid get the letters: its input method window says nothing
about the field. See `android/waydroid/KEYS.md`.

Touch targets cover the gaps between keys. An English common-word prefix
model modestly expands likely next letters at key edges; the center of every
key always selects that key. It does not replace text or save typed words.
The prefix is cleared when focus changes or the keyboard closes.

`ztos-sounds` uses the PulseAudio protocol supplied by PipeWire. It predecodes
the three Windows Phone WAVs to 48 kHz stereo and mixes at most four voices
into one reusable event stream. The tap stream requests 128-frame updates
(the server negotiates 256), with a negotiated 16 ms tap buffer. The speaker
uses 512-frame blocks on the IRQ-driven ABOX sink; the configured 2048-frame
default remains available to other output drivers. ABOX uses IRQ scheduling, 512-frame periods, eight periods
of ring capacity, and 256 frames of safety headroom. Old queued silence is
flushed before the tap stream wakes after idle. Single taps
use the requested ringer level; overlapping voices share headroom so they do
not clip. A tap replaces the oldest voice when full;
pending socket bursts retain only the newest four taps. Nothing queues an
unbounded backlog or creates a new server stream per key. Full audio blocks
include silence after a sample ends. The stream corks after 500 ms of quiet,
after its sample tail has played, so the speaker device can suspend normally.

Media detection excludes this helper and event/phone roles. Subscription
updates are coalesced, and the shell is notified only when media activity
changes.

Volume buttons adjust media while playback is active, otherwise the saved
ringer/notification level. Entering zero ringer volume gives a short haptic
pulse and silences key sounds. Media sink volume is unchanged when idle.
Settings → ringtones + sounds provides explicit sliders for both levels.

The Segoe UI v1710 fonts and their supplied notice are installed from the OS
overlay. Fontconfig selects Segoe UI for sans-serif/system-ui and maps older
Segoe WP requests to it. Noto Color Emoji supplies emoji glyphs.
