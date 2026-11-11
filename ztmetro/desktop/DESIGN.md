# Desktop mode

An XFCE desktop on a second display, next to the phone's Metro shell, in the
same Wayland session. It's like Samsung DeX: the monitor extends the phone
instead of mirroring it, and an app can move from one to the other.

The target is a monitor over USB-C DP Alt Mode. Until that works (branch
`feature/dp-altmode`), the same desktop runs on a **virtual output**. You see
and drive it from the phone with the **ztdesk** app. The virtual output stays
after DP works too: it's "desktop mode without a monitor".

## The decision: ztMETRO is the desktop's compositor

The requirement that decides it is that both environments run in **one**
Wayland session and apps move between them. A Wayland client's surfaces belong
to the compositor it is connected to. So the only design where a window can
go from the phone to the desktop and back, keeping its process and state, is
one where ztMETRO itself manages both outputs.

| Option | Apps move between phone and desktop? | Verdict |
|---|---|---|
| **ztMETRO manages output 2 as a stacking desktop; XFCE's Wayland pieces run on it** | **Yes**: same compositor, same `wl_surface` | **Chosen** |
| labwc or Wayfire nested as a Wayland client on output 2 | No. Apps on the desktop are labwc's clients, and the phone can't see them. A window can't cross from one compositor to the other. | Rejected |
| Full XFCE X11 session (xfwm4) on rootful Xwayland | No, for the same reason: X11 windows inside one Xwayland surface | Rejected |

xfwm4 only works on X11, so ztMETRO is the window manager. XFCE 4.20 has a
Wayland mode, the one used on labwc and Wayfire. Its panel (`xfce4-panel`) and
background/icons (`xfdesktop`) are **wlr-layer-shell** clients. The panel's
window buttons use **wlr-foreign-toplevel-management** (libxfce4windowing).
Thunar, xfce4-terminal and the settings dialogs are ordinary GTK 3 apps.
`xfsettingsd` runs too, but without wlr-output-management its display page does
nothing. Workspaces (ext-workspace) aren't implemented, so the pager shows one
workspace.

## How it fits in ztmetro.c

Metro draws its own screen with its own GL: perspective, tilting tiles, one
app at a time. That stays as it is. The desktop is drawn by
**wlroots' scene graph** (`wlr_scene`) on the **same renderer** (GLES2 on
Panfrost), only on the desktop's outputs. This is GPU compositing. The pixman
renderer is not used.

- `desktop/desktop.c` holds all of it, about 3,100 lines. It is included at the
  end of `ztmetro.c` (as `auth/prompt.c` is) so it can use the shell's helpers.
  `desktop/desktop.h` lists the calls `ztmetro.c` makes into it. The changes
  to `ztmetro.c` itself are about 40 lines of hooks, each one line.
- **Outputs:** the first output is the phone's panel, and stays Metro's. Every
  later output goes to `desk_new_output`. One output at a time carries the
  desktop: a monitor replaces the virtual output when it comes.
- **Windows:** an xdg toplevel is either Metro's or the desktop's. A desktop
  window is taken off `server->toplevels`, so nothing in Metro (the switcher,
  suspend, launch matching, rendering) sees it. Its xdg listeners belong to
  Metro, so they hand over at their first line (`desk_toplevel_map`,
  `_commit`, `_unmap`, `_destroy`, `_fullscreen`).
- **Window management:** stacking, click-to-focus and raise. Move by the title
  bar, with Alt+drag, or when the app asks (`xdg_toplevel.move`). Resize from
  the border (8 px), with Alt+right-drag, or when the app asks. Also maximize
  (double click, Super+Up), minimize, full screen, Alt+Tab and Alt+F4. A window
  menu opens from a right-click on the title bar, from Alt+Space, or from the
  app's own `show_window_menu` (GTK's right-click on its header bar). Its items
  are "move to phone", "minimize", "maximize", "close".
- **Decorations:** apps that ask for server-side decorations (xdg-decoration:
  foot, Qt, Firefox) get our title bar. The buttons are: to phone, minimize,
  maximize, close. GTK 3 and 4 always draw their own (client-side) title bars,
  so for them, "move to phone" is in the window menu.
- **Layers:** wlr-layer-shell (background, bottom, top, overlay). Exclusive
  zones shrink the area that maximized windows get. Layer popups (the panel's
  menus) are drawn above the windows. A layer surface asking for the phone's
  output is refused: Metro has no layers.
