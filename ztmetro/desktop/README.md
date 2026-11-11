# Desktop mode: how to try it

An XFCE desktop beside Metro, in the same session. See `DESIGN.md` for why it
is built this way.

## On the phone

1. Install the shell from this branch. The window manager is part of
   `ztmetro`, idle until a second output exists. Then install the optional
   package: `makepkg -si` in `packages/ztos-desktop`. That pulls in XFCE's
   panel, xfdesktop, the settings daemon, Thunar, xfce4-terminal and Xwayland.
2. Nothing of XFCE runs until you ask: **Start automatically** is off by
   default, and a monitor that connects is only an output until then.
   Start a desktop, in any of three ways:
   - **Desktop mode** (ztdeskmode, from its tile or the app list): it shows
     whether XFCE runs and on which output, the monitor and its mode, and
     has Start/Stop, **Start automatically**, the virtual screen and the
     monitor resolution. It is Settings' page, as an app of its own;
   - Settings → display → desktop mode → **Start XFCE on a virtual screen**;
   - from a shell:

         echo 'desktop add 1920x1080' > $XDG_RUNTIME_DIR/ztmetro.ctl
         echo 'desktop start'         > $XDG_RUNTIME_DIR/ztmetro.ctl

   With a monitor on USB-C (once DP Alt Mode gives a DRM connector), the
   desktop goes to it by itself. XFCE starts on it too when **Start
   automatically** is on.
3. Open **Desktop** (ztdesk) from the app list to see and use it:
   - **Touchpad mode:** move with one finger, tap to click, tap with two
     fingers for the right button, move two fingers to scroll, tap then hold
     to drag.
   - **Direct mode:** a tap is a click where it lands, a hold is a right
     click, and a finger that moves drags.
   - Both modes: pinch to zoom the view.
   - The bar: **keyboard** brings up the phone's own keyboard, which types
     into the desktop. **keys** adds Esc, Tab, Win, Alt+Tab, Alt+F4 and the
     arrows. **fit** shows the whole desktop. **…** starts or stops XFCE, or
     removes the virtual screen.
4. Move an app:
   - **Phone → desktop:** open the switcher (hold Start), then pull the app's
     card down.
   - **Desktop → phone:** use the phone button in a window's title bar, the
     window menu's "move to phone" (right-click a title bar, or Alt+Space), or
     Super+P.

The control pipe's `desktop ...` lines are listed at `desk_ctl` in
`desktop.c`. They cover:

- `move APP`
- `phone APP`
- `click X Y`
- `watch SECONDS`
- `state`

The state is in `$XDG_RUNTIME_DIR/ztmetro-desktop.state`: an `output NAME
monitor|virtual W H SCALE X Y` line (or `output none`), a `mode W H mHz` line
for the output, `session running|stopped` and a `window ...` line for each
window. `desktop stop` sends SIGTERM to the session's process group and,
3 s later, SIGKILL to whatever of it is still there (xfdesktop has been seen
to outlive SIGTERM).

## On a PC, without a phone

    ztmetro/desktop/dev/run.sh                      # captures in out/desktop/
    ztmetro/desktop/dev/run.sh 'c desktop move foot; sleep 1; shot moved'
    ztmetro/desktop/dev/run.sh sh                   # a shell inside

This builds ztmetro in a container (Arch, wlroots 0.20, XFCE) and runs it on
the headless backend:

- HEADLESS-1 is the "phone";
- HEADLESS-2 is a 1600×900 desktop with XFCE, on this machine's GPU.

## What it costs (Galaxy S9, measured)

**RAM:** a 1920×1080 virtual desktop with XFCE costs about 240 MB of
"available" (1229 MB → 992 MB). The resident sizes behind that:

| Process | RSS |
|---|---|
| xfdesktop | 106 MB (the wallpaper, at full size) |
| xfce4-panel | 75 MB |
| Thunar daemon | 46 MB |
| xfsettingsd | 40 MB |
| tumblerd | 17 MB |
| xfconfd | 15 MB |
| ztmetro | +15 MB (the desktop's buffers) |
| Xwayland, once an X11 app has run | 75 MB |

With Waydroid also running, that leaves about 1 GB free on the 3.5 GB phone.

**CPU and battery:**

- A virtual desktop that nobody looks at is drawn once a second, and its
  apps get one frame callback a second. The shell measured 0% of a core
  over 10 s.
- With ztdesk in front, the desktop runs at 60 Hz, but only redraws when
  something on it changed. ztdesk captures only changed frames, at most
  30 a second.
- Without a desktop output, nothing of desktop mode runs: no timers, and
  Xwayland only starts when an X11 client connects.

**GPU:** glmark2 on the desktop output, composited by the shell (1280×720
window, the same five scenes each time: build, texture, shading=phong,
refract, terrain):

| Run | Score |
|---|---|
| glmark2-es2-wayland (GLES 3.1) | 367 |
| glmark2-wayland (desktop GL 3.1) | 362 |
| glmark2-es2-wayland --off-screen | 549 |
| glmark2 on X11 (Xwayland, glamor) | 515 |

Every run reported `GL_RENDERER Mali-G72 MC18 (Panfrost)`; no llvmpipe or
softpipe anywhere.
