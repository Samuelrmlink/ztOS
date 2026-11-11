---
name: ztos-hdmi-capture
description: Using the PC's HDMI capture card to see what the phone sends over USB-C DP Alt Mode (dock to HDMI to an MS2109 card). Use it when verifying the external display or the desktop on DP-1.
---

# HDMI capture rig

Chain: phone → powered USB-C dock → HDMI → MacroSilicon MS2109 capture card (`534d:2109`, `/dev/video0`) on the PC.

- **`scripts/bench/hdmi-grab [out.png] [secs]`** takes one picture. It exits 0 on a real picture.
- **`scripts/bench/hdmi-watch DIR`** records continuously (20 s chunks, up to 10 min) and keeps one frame from each chunk that has a picture (`DIR/PICTURE-N.jpg`). Use it when the phone side triggers hotplug.

## Traps
- After the stream opens, the card sends about 1.5 s of flat frames (Y=7 everywhere), which looks the same as "no signal". A short grab looks dead when it isn't, and the tools above skip those frames.
- **The card only asserts HPD while a stream is open.** With no capture running, the phone sees the display unplugged, and DP-1 is removed and added as the stream closes and opens. That is the card, not a bug. Keep a long capture running for tests that need the monitor present.
- Real unplug tests (dock detach) need the owner's hands. Add them to your end-of-task list.