- **Xwayland:** X11 apps, on the desktop only. It starts lazily, when the first
  X11 client connects, and costs nothing until then. Its `DISPLAY` is given to
  the desktop session only. X11 windows can't move to the phone, because Metro
  draws xdg toplevels only.
- **Input:** one seat. Pointer capability is added while a desktop exists. A
  mouse plugged into the phone is the desktop's pointer while there is a
  desktop, and the phone's finger otherwise (as before). The keyboard follows
  the last input:
  - a click or a key on the desktop gives the desktop's front window the
    keyboard;
  - a touch on the phone, or Metro showing an app, gives it back;
  - with **ztdesk** in front, the keyboard (the phone's on-screen one too)
    types into the desktop.
- **Clients don't see the phone's panel** when they are XFCE's shell pieces
  (`xfce4-panel`, `xfdesktop`, …, matched by process name in a
  `wl_display` global filter). Otherwise they would draw a panel and a
  background on the phone's 360-point "monitor" and take it as the primary.
  Apps still see both, because an app moved to the phone needs the panel's
  `wl_output` for its scale (GTK 3 takes its scale from the outputs it is on).

## Which side a window opens on

`desk_wants`, at the window's first commit:

1. ztdesk always opens on the phone.
2. If Metro launched something and is waiting for its window (by name), it opens
   on the phone.
3. If the process has `ZT_DESKTOP=1` in its environment, it opens on the
   desktop. Everything the desktop session starts (the panel's launchers,
   Thunar's "open with") inherits it.
4. If the last input went to the desktop within 10 s, more recently than any
   touch on the phone, it opens on the desktop. This covers D-Bus-activated
   apps, such as Thunar's daemon opening a folder.
5. Otherwise it opens on the phone.

## Moving apps

- **Phone → desktop:** in the app switcher, long-press a card and choose
  "move to desktop". The control pipe's `desktop move APP|shown` does the same.
  Metro forgets the window as if it had closed. The desktop adopts it
  unmaximized and untiled, at 1024×700 (480×860 for our own phone-shaped apps).
  The scene sends it the desktop output's `enter`, its preferred buffer scale
  and its fractional scale (1.0). We send `leave` for the panel, so GTK 3
  stops drawing at 4×.
- **Desktop → phone:** use the title bar's phone button, the window menu,
  Super+P, or `desktop phone APP|focused`. The scene node goes. The window
  gets `leave` for the desktop, `enter` for the panel, Metro's scale (with the
  per-app zoom), maximized and tiled at the app area's size. Metro then shows
  it as the app in front.
- The surface and the process are the same throughout: no restart, no lost
  state.

## The virtual output (headless)

wlroots' headless backend is added to the session's multi-backend in
`desk_init`, so DRM and headless outputs exist side by side. The control pipe
adds and removes the virtual output:

    echo 'desktop add 1920x1080@60' > $XDG_RUNTIME_DIR/ztmetro.ctl   # or Settings, or ztdesk
    echo 'desktop remove'          > $XDG_RUNTIME_DIR/ztmetro.ctl

It is rendered on the GPU like any output: the scene, GLES2, and GBM buffers
from the session's allocator. **Battery:** a virtual output that no one looks
at gets no frames. Its apps get one frame callback a second instead of 60, and
nothing is drawn. It counts as "looked at" while ztdesk is in front on a lit
screen, or for the seconds given by `desktop watch N` (for wayvnc or grim).

## ztdesk, the controller

ztdesk is a Metro app on the phone (`ztmetro/desktop/ztdesk.c`, ZrnMetroCore).
It uses standard protocols only, which the shell exposes to everyone. wayvnc
works against the same desktop.

- **View:** wlr-screencopy of the desktop output. It asks for linux-dmabuf
  buffers when the shell offers them: the copy is a GPU blit, and ztdesk shows
  the frame as a texture, with no CPU pixel copy. Shared memory is the
  fallback. It captures at most 30 frames a second, and only with damage
  (`copy_with_damage`), so a still desktop costs nothing.
- **Input:** wlr-virtual-pointer and zwp-virtual-keyboard. The shell routes
  both to the desktop, never to the phone.
  - **Touchpad mode:** relative motion and a pointer. Tap to click, two-finger
    tap for the right button, two-finger drag to scroll, tap and hold then
    drag to drag.
  - **Direct mode:** absolute positions on the desktop output, through
    `create_virtual_pointer_with_output`. A tap is a click where the finger is.
  - Pinch zooms the view and two fingers pan it. In touchpad mode the view
    follows the pointer.
  - **Keyboard:** the shell's own on-screen keyboard (`osk show` on the
    control pipe). While ztdesk is in front the desktop has the keyboard, so
    the keyboard's keys go straight to the desktop's front window.
  - The mode, zoom and key bar are remembered in `~/.config/ztdesk`.

