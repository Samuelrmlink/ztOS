// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Desktop mode: a second output, beside the phone's panel, run as a stacking
 * desktop (windows, a pointer, layer-shell panels) in the same Wayland session
 * as Metro. See desktop/DESIGN.md. The code is desktop/desktop.c, included at
 * the end of ztmetro.c so it can use the shell's own helpers; these are the
 * calls ztmetro.c makes into it.
 */
#ifndef ZT_DESKTOP_H
#define ZT_DESKTOP_H

#include <stdbool.h>

struct zt_server;
struct zt_toplevel;
struct zt_desk;
struct desk_view;
struct wlr_compositor;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct wlr_output;
struct wlr_surface;
struct wlr_xdg_popup;

/* main(): after the xdg-shell and before the backend starts */
static void desk_init(struct zt_server *server, struct wlr_compositor *compositor);
/* main(): once WAYLAND_DISPLAY is known (the session can be started from here on) */
static void desk_session_ready(struct zt_server *server);
/* server_new_output(): every output after the panel is the desktop's */
static void desk_new_output(struct zt_server *server, struct wlr_output *output);

/* the xdg toplevel's own listeners hand over to the desktop while it is
 * there: each returns true when the desktop has dealt with the event */
static bool desk_toplevel_commit(struct zt_toplevel *toplevel);
static bool desk_toplevel_map(struct zt_toplevel *toplevel);
static bool desk_toplevel_unmap(struct zt_toplevel *toplevel);
static void desk_toplevel_destroy(struct zt_toplevel *toplevel);
static bool desk_toplevel_fullscreen(struct zt_toplevel *toplevel);
/* a popup whose parent is on the desktop */
static bool desk_new_popup(struct zt_server *server, struct wlr_xdg_popup *popup);
/* a surface drawn on the desktop, not by Metro (its commits don't wake the panel) */
static bool desk_surface(struct zt_server *server, struct wlr_surface *surface);

/* input */
static bool desk_owns_pointers(struct zt_server *server);
static void desk_new_input(struct zt_server *server, struct wlr_input_device *device);
static bool desk_keyboard_live(struct zt_server *server);
static bool desk_key(struct zt_server *server, struct wlr_keyboard *keyboard,
	const struct wlr_keyboard_key_event *event);
static void desk_app_shown(struct zt_server *server, struct zt_toplevel *toplevel);
static void desk_phone_touched(struct zt_server *server);
/* the phone locked or unlocked: the desktop locks with it once no dock is
 * there, and stays locked until the phone is unlocked */
static void desk_phone_lock(struct zt_server *server, bool locked);
/* the phone's screen went on or off, or the phone drew a frame: whether anyone
 * sees the desktop (ztdesk in front on a lit screen) may have changed */
static void desk_phone_view(struct zt_server *server);

/* the control pipe's "desktop ..." lines, and SIGHUP */
static void desk_ctl(struct zt_server *server, const char *args);
static void desk_reload(struct zt_server *server);

/* Metro's "move to desktop" (the switcher's menu, the control pipe) */
static bool desk_available(struct zt_server *server);
static bool desk_take_from_phone(struct zt_server *server, struct zt_toplevel *toplevel);

#endif