## Session start

`ztos-desktop-session` (in this directory) runs `xfsettingsd`, Thunar's daemon,
`xfdesktop` and `xfce4-panel`. Each one is started again if it dies, at most 5 times a
minute. The script also runs anything listed in
`~/.config/ztmetro/desktop-autostart`. The shell starts it with these set:

- `ZT_DESKTOP=1`
- `XDG_CURRENT_DESKTOP=XFCE`
- `WAYLAND_DISPLAY`
- `DISPLAY` (Xwayland's)
- `ZT_DESKTOP_OUTPUT`

The shell stops it with SIGTERM to its process group. It starts:

- from the control pipe's `desktop start`;
- from Settings → "desktop mode";
- from ztdesk's start button;
- by itself when a monitor comes, if "Start the desktop automatically" is on
  (and on the virtual output, if that is on too).

There is no xfce4-session: none of its parts are needed, and it expects to own
the session.

## When the monitor goes

The setting is `unplug` in `~/.config/ztmetro/desktop`, on the Settings page:

- **keep** (the default): the desktop moves to a virtual output of the same
  size. Its windows keep running there and can be seen in ztdesk.
- **phone**: xdg windows go to Metro. X11 windows and the session stay on the
  virtual output, since Metro can't draw X11 windows.

## DP hotplug (for feature/dp-altmode)

A DRM connector that becomes connected is a new `wlr_output` from wlroots' DRM
backend. That is exactly `desk_new_output`, so the DP work needs nothing from
this side. To leave room for anything it does need (a mode quirk, a log), the
shell runs **`ztos-desktop-hotplug add|remove CONNECTOR`** (if installed)
whenever a monitor output comes or goes. The stub is in this directory.

## Packages and cost

`packages/ztos-desktop` is optional, like `ztos-waydroid`. It depends on:

- xfce4-panel, xfdesktop, xfce4-settings, xfconf, thunar, xfce4-terminal,
  garcon, tumbler, gtk-layer-shell, adwaita-icon-theme;
- xorg-xwayland;
- mesa-utils, glmark2, vulkan-panfrost and vulkan-tools (for the checks).

The download is about 27 MB, about 110 MB installed. RAM is measured in
`README.md`.

## The GPU

Everything on the desktop is GPU-drawn where the toolkit draws with the GPU.
Measured on the S9 (Mali-G72 MC18, Mesa 26.2.6 Panfrost):

- The compositor draws the desktop output (headless or a monitor) with wlroots'
  GLES2 renderer on Panfrost, and the shell's log names the renderer. Nothing
  falls back to llvmpipe or softpipe.
- Panfrost gives desktop OpenGL 3.1 (core and compatibility) and GLES 3.1 on
  this Bifrost GPU. No `MESA_GL_VERSION_OVERRIDE` is needed or set.
- Xwayland uses glamor. GLX is direct rendering on Panfrost (`glxinfo -B`).
- Vulkan through panvk refuses Bifrost (v6) unless the variable
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is set. With it, it reports Vulkan
  1.0, non-conformant, and vkcube runs. Nothing we ship sets it.
- GTK 3 (XFCE, Thunar) draws its widgets with cairo on the CPU, as it does
  everywhere. The compositor shows its buffers as GPU textures. GTK 4 apps
  would use GL.
- ztdesk's view is a DMA-BUF that the compositor copies into on the GPU. It is
  drawn as a texture, and shared memory is the fallback.

## Files

| File | What |
|---|---|
| `desktop/desktop.c`, `desktop.h` | the desktop in the shell |
| `desktop/protocols/` | the protocol XML used (layer-shell for the shell; virtual pointer, virtual keyboard and screencopy for ztdesk) |
| `desktop/ztos-desktop-session` | the XFCE session |
| `desktop/ztos-desktop-hotplug` | the DP hook stub |
| `desktop/ztdesk.c` | the controller app |
| `desktop/dev/` | a container that runs the whole thing headless on a PC |
