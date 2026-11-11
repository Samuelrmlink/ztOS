// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Desktop mode: every output after the phone's panel is a desktop, run here
 * in the same Wayland session as Metro (see desktop/DESIGN.md).
 *
 * Metro draws the panel itself, in its own GL. The desktop is drawn by
 * wlroots' scene graph instead, on the same renderer (the GPU): windows that
 * stack, a pointer, title bars for the apps that let us draw them, and
 * layer-shell surfaces for the panel and the desktop's background (XFCE's
 * xfce4-panel and xfdesktop). X11 apps come through Xwayland, started the
 * first time one connects.
 *
 * A window is Metro's or the desktop's, never both: an xdg toplevel on the
 * desktop is taken off Metro's list (server->toplevels), so nothing in Metro
 * sees it, and its xdg listeners, which are Metro's, hand over to the
 * functions here. Moving it back to the phone puts it on the list again.
 *
 * With no monitor there can still be a desktop: a virtual output on wlroots'
 * headless backend ("desktop add" on the control pipe), seen and driven from
 * the phone through the ztdesk app (screencopy, virtual pointer and keyboard).
 *
 * Included at the end of ztmetro.c, not built on its own.
 */
#include <wlr/backend/headless.h>
#include <wlr/backend/drm.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#define DESK_TITLE_H	30	/* the title bar we draw, points */
#define DESK_BUTTON_W	46	/* each of its buttons */
#define DESK_EDGE	8	/* where a title-barred window can be taken to resize, outside it */
#define DESK_MENU_W	200
#define DESK_MENU_ROW	34
#define DESK_ACTIVE_MS	10000	/* a window that maps this soon after desktop input opens there */

enum desk_layer_index { DL_BACKGROUND, DL_BOTTOM, DL_TOP, DL_OVERLAY, DL_COUNT };
enum desk_grab { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE };
enum desk_button { DB_NONE = -1, DB_PHONE, DB_MINIMIZE, DB_MAXIMIZE, DB_CLOSE, DB_COUNT };
enum desk_menu_item { DM_PHONE, DM_MINIMIZE, DM_MAXIMIZE, DM_CLOSE, DM_COUNT };
/* what happens to the desktop's windows when its monitor goes */
enum desk_unplug { UNPLUG_KEEP, UNPLUG_PHONE };

struct desk_output {
	struct wl_list link;		/* zt_desk.outputs */
	struct zt_desk *desk;
	struct wlr_output *wo;
	struct wlr_scene_output *so;
	bool virtual;			/* on the headless backend */
	struct wl_listener frame, request_state, destroy;
};

struct desk_view {
	struct wl_list link;		/* zt_desk.views, front first */
	struct zt_desk *desk;
	struct zt_toplevel *top;	/* an xdg toplevel (Metro's struct), or */
#if WLR_HAS_XWAYLAND
	struct wlr_xwayland_surface *xs;	/* an X11 window */
#endif
	struct wlr_scene_tree *tree;	/* the whole window: title bar and content */
	struct wlr_scene_tree *content;
	struct wlr_scene_buffer *title;	/* NULL: the app draws its own */
	bool mapped, minimized, maximized, fullscreen, placed;
	int x, y;			/* the window's corner (its title bar's), layout points */
	int w, h;			/* the content's size */
	struct wlr_box saved;		/* where it was before maximising or full screen */
	int hover;			/* the title bar's button under the pointer */
	int title_drawn_w;		/* what the title bar was last drawn for */
	bool title_drawn_focus;
	int title_drawn_hover;
	char title_drawn[128];
	struct wlr_foreign_toplevel_handle_v1 *ft;
	struct wl_listener ft_activate, ft_minimize, ft_maximize, ft_fullscreen, ft_close;
	/* xdg: the requests the desktop answers (Metro's listeners do map, commit…) */
	struct wl_listener req_move, req_resize, req_maximize, req_minimize, req_menu,
		set_title, set_app_id;
#if WLR_HAS_XWAYLAND
	struct wl_listener x_destroy, x_associate, x_dissociate, x_map, x_unmap, x_commit,
		x_configure, x_activate, x_move, x_resize, x_maximize, x_minimize, x_fullscreen,
		x_title, x_geometry;
	bool x_associated;
#endif
};

struct desk_layer {
	struct wl_list link;		/* zt_desk.layers */
	struct zt_desk *desk;
	struct wlr_layer_surface_v1 *ls;
	struct wlr_scene_layer_surface_v1 *scene;
	bool mapped;
	struct wl_listener map, unmap, commit, destroy, new_popup;
};

struct desk_popup {
	struct wl_list link;		/* zt_desk.popups */
	struct zt_desk *desk;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *wrap;	/* a layer surface's popup: a tree of its own, above the windows */
	struct wl_listener commit, destroy;
};

struct desk_vkbd {
	struct zt_desk *desk;
	struct wlr_virtual_keyboard_v1 *vk;
	struct wl_listener key, modifiers, destroy;
};

struct desk_pointer {
	struct zt_desk *desk;
	struct wlr_input_device *device;
	struct wl_listener destroy;
};

struct zt_desk {
	struct zt_server *server;
	struct wlr_backend *headless;
	struct wl_list outputs;		/* desk_output */
	struct desk_output *out;	/* the one the desktop is on; NULL: no desktop */
	struct wlr_output_layout *layout;	/* the desktop's outputs only: the pointer's world */
	struct wlr_scene *scene;
	struct wlr_scene_rect *backdrop;	/* under everything, for when nothing draws a background */
	struct wlr_scene_tree *layer_tree[DL_COUNT];
	struct wlr_scene_tree *win_tree, *full_tree, *x_or_tree, *popup_tree, *menu_tree, *drag_tree;
	struct wlr_box usable;		/* the output less the panels' exclusive zones, layout points */
	struct wl_list views;		/* desk_view, front first */
	struct wl_list layers;		/* desk_layer */
	struct wl_list popups;		/* desk_popup */
	struct desk_view *focused;
	struct wlr_layer_surface_v1 *focused_layer;
	int cascade;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor;
	const char *cursor_name;	/* what the compositor last set, NULL: the client's */
	struct wlr_virtual_pointer_manager_v1 *vptr_mgr;
	struct wlr_virtual_keyboard_manager_v1 *vkbd_mgr;
	struct wlr_foreign_toplevel_manager_v1 *ft_mgr;
	struct wlr_cursor_shape_manager_v1 *shape_mgr;
	struct wlr_primary_selection_v1_device_manager *primary_mgr;
#if WLR_HAS_XWAYLAND
	struct wlr_xwayland *xwayland;
	struct wl_listener x_ready, x_new_surface;
#endif

	/* the pointer's grab of a window */
	enum desk_grab grab;
	struct desk_view *grab_view;
	double grab_x, grab_y;		/* where the pointer took hold, layout */
	struct wlr_box grab_box;	/* and the window then (content size) */
	uint32_t grab_edges;
	uint32_t buttons_down;

	/* the window menu (right-click on a title bar, Alt+Space, an app's own) */
	struct desk_view *menu_view;
	struct wlr_scene_buffer *menu;
	int menu_x, menu_y, menu_hover;

	/* what the shell knows */
	uint32_t last_input_ms;		/* the last pointer or key that went to the desktop */
	uint32_t phone_touch_ms;	/* and the last finger on the phone */
	uint32_t watch_until;		/* someone is looking at a virtual output (ztdesk, wayvnc, grim) */
	struct wl_event_source *idle_timer;	/* frames for a virtual output nobody looks at */
	pid_t session_pid;		/* ztos-desktop-session, while it runs */
	bool session_wanted;
	pid_t session_pgid;		/* its process group, kept after the script itself ends */
	pid_t session_pgid_dying;	/* a stopped session's process group, killed if it lingers */
	struct wl_event_source *session_kill_timer;
	char x_display[16];

	/* ~/.config/ztmetro/desktop (Settings' "desktop mode" page writes it) */
	bool autostart;			/* start XFCE when a monitor comes */
	bool autostart_virtual;		/* and on the virtual output too */
	enum desk_unplug unplug;
	int virt_w, virt_h, virt_hz;
	int mon_w, mon_h, mon_hz;	/* a monitor's mode (monitor_mode =); 0: its preferred */
	float scale;			/* 0: by the monitor's size */
	bool xwayland_on;

	struct wl_listener new_layer, new_vptr, new_vkbd, request_cursor, request_shape,
		request_primary, request_drag, start_drag,
		cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame;
};

static void desk_focus_view(struct zt_desk *desk, struct desk_view *view);
static void desk_arrange_layers(struct zt_desk *desk);
static void desk_title_draw(struct desk_view *view, bool force);
static void desk_menu_close(struct zt_desk *desk);
static void desk_view_place(struct desk_view *view);
static void desk_view_configure(struct desk_view *view, int x, int y, int w, int h);
static void desk_give_to_phone(struct desk_view *view);
static void desk_session_start(struct zt_desk *desk);
static void desk_session_stop(struct zt_desk *desk);
static void desk_state_write(struct zt_desk *desk);
static void desk_view_set_minimized(struct desk_view *view, bool minimized);
static void desk_view_set_maximized(struct desk_view *view, bool maximized);
static void desk_view_set_fullscreen(struct desk_view *view, bool fullscreen);
static void desk_view_close(struct desk_view *view);
static void desk_pointer_refocus(struct zt_desk *desk, uint32_t time);

/* ---- settings ------------------------------------------------------------- */

static void desk_config_load(struct zt_desk *desk) {
	desk->autostart = false;
	desk->autostart_virtual = false;
	desk->unplug = UNPLUG_KEEP;
	desk->virt_w = 1920, desk->virt_h = 1080, desk->virt_hz = 60;
	desk->mon_w = desk->mon_h = desk->mon_hz = 0;
	desk->scale = 0.0f;
	desk->xwayland_on = true;
	char path[512], line[256];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/desktop", home ? home : "");
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char key[64], value[128];
		if (sscanf(line, " %63[^= \t] = %127[^\n]", key, value) != 2) {
			continue;
		}
		bool on = strcmp(value, "1") == 0 || strcmp(value, "on") == 0 || strcmp(value, "true") == 0;
		if (strcmp(key, "autostart") == 0) {
			desk->autostart = on;
		} else if (strcmp(key, "autostart_virtual") == 0) {
			desk->autostart_virtual = on;
		} else if (strcmp(key, "unplug") == 0) {
			desk->unplug = strcmp(value, "phone") == 0 ? UNPLUG_PHONE : UNPLUG_KEEP;
		} else if (strcmp(key, "virtual_size") == 0) {
			int w, h, hz = 60;
			int n = sscanf(value, "%dx%d@%d", &w, &h, &hz);
			if (n >= 2 && w >= 640 && h >= 480 && w <= 3840 && h <= 2160) {
				desk->virt_w = w, desk->virt_h = h;
				desk->virt_hz = n == 3 && hz >= 10 && hz <= 120 ? hz : 60;
			}
		} else if (strcmp(key, "monitor_mode") == 0) {
			int w, h, hz = 0;
			int n = sscanf(value, "%dx%d@%d", &w, &h, &hz);
			if (n >= 2 && w >= 640 && h >= 480 && w <= 7680 && h <= 4320) {
				desk->mon_w = w, desk->mon_h = h;
				desk->mon_hz = n == 3 && hz > 0 && hz <= 240 ? hz : 0;
			}
		} else if (strcmp(key, "scale") == 0) {
			float s = strtof(value, NULL);
			desk->scale = s >= 1.0f && s <= 3.0f ? s : 0.0f;
		} else if (strcmp(key, "xwayland") == 0) {
			desk->xwayland_on = on;
		}
	}
	fclose(f);
}

/* ---- cairo drawings as scene buffers --------------------------------------- */

struct desk_cbuf {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void desk_cbuf_destroy(struct wlr_buffer *buffer) {
	struct desk_cbuf *c = wl_container_of(buffer, c, base);
	cairo_surface_destroy(c->surface);
	free(c);
}

static bool desk_cbuf_begin(struct wlr_buffer *buffer, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct desk_cbuf *c = wl_container_of(buffer, c, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	cairo_surface_flush(c->surface);
	*data = cairo_image_surface_get_data(c->surface);
	*format = DRM_FORMAT_ARGB8888;	/* cairo's ARGB32, premultiplied, as wlroots wants */
	*stride = cairo_image_surface_get_stride(c->surface);
	return true;
}

static void desk_cbuf_end(struct wlr_buffer *buffer) {
}

static const struct wlr_buffer_impl desk_cbuf_impl = {
	.destroy = desk_cbuf_destroy,
	.begin_data_ptr_access = desk_cbuf_begin,
	.end_data_ptr_access = desk_cbuf_end,
};

/* Hands the surface over: put the buffer in a scene buffer, then drop it. */
static struct wlr_buffer *desk_cbuf(cairo_surface_t *surface) {
	struct desk_cbuf *c = calloc(1, sizeof(*c));
	wlr_buffer_init(&c->base, &desk_cbuf_impl, cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface));
	c->surface = surface;
	return &c->base;
}

static float desk_scale(struct zt_desk *desk) {
	return desk->out ? desk->out->wo->scale : 1.0f;
}

static void desk_text(cairo_t *cr, const char *font, double size, const char *text,
		double x, double y, double max_w, bool centre) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, fd);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
	pango_layout_set_single_paragraph_mode(layout, true);
	int lw, lh;
	pango_layout_get_pixel_size(layout, &lw, &lh);
	cairo_move_to(cr, centre ? x + (max_w - lw) / 2.0 : x, y - lh / 2.0);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(fd);
	g_object_unref(layout);
}

/* ---- views: where they are, what they are ----------------------------------- */

static struct wlr_surface *desk_view_surface(struct desk_view *view) {
	if (view->top) {
		return view->top->xdg_toplevel->base->surface;
	}
#if WLR_HAS_XWAYLAND
	if (view->xs) {
		return view->xs->surface;
	}
#endif
	return NULL;
}

static const char *desk_view_title(struct desk_view *view) {
	const char *t = NULL;
	if (view->top) {
		t = view->top->xdg_toplevel->title;
		if (!t || !t[0]) t = view->top->xdg_toplevel->app_id;
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		t = view->xs->title;
		if (!t || !t[0]) t = view->xs->class;
	}
#endif
	return t ? t : "";
}

static const char *desk_view_app_id(struct desk_view *view) {
	const char *id = NULL;
	if (view->top) id = view->top->xdg_toplevel->app_id;
#if WLR_HAS_XWAYLAND
	else if (view->xs) id = view->xs->class;
#endif
	return id ? id : "";
}

static int desk_title_h(struct desk_view *view) {
	return view->title && !view->fullscreen ? DESK_TITLE_H : 0;
}

/* Whether an xdg app takes our title bar: it said so through xdg-decoration
 * (Metro always answers "server side"). GTK and most toolkits that draw their
 * own (client-side) never ask. */
static bool desk_xdg_wants_ssd(struct zt_desk *desk, struct zt_toplevel *top) {
	struct zt_decoration *d;
	wl_list_for_each(d, &desk->server->decorations, link) {
		if (d->deco->toplevel == top->xdg_toplevel) {
			return true;
		}
	}
	return false;
}

static void desk_view_ensure_title(struct desk_view *view) {
	bool want;
	if (view->top) {
		want = desk_xdg_wants_ssd(view->desk, view->top);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		/* X11 windows have no window manager to frame them but us: framed
		 * unless they asked for none (_MOTIF_WM_HINTS) */
		want = view->xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
	}
#endif
	else {
		want = false;
	}
	if (want && !view->title) {
		view->title = wlr_scene_buffer_create(view->tree, NULL);
		view->title->node.data = view;
		view->title_drawn_w = -1;
	} else if (!want && view->title) {
		wlr_scene_node_destroy(&view->title->node);
		view->title = NULL;
	}
	if (view->content) {
		wlr_scene_node_set_position(&view->content->node, 0, desk_title_h(view));
	}
}

static void desk_view_geometry(struct desk_view *view, int *w, int *h) {
	*w = view->w, *h = view->h;
	if (view->top) {
		struct wlr_box *g = &view->top->xdg_toplevel->base->geometry;
		*w = g->width, *h = g->height;
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		*w = view->xs->width, *h = view->xs->height;
	}
#endif
}

/* The view's frame (title bar and content), layout points. */
static struct wlr_box desk_view_frame(struct desk_view *view) {
	return (struct wlr_box){ view->x, view->y, view->w, view->h + desk_title_h(view) };
}

static void desk_view_move(struct desk_view *view, int x, int y) {
	view->x = x, view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
#if WLR_HAS_XWAYLAND
	if (view->xs) {
		/* X11 clients place their menus from where they think they are */
		wlr_xwayland_surface_configure(view->xs, x, y + desk_title_h(view), view->w, view->h);
	}
#endif
}

/* Ask for a size (content), and put the frame's corner at x, y. */
static void desk_view_configure(struct desk_view *view, int x, int y, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	view->x = x, view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	if (view->top) {
		wlr_xdg_toplevel_set_size(view->top->xdg_toplevel, w, h);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		view->w = w, view->h = h;
		wlr_xwayland_surface_configure(view->xs, x, y + desk_title_h(view), w, h);
	}
#endif
}

static struct desk_view *desk_view_at(struct zt_desk *desk, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy, struct wlr_layer_surface_v1 **layer) {
	*surface = NULL;
	if (layer) *layer = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&desk->scene->tree.node, lx, ly, sx, sy);
	if (!node) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss) {
			*surface = ss->surface;
		}
	}
	/* up to the tree that says what it belongs to */
	for (struct wlr_scene_tree *t = node->parent; t; t = t->node.parent) {
		if (!t->node.data) {
			continue;
		}
		struct desk_layer *dl;
		wl_list_for_each(dl, &desk->layers, link) {
			if (dl->scene && t == dl->scene->tree) {
				if (layer) *layer = dl->ls;
				return NULL;
			}
		}
		struct desk_view *v;
		wl_list_for_each(v, &desk->views, link) {
			if (t == v->tree) {
				return v;
			}
		}
	}
	if (node->data) {
		struct desk_view *v;
		wl_list_for_each(v, &desk->views, link) {
			if (node->data == v) {
				return v;
			}
		}
	}
	return NULL;
}

/* ---- title bars --------------------------------------------------------------- */

static void desk_glyph(cairo_t *cr, int which, double cx, double cy, bool maximized) {
	cairo_set_line_width(cr, 1.0);
	switch (which) {
	case DB_PHONE:	/* a phone: the window goes there */
		cairo_rectangle(cr, cx - 4.5, cy - 7.5, 9, 15);
		cairo_stroke(cr);
		cairo_rectangle(cr, cx - 1.5, cy + 4.5, 3, 1);
		cairo_fill(cr);
		break;
	case DB_MINIMIZE:
		cairo_rectangle(cr, cx - 5, cy, 10, 1);
		cairo_fill(cr);
		break;
	case DB_MAXIMIZE:
		if (maximized) {
			cairo_rectangle(cr, cx - 4.5, cy - 2.5, 7, 7);
			cairo_move_to(cr, cx - 2.5, cy - 2.5);
			cairo_line_to(cr, cx - 2.5, cy - 4.5);
			cairo_line_to(cr, cx + 4.5, cy - 4.5);
			cairo_line_to(cr, cx + 4.5, cy + 2.5);
			cairo_line_to(cr, cx + 2.5, cy + 2.5);
		} else {
			cairo_rectangle(cr, cx - 4.5, cy - 4.5, 9, 9);
		}
		cairo_stroke(cr);
		break;
	case DB_CLOSE:
		cairo_move_to(cr, cx - 5, cy - 5);
		cairo_line_to(cr, cx + 5, cy + 5);
		cairo_move_to(cr, cx + 5, cy - 5);
		cairo_line_to(cr, cx - 5, cy + 5);
		cairo_stroke(cr);
		break;
	}
}

/* An X11 window can't go to the phone: Metro draws xdg toplevels only. */
static bool desk_view_can_go_to_phone(struct desk_view *view) {
	return view->top != NULL;
}

static int desk_title_button_x(struct desk_view *view, int which) {
	/* right to left: close, maximise, minimise, phone */
	return view->w - (DB_COUNT - which) * DESK_BUTTON_W;
}

static int desk_title_button_at(struct desk_view *view, double x) {
	for (int b = 0; b < DB_COUNT; b++) {
		if (b == DB_PHONE && !desk_view_can_go_to_phone(view)) {
			continue;
		}
		int bx = desk_title_button_x(view, b);
		if (x >= bx && x < bx + DESK_BUTTON_W) {
			return b;
		}
	}
	return DB_NONE;
}

static void desk_title_draw(struct desk_view *view, bool force) {
	if (!view->title) {
		return;
	}
	struct zt_desk *desk = view->desk;
	bool focus = desk->focused == view;
	const char *title = desk_view_title(view);
	if (!force && view->title_drawn_w == view->w && view->title_drawn_focus == focus &&
			view->title_drawn_hover == view->hover && !strncmp(view->title_drawn, title, sizeof(view->title_drawn) - 1)) {
		return;
	}
	view->title_drawn_w = view->w;
	view->title_drawn_focus = focus;
	view->title_drawn_hover = view->hover;
	snprintf(view->title_drawn, sizeof(view->title_drawn), "%s", title);
	if (view->w <= 0) {
		return;
	}
	float s = desk_scale(desk);
	int pw = (int)ceilf(view->w * s), ph = (int)ceilf(DESK_TITLE_H * s);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, s, s);
	uint32_t accent = desk->server->accent;
	if (focus) {
		cairo_set_source_rgb(cr, ((accent >> 16) & 255) / 255.0, ((accent >> 8) & 255) / 255.0,
			(accent & 255) / 255.0);
	} else {
		cairo_set_source_rgb(cr, 0.17, 0.17, 0.17);
	}
	cairo_paint(cr);
	double fg = focus ? 1.0 : 0.62;
	cairo_set_source_rgb(cr, fg, fg, fg);
	int buttons_w = DESK_BUTTON_W * (desk_view_can_go_to_phone(view) ? DB_COUNT : DB_COUNT - 1);
	desk_text(cr, FONT_REGULAR, 13.0, title, 12, DESK_TITLE_H / 2.0, view->w - 24 - buttons_w, false);
	for (int b = 0; b < DB_COUNT; b++) {
		if (b == DB_PHONE && !desk_view_can_go_to_phone(view)) {
			continue;
		}
		int bx = desk_title_button_x(view, b);
		if (view->hover == b) {
			if (b == DB_CLOSE) {
				cairo_set_source_rgb(cr, 0.91, 0.07, 0.14);
			} else {
				cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
			}
			cairo_rectangle(cr, bx, 0, DESK_BUTTON_W, DESK_TITLE_H);
			cairo_fill(cr);
		}
		double c = view->hover == b ? 1.0 : fg;
		cairo_set_source_rgb(cr, c, c, c);
		desk_glyph(cr, b, bx + DESK_BUTTON_W / 2.0 + 0.5, DESK_TITLE_H / 2.0 + 0.5, view->maximized);
	}
	cairo_destroy(cr);
	struct wlr_buffer *buffer = desk_cbuf(surface);
	wlr_scene_buffer_set_buffer(view->title, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(view->title, view->w, DESK_TITLE_H);
}

/* ---- the window menu ------------------------------------------------------------ */

static const char *desk_menu_label(struct desk_view *view, int item) {
	switch (item) {
	case DM_PHONE: return "move to phone";
	case DM_MINIMIZE: return "minimize";
	case DM_MAXIMIZE: return view->maximized ? "restore" : "maximize";
	case DM_CLOSE: return "close";
	}
	return "";
}

static bool desk_menu_item_on(struct desk_view *view, int item) {
	return item != DM_PHONE || desk_view_can_go_to_phone(view);
}

static void desk_menu_draw(struct zt_desk *desk) {
	struct desk_view *view = desk->menu_view;
	float s = desk_scale(desk);
	int h = DM_COUNT * DESK_MENU_ROW + 8;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)ceilf(DESK_MENU_W * s), (int)ceilf(h * s));
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, s, s);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, DESK_MENU_W - 1, h - 1);
	cairo_stroke(cr);
	for (int i = 0; i < DM_COUNT; i++) {
		double y = 4 + i * DESK_MENU_ROW;
		bool on = desk_menu_item_on(view, i);
		if (i == desk->menu_hover && on) {
			uint32_t a = desk->server->accent;
			cairo_set_source_rgb(cr, ((a >> 16) & 255) / 255.0, ((a >> 8) & 255) / 255.0, (a & 255) / 255.0);
			cairo_rectangle(cr, 1, y, DESK_MENU_W - 2, DESK_MENU_ROW);
			cairo_fill(cr);
		}
		double c = on ? 1.0 : 0.4;
		cairo_set_source_rgb(cr, c, c, c);
		desk_text(cr, FONT_REGULAR, 14.0, desk_menu_label(view, i), 14, y + DESK_MENU_ROW / 2.0,
			DESK_MENU_W - 28, false);
	}
	cairo_destroy(cr);
	struct wlr_buffer *buffer = desk_cbuf(surface);
	wlr_scene_buffer_set_buffer(desk->menu, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(desk->menu, DESK_MENU_W, h);
}

static void desk_menu_open(struct zt_desk *desk, struct desk_view *view, int x, int y) {
	desk_menu_close(desk);
	if (!desk->out) {
		return;
	}
	struct wlr_box o;
	wlr_output_layout_get_box(desk->layout, desk->out->wo, &o);
	int h = DM_COUNT * DESK_MENU_ROW + 8;
	if (x + DESK_MENU_W > o.x + o.width) x = o.x + o.width - DESK_MENU_W;
	if (y + h > o.y + o.height) y = o.y + o.height - h;
	if (x < o.x) x = o.x;
	if (y < o.y) y = o.y;
	desk->menu_view = view;
	desk->menu_x = x, desk->menu_y = y;
	desk->menu_hover = -1;
	desk->menu = wlr_scene_buffer_create(desk->menu_tree, NULL);
	wlr_scene_node_set_position(&desk->menu->node, x, y);
	desk_menu_draw(desk);
}

static void desk_menu_close(struct zt_desk *desk) {
	if (desk->menu) {
		wlr_scene_node_destroy(&desk->menu->node);
		desk->menu = NULL;
	}
	desk->menu_view = NULL;
}

static int desk_menu_item_at(struct zt_desk *desk, double lx, double ly) {
	int h = DM_COUNT * DESK_MENU_ROW + 8;
	if (lx < desk->menu_x || lx >= desk->menu_x + DESK_MENU_W || ly < desk->menu_y || ly >= desk->menu_y + h) {
		return -2;	/* outside */
	}
	int i = (int)((ly - desk->menu_y - 4) / DESK_MENU_ROW);
	return i >= 0 && i < DM_COUNT ? i : -1;
}

static void desk_menu_choose(struct zt_desk *desk, int item) {
	struct desk_view *view = desk->menu_view;
	desk_menu_close(desk);
	if (!view || !desk_menu_item_on(view, item)) {
		return;
	}
	switch (item) {
	case DM_PHONE: desk_give_to_phone(view); break;
	case DM_MINIMIZE: desk_view_set_minimized(view, true); break;
	case DM_MAXIMIZE: desk_view_set_maximized(view, !view->maximized); break;
	case DM_CLOSE: desk_view_close(view); break;
	}
}

/* ---- focus -------------------------------------------------------------------- */

static void desk_keyboard_enter(struct zt_desk *desk, struct wlr_surface *surface) {
	struct wlr_seat *seat = desk->server->seat;
	if (!surface || seat->keyboard_state.focused_surface == surface) {
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
	text_inputs_focus(desk->server, surface);
}

static void desk_view_activate(struct desk_view *view, bool on) {
	if (view->top) {
		wlr_xdg_toplevel_set_activated(view->top->xdg_toplevel, on);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		wlr_xwayland_surface_activate(view->xs, on);
	}
#endif
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_activated(view->ft, on);
	}
}

static struct desk_view *desk_top_view(struct zt_desk *desk, struct desk_view *except) {
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		if (v != except && v->mapped && !v->minimized) {
			return v;
		}
	}
	return NULL;
}

static void desk_focus_view(struct zt_desk *desk, struct desk_view *view) {
	struct desk_view *before = desk->focused;
	if (view && view->minimized) {
		desk_view_set_minimized(view, false);
		return;	/* (which focuses it) */
	}
	if (view) {
		wl_list_remove(&view->link);
		wl_list_insert(&desk->views, &view->link);
		wlr_scene_node_raise_to_top(&view->tree->node);
#if WLR_HAS_XWAYLAND
		if (view->xs) {
			wlr_xwayland_surface_restack(view->xs, NULL, XCB_STACK_MODE_ABOVE);
		}
#endif
	}
	if (before != view) {
		desk->focused = view;
		if (before) {
			desk_view_activate(before, false);
			desk_title_draw(before, false);
		}
		if (view) {
			desk_view_activate(view, true);
			desk_title_draw(view, false);
		}
	}
	if (view && !desk->focused_layer) {
		desk_keyboard_enter(desk, desk_view_surface(view));
	}
}

/* The desktop takes the keyboard: its window in front has it from now on (a
 * virtual keyboard's key, a click on the desktop). Metro takes it back when it
 * shows an app (show_toplevel) or the phone is touched (desk_phone_touched). */
static void desk_take_keyboard(struct zt_desk *desk) {
	desk->last_input_ms = now_ms();
	if (desk->focused_layer && desk->focused_layer->surface->mapped) {
		desk_keyboard_enter(desk, desk->focused_layer->surface);
	} else if (desk->focused && desk->focused->mapped) {
		desk_keyboard_enter(desk, desk_view_surface(desk->focused));
	}
}

static bool desk_has_keyboard(struct zt_desk *desk) {
	struct wlr_surface *s = desk->server->seat->keyboard_state.focused_surface;
	return s && desk_surface(desk->server, s);
}

/* ---- view state ----------------------------------------------------------------- */

static void desk_view_set_minimized(struct desk_view *view, bool minimized) {
	struct zt_desk *desk = view->desk;
	if (view->minimized == minimized) {
		if (!minimized) desk_focus_view(desk, view);
		return;
	}
	view->minimized = minimized;
	wlr_scene_node_set_enabled(&view->tree->node, !minimized);
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->ft, minimized);
	}
	if (view->top) {
		/* hidden: it can stop drawing (xdg-shell's suspended) */
		wlr_xdg_toplevel_set_suspended(view->top->xdg_toplevel, minimized);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		wlr_xwayland_surface_set_minimized(view->xs, minimized);
	}
#endif
	if (minimized) {
		if (desk->focused == view) {
			desk_view_activate(view, false);
			desk->focused = NULL;
			struct desk_view *next = desk_top_view(desk, view);
			if (next) {
				desk_focus_view(desk, next);
			} else if (desk_has_keyboard(desk)) {
				wlr_seat_keyboard_notify_clear_focus(desk->server->seat);
			}
		}
	} else {
		desk_focus_view(desk, view);
	}
}

static void desk_view_set_maximized(struct desk_view *view, bool maximized) {
	struct zt_desk *desk = view->desk;
	if (view->fullscreen || view->maximized == maximized) {
		if (view->top && view->top->xdg_toplevel->base->initialized) {
			wlr_xdg_surface_schedule_configure(view->top->xdg_toplevel->base);
		}
		return;
	}
	view->maximized = maximized;
	if (view->top) {
		wlr_xdg_toplevel_set_maximized(view->top->xdg_toplevel, maximized);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		wlr_xwayland_surface_set_maximized(view->xs, maximized, maximized);
	}
#endif
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_maximized(view->ft, maximized);
	}
	if (maximized) {
		view->saved = (struct wlr_box){ view->x, view->y, view->w, view->h };
		struct wlr_box *u = &desk->usable;
		desk_view_configure(view, u->x, u->y, u->width, u->height - desk_title_h(view));
	} else {
		struct wlr_box *s = &view->saved;
		desk_view_configure(view, s->x, s->y, s->width, s->height);
	}
	desk_title_draw(view, true);
}

static void desk_view_set_fullscreen(struct desk_view *view, bool fullscreen) {
	struct zt_desk *desk = view->desk;
	if (view->fullscreen == fullscreen || !desk->out) {
		return;
	}
	if (fullscreen && !view->maximized) {
		view->saved = (struct wlr_box){ view->x, view->y, view->w, view->h };
	}
	view->fullscreen = fullscreen;
	if (view->title) {
		wlr_scene_node_set_enabled(&view->title->node, !fullscreen);
	}
	wlr_scene_node_set_position(&view->content->node, 0, desk_title_h(view));
	wlr_scene_node_reparent(&view->tree->node, fullscreen ? desk->full_tree : desk->win_tree);
	if (view->top) {
		wlr_xdg_toplevel_set_fullscreen(view->top->xdg_toplevel, fullscreen);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		wlr_xwayland_surface_set_fullscreen(view->xs, fullscreen);
	}
#endif
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ft, fullscreen);
	}
	if (fullscreen) {
		struct wlr_box o;
		wlr_output_layout_get_box(desk->layout, desk->out->wo, &o);
		desk_view_configure(view, o.x, o.y, o.width, o.height);
	} else if (view->maximized) {
		struct wlr_box *u = &desk->usable;
		desk_view_configure(view, u->x, u->y, u->width, u->height - desk_title_h(view));
	} else {
		struct wlr_box *s = &view->saved;
		desk_view_configure(view, s->x, s->y, s->width, s->height);
	}
	desk_focus_view(desk, view);
}

static void desk_view_close(struct desk_view *view) {
	if (view->top) {
		wlr_xdg_toplevel_send_close(view->top->xdg_toplevel);
	}
#if WLR_HAS_XWAYLAND
	else if (view->xs) {
		wlr_xwayland_surface_close(view->xs);
	}
#endif
}

/* Where a new window goes: in the middle of what is free, a step down and
 * along from the last one, and never with its title bar out of reach. */
static void desk_view_place(struct desk_view *view) {
	struct zt_desk *desk = view->desk;
	struct wlr_box *u = &desk->usable;
	int w = view->w, h = view->h + desk_title_h(view);
	if (w > u->width) w = u->width;
	if (h > u->height) h = u->height;
	int step = 28 * (desk->cascade++ % 8);
	int x = u->x + (u->width - w) / 2 + step - 3 * 28 * (desk->cascade > 4);
	int y = u->y + (u->height - h) / 3 + step;
	if (x + w > u->x + u->width) x = u->x + u->width - w;
	if (y + h > u->y + u->height) y = u->y + u->height - h;
	if (x < u->x) x = u->x;
	if (y < u->y) y = u->y;
	desk_view_move(view, x, y);
	view->placed = true;
}

/* ---- foreign-toplevel: xfce4-panel's window buttons ------------------------------- */

static void desk_ft_activate(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, ft_activate);
	view->desk->focused_layer = NULL;
	desk_focus_view(view->desk, view);
	desk_take_keyboard(view->desk);
}

static void desk_ft_minimize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, ft_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *e = data;
	desk_view_set_minimized(view, e->minimized);
}

static void desk_ft_maximize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, ft_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *e = data;
	desk_view_set_maximized(view, e->maximized);
}

static void desk_ft_fullscreen(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, ft_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *e = data;
	desk_view_set_fullscreen(view, e->fullscreen);
}

static void desk_ft_close(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, ft_close);
	desk_view_close(view);
}

static void desk_ft_create(struct desk_view *view) {
	struct zt_desk *desk = view->desk;
	view->ft = wlr_foreign_toplevel_handle_v1_create(desk->ft_mgr);
	wlr_foreign_toplevel_handle_v1_set_title(view->ft, desk_view_title(view));
	wlr_foreign_toplevel_handle_v1_set_app_id(view->ft, desk_view_app_id(view));
	if (desk->out) {
		wlr_foreign_toplevel_handle_v1_output_enter(view->ft, desk->out->wo);
	}
	view->ft_activate.notify = desk_ft_activate;
	wl_signal_add(&view->ft->events.request_activate, &view->ft_activate);
	view->ft_minimize.notify = desk_ft_minimize;
	wl_signal_add(&view->ft->events.request_minimize, &view->ft_minimize);
	view->ft_maximize.notify = desk_ft_maximize;
	wl_signal_add(&view->ft->events.request_maximize, &view->ft_maximize);
	view->ft_fullscreen.notify = desk_ft_fullscreen;
	wl_signal_add(&view->ft->events.request_fullscreen, &view->ft_fullscreen);
	view->ft_close.notify = desk_ft_close;
	wl_signal_add(&view->ft->events.request_close, &view->ft_close);
}

static void desk_ft_destroy(struct desk_view *view) {
	if (!view->ft) {
		return;
	}
	wl_list_remove(&view->ft_activate.link);
	wl_list_remove(&view->ft_minimize.link);
	wl_list_remove(&view->ft_maximize.link);
	wl_list_remove(&view->ft_fullscreen.link);
	wl_list_remove(&view->ft_close.link);
	wlr_foreign_toplevel_handle_v1_destroy(view->ft);
	view->ft = NULL;
}

/* ---- interactive move and resize ------------------------------------------------------ */

static void desk_grab_begin(struct zt_desk *desk, struct desk_view *view, enum desk_grab mode,
		uint32_t edges) {
	if (view->fullscreen) {
		return;
	}
	if (view->maximized && mode == GRAB_MOVE) {
		/* dragging a maximised window by its title takes it out, under the pointer */
		double fx = (desk->cursor->x - view->x) / (view->w > 0 ? view->w : 1);
		desk_view_set_maximized(view, false);
		int x = (int)(desk->cursor->x - fx * view->saved.width);
		desk_view_move(view, x, (int)desk->cursor->y - DESK_TITLE_H / 2);
	} else if (view->maximized) {
		return;
	}
	desk_focus_view(desk, view);
	desk->grab = mode;
	desk->grab_view = view;
	desk->grab_x = desk->cursor->x, desk->grab_y = desk->cursor->y;
	desk->grab_box = (struct wlr_box){ view->x, view->y, view->w, view->h };
	desk->grab_edges = edges;
	wlr_seat_pointer_clear_focus(desk->server->seat);
	if (view->top && mode == GRAB_RESIZE) {
		wlr_xdg_toplevel_set_resizing(view->top->xdg_toplevel, true);
	}
}

static void desk_grab_end(struct zt_desk *desk) {
	if (desk->grab == GRAB_RESIZE && desk->grab_view && desk->grab_view->top) {
		wlr_xdg_toplevel_set_resizing(desk->grab_view->top->xdg_toplevel, false);
	}
	desk->grab = GRAB_NONE;
	desk->grab_view = NULL;
}

static void desk_grab_motion(struct zt_desk *desk) {
	struct desk_view *view = desk->grab_view;
	double dx = desk->cursor->x - desk->grab_x, dy = desk->cursor->y - desk->grab_y;
	struct wlr_box b = desk->grab_box;
	if (desk->grab == GRAB_MOVE) {
		int y = b.y + (int)dy;
		if (y < desk->usable.y) y = desk->usable.y;	/* the title bar stays reachable */
		desk_view_move(view, b.x + (int)dx, y);
		return;
	}
	int x = b.x, y = b.y, w = b.width, h = b.height;
	const int min = 120;
	if (desk->grab_edges & WLR_EDGE_LEFT) {
		w = b.width - (int)dx;
		if (w < min) w = min;
		x = b.x + b.width - w;
	} else if (desk->grab_edges & WLR_EDGE_RIGHT) {
		w = b.width + (int)dx;
	}
	if (desk->grab_edges & WLR_EDGE_TOP) {
		h = b.height - (int)dy;
		if (h < min / 2) h = min / 2;
		y = b.y + b.height - h;
	} else if (desk->grab_edges & WLR_EDGE_BOTTOM) {
		h = b.height + (int)dy;
	}
	if (w < min) w = min;
	if (h < min / 2) h = min / 2;
	if (view->top) {
		/* the corner moves when the app has taken the size (desk_xdg_commit) */
		view->saved = (struct wlr_box){ x, y, w, h };
		wlr_xdg_toplevel_set_size(view->top->xdg_toplevel, w, h);
	} else {
		desk_view_configure(view, x, y, w, h);
	}
}

/* ---- the pointer ------------------------------------------------------------------------- */

static void desk_cursor_set(struct zt_desk *desk, const char *name) {
	if (desk->cursor_name != name) {
		desk->cursor_name = name;
		wlr_cursor_set_xcursor(desk->cursor, desk->xcursor, name);
	}
}

static uint32_t desk_edges_at(struct desk_view *view, double lx, double ly) {
	if (!view->title || view->maximized || view->fullscreen) {
		return 0;
	}
	struct wlr_box f = desk_view_frame(view);
	if (lx < f.x - DESK_EDGE || lx >= f.x + f.width + DESK_EDGE ||
			ly < f.y - DESK_EDGE || ly >= f.y + f.height + DESK_EDGE) {
		return 0;
	}
	uint32_t e = 0;
	if (lx < f.x) e |= WLR_EDGE_LEFT;
	else if (lx >= f.x + f.width) e |= WLR_EDGE_RIGHT;
	if (ly < f.y) e |= WLR_EDGE_TOP;
	else if (ly >= f.y + f.height) e |= WLR_EDGE_BOTTOM;
	return e;
}

static const char *desk_edge_cursor(uint32_t e) {
	switch (e) {
	case WLR_EDGE_TOP: return "n-resize";
	case WLR_EDGE_BOTTOM: return "s-resize";
	case WLR_EDGE_LEFT: return "w-resize";
	case WLR_EDGE_RIGHT: return "e-resize";
	case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
	case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "se-resize";
	}
	return "default";
}

/* the title-barred view whose resize border is under the pointer, front first */
static struct desk_view *desk_edge_view(struct zt_desk *desk, double lx, double ly, uint32_t *edges) {
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		if (!v->mapped || v->minimized) continue;
		struct wlr_box f = desk_view_frame(v);
		if (lx >= f.x && lx < f.x + f.width && ly >= f.y && ly < f.y + f.height) {
			return NULL;	/* inside a window in front: not an edge behind it */
		}
		uint32_t e = desk_edges_at(v, lx, ly);
		if (e) {
			*edges = e;
			return v;
		}
	}
	return NULL;
}

static void desk_set_hover(struct zt_desk *desk, struct desk_view *view, int button) {
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		int want = v == view ? button : DB_NONE;
		if (v->hover != want) {
			v->hover = want;
			desk_title_draw(v, false);
		}
	}
}

static void desk_pointer_refocus(struct zt_desk *desk, uint32_t time) {
	struct wlr_seat *seat = desk->server->seat;
	double lx = desk->cursor->x, ly = desk->cursor->y;
	if (desk->grab != GRAB_NONE) {
		desk_grab_motion(desk);
		return;
	}
	if (desk->menu) {
		int item = desk_menu_item_at(desk, lx, ly);
		if (item != -2) {
			if (item != desk->menu_hover) {
				desk->menu_hover = item;
				desk_menu_draw(desk);
			}
			desk_cursor_set(desk, "default");
			wlr_seat_pointer_clear_focus(seat);
			return;
		}
	}
	struct wlr_surface *surface;
	double sx, sy;
	struct desk_view *view = desk_view_at(desk, lx, ly, &surface, &sx, &sy, NULL);
	uint32_t edges = 0;
	struct desk_view *edge_view = !surface && (!view || !view->title || ly >= view->y + DESK_TITLE_H) ?
		desk_edge_view(desk, lx, ly, &edges) : NULL;
	if (view && !surface && view->title && ly < view->y + DESK_TITLE_H) {
		/* our title bar */
		desk_set_hover(desk, view, desk_title_button_at(view, lx - view->x));
		desk_cursor_set(desk, "default");
		wlr_seat_pointer_clear_focus(seat);
		return;
	}
	desk_set_hover(desk, NULL, DB_NONE);
	if (edge_view) {
		desk_cursor_set(desk, desk_edge_cursor(edges));
		wlr_seat_pointer_clear_focus(seat);
		return;
	}
	if (surface) {
		if (seat->pointer_state.focused_surface != surface) {
			desk->cursor_name = NULL;	/* the client sets its own on enter */
		}
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		desk_cursor_set(desk, "default");
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void desk_cursor_motion(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, cursor_motion);
	struct wlr_pointer_motion_event *e = data;
	wlr_cursor_move(desk->cursor, &e->pointer->base, e->delta_x, e->delta_y);
	desk_pointer_refocus(desk, e->time_msec);
}

static void desk_cursor_motion_abs(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, cursor_motion_abs);
	struct wlr_pointer_motion_absolute_event *e = data;
	wlr_cursor_warp_absolute(desk->cursor, &e->pointer->base, e->x, e->y);
	desk_pointer_refocus(desk, e->time_msec);
}

static void desk_cursor_button(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, cursor_button);
	struct wlr_pointer_button_event *e = data;
	struct wlr_seat *seat = desk->server->seat;
	bool pressed = e->state == WL_POINTER_BUTTON_STATE_PRESSED;
	desk->last_input_ms = now_ms();
	if (!pressed) {
		desk->buttons_down &= ~(1u << (e->button & 31));
		if (desk->grab != GRAB_NONE) {
			desk_grab_end(desk);
			desk_pointer_refocus(desk, e->time_msec);
			return;
		}
		if (desk->menu && desk_menu_item_at(desk, desk->cursor->x, desk->cursor->y) != -2) {
			int item = desk_menu_item_at(desk, desk->cursor->x, desk->cursor->y);
			if (item >= 0) desk_menu_choose(desk, item);
			return;
		}
		wlr_seat_pointer_notify_button(seat, e->time_msec, e->button, e->state);
		return;
	}
	desk->buttons_down |= 1u << (e->button & 31);
	double lx = desk->cursor->x, ly = desk->cursor->y;
	if (desk->menu) {
		int item = desk_menu_item_at(desk, lx, ly);
		if (item != -2) {
			return;	/* chosen on release */
		}
		desk_menu_close(desk);
	}
	struct wlr_surface *surface;
	double sx, sy;
	struct wlr_layer_surface_v1 *layer;
	struct desk_view *view = desk_view_at(desk, lx, ly, &surface, &sx, &sy, &layer);
	/* Alt and a drag: move (left) or resize (right) from anywhere in a window */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	bool alt = kb && (wlr_keyboard_get_modifiers(kb) & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO));
	if (view && alt && (e->button == BTN_LEFT || e->button == BTN_RIGHT)) {
		uint32_t edges = 0;
		if (e->button == BTN_RIGHT) {
			edges = (lx < view->x + view->w / 2 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
				(ly < view->y + (view->h + desk_title_h(view)) / 2 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
		}
		desk->focused_layer = NULL;
		desk_grab_begin(desk, view, e->button == BTN_LEFT ? GRAB_MOVE : GRAB_RESIZE, edges);
		return;
	}
	if (view && !surface && view->title && ly < view->y + DESK_TITLE_H) {
		desk->focused_layer = NULL;
		desk_focus_view(desk, view);
		desk_take_keyboard(desk);
		int b = desk_title_button_at(view, lx - view->x);
		if (e->button == BTN_RIGHT) {
			desk_menu_open(desk, view, (int)lx, (int)ly);
		} else if (e->button == BTN_MIDDLE) {
			desk_view_set_minimized(view, true);
		} else if (b == DB_CLOSE) {
			desk_view_close(view);
		} else if (b == DB_MAXIMIZE) {
			desk_view_set_maximized(view, !view->maximized);
		} else if (b == DB_MINIMIZE) {
			desk_view_set_minimized(view, true);
		} else if (b == DB_PHONE) {
			desk_give_to_phone(view);
		} else if (e->button == BTN_LEFT) {
			static uint32_t last_ms;
			static struct desk_view *last_view;
			if (last_view == view && e->time_msec - last_ms < 400) {
				desk_view_set_maximized(view, !view->maximized);	/* a double click */
				last_view = NULL;
			} else {
				last_view = view, last_ms = e->time_msec;
				desk_grab_begin(desk, view, GRAB_MOVE, 0);
			}
		}
		return;
	}
	uint32_t edges = 0;
	struct desk_view *edge_view = !surface ? desk_edge_view(desk, lx, ly, &edges) : NULL;
	if (edge_view && e->button == BTN_LEFT) {
		desk->focused_layer = NULL;
		desk_grab_begin(desk, edge_view, GRAB_RESIZE, edges);
		return;
	}
	if (layer) {
		if (layer->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			desk->focused_layer = layer;
			desk_keyboard_enter(desk, layer->surface);
		}
	} else if (view) {
		desk->focused_layer = NULL;
		desk_focus_view(desk, view);
		desk_take_keyboard(desk);
	}
	wlr_seat_pointer_notify_button(seat, e->time_msec, e->button, e->state);
}

static void desk_cursor_axis(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, cursor_axis);
	struct wlr_pointer_axis_event *e = data;
	desk->last_input_ms = now_ms();
	wlr_seat_pointer_notify_axis(desk->server->seat, e->time_msec, e->orientation, e->delta,
		e->delta_discrete, e->source, e->relative_direction);
}

static void desk_cursor_frame(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, cursor_frame);
	wlr_seat_pointer_notify_frame(desk->server->seat);
}

static void desk_request_cursor(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *e = data;
	if (e->seat_client == desk->server->seat->pointer_state.focused_client && desk->grab == GRAB_NONE) {
		desk->cursor_name = NULL;
		wlr_cursor_set_surface(desk->cursor, e->surface, e->hotspot_x, e->hotspot_y);
	}
}

static void desk_request_shape(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, request_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *e = data;
	if (e->seat_client == desk->server->seat->pointer_state.focused_client && desk->grab == GRAB_NONE) {
		desk->cursor_name = NULL;
		wlr_cursor_set_xcursor(desk->cursor, desk->xcursor, wlr_cursor_shape_v1_name(e->shape));
	}
}

static void desk_request_primary(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, request_primary);
	struct wlr_seat_request_set_primary_selection_event *e = data;
	wlr_seat_set_primary_selection(desk->server->seat, e->source, e->serial);
}

/* Drag and drop between the desktop's windows (Thunar's files, text) */
static void desk_request_drag(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, request_drag);
	struct wlr_seat_request_start_drag_event *e = data;
	struct wlr_seat *seat = desk->server->seat;
	if (desk->out && wlr_seat_validate_pointer_grab_serial(seat, e->origin, e->serial)) {
		wlr_seat_start_pointer_drag(seat, e->drag, e->serial);
	}
	/* anything else (a touch on the phone) is left as before: not started */
}

static void desk_start_drag(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon) {
		struct wlr_scene_tree *icon = wlr_scene_drag_icon_create(desk->drag_tree, drag->icon);
		(void)icon;
	}
}

/* ---- pointers: a mouse on the phone, and ztdesk's virtual one ------------------------------------ */

static void desk_pointer_destroy(struct wl_listener *l, void *data) {
	struct desk_pointer *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void desk_attach_pointer(struct zt_desk *desk, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(desk->cursor, device);
	if (desk->out) {
		wlr_cursor_map_input_to_output(desk->cursor, device, desk->out->wo);
	}
	struct desk_pointer *p = calloc(1, sizeof(*p));
	p->desk = desk;
	p->device = device;
	p->destroy.notify = desk_pointer_destroy;
	wl_signal_add(&device->events.destroy, &p->destroy);
}

static void desk_new_vptr(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, new_vptr);
	struct wlr_virtual_pointer_v1_new_pointer_event *e = data;
	struct wlr_input_device *device = &e->new_pointer->pointer.base;
	desk_attach_pointer(desk, device);
	wlr_log(WLR_INFO, "desktop: a virtual pointer%s", e->suggested_output ? " (absolute, on its output)" : "");
}

static void desk_vkbd_key(struct wl_listener *l, void *data) {
	struct desk_vkbd *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *e = data;
	struct wlr_seat *seat = k->desk->server->seat;
	desk_take_keyboard(k->desk);
	wlr_seat_set_keyboard(seat, &k->vk->keyboard);
	wlr_seat_keyboard_notify_key(seat, e->time_msec, e->keycode, e->state);
}

static void desk_vkbd_modifiers(struct wl_listener *l, void *data) {
	struct desk_vkbd *k = wl_container_of(l, k, modifiers);
	struct wlr_seat *seat = k->desk->server->seat;
	if (!desk_has_keyboard(k->desk)) {
		return;
	}
	wlr_seat_set_keyboard(seat, &k->vk->keyboard);
	wlr_seat_keyboard_notify_modifiers(seat, &k->vk->keyboard.modifiers);
}

static void desk_vkbd_destroy(struct wl_listener *l, void *data) {
	struct desk_vkbd *k = wl_container_of(l, k, destroy);
	struct wlr_seat *seat = k->desk->server->seat;
	if (wlr_seat_get_keyboard(seat) == &k->vk->keyboard) {
		wlr_seat_set_keyboard(seat, &k->desk->server->vkbd);	/* the shell's own keyboard again */
	}
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->destroy.link);
	free(k);
}

static void desk_new_vkbd(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, new_vkbd);
	struct wlr_virtual_keyboard_v1 *vk = data;
	struct desk_vkbd *k = calloc(1, sizeof(*k));
	k->desk = desk;
	k->vk = vk;
	k->key.notify = desk_vkbd_key;
	wl_signal_add(&vk->keyboard.events.key, &k->key);
	k->modifiers.notify = desk_vkbd_modifiers;
	wl_signal_add(&vk->keyboard.events.modifiers, &k->modifiers);
	k->destroy.notify = desk_vkbd_destroy;
	wl_signal_add(&vk->keyboard.base.events.destroy, &k->destroy);
	wlr_log(WLR_INFO, "desktop: a virtual keyboard");
}

/* ---- layer shell: XFCE's panel and desktop ------------------------------------------------ */

static void desk_arrange_layers(struct zt_desk *desk) {
	if (!desk->out) {
		return;
	}
	struct wlr_box full;
	wlr_output_layout_get_box(desk->layout, desk->out->wo, &full);
	struct wlr_box usable = full;
	/* exclusive zones first, each layer from the top down, as wlroots' examples do */
	static const enum zwlr_layer_shell_v1_layer order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND };
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < 4; i++) {
			struct desk_layer *dl;
			wl_list_for_each(dl, &desk->layers, link) {
				if (!dl->ls->initialized || dl->ls->current.layer != order[i] ||
						dl->ls->output != desk->out->wo) {
					continue;
				}
				bool exclusive = dl->ls->current.exclusive_zone > 0;
				if (exclusive != (pass == 0)) {
					continue;
				}
				wlr_scene_layer_surface_v1_configure(dl->scene, &full, &usable);
			}
		}
	}
	bool changed = memcmp(&usable, &desk->usable, sizeof(usable)) != 0;
	desk->usable = usable;
	if (changed) {
		struct desk_view *v;
		wl_list_for_each(v, &desk->views, link) {
			if (v->maximized && !v->fullscreen) {
				desk_view_configure(v, usable.x, usable.y, usable.width, usable.height - desk_title_h(v));
			}
		}
	}
}

static enum desk_layer_index desk_layer_index(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return DL_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return DL_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return DL_TOP;
	default: return DL_OVERLAY;
	}
}

static void desk_layer_commit(struct wl_listener *l, void *data) {
	struct desk_layer *dl = wl_container_of(l, dl, commit);
	struct wlr_layer_surface_v1 *ls = dl->ls;
	struct zt_desk *desk = dl->desk;
	if (!ls->initialized) {
		return;
	}
	struct wlr_scene_tree *want = desk->layer_tree[desk_layer_index(ls->current.layer)];
	if (dl->scene->tree->node.parent != want) {
		wlr_scene_node_reparent(&dl->scene->tree->node, want);
	}
	if (ls->initial_commit || ls->current.committed) {
		desk_arrange_layers(desk);
	}
}

static void desk_layer_map(struct wl_listener *l, void *data) {
	struct desk_layer *dl = wl_container_of(l, dl, map);
	dl->mapped = true;
	desk_arrange_layers(dl->desk);
	if (dl->ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		dl->desk->focused_layer = dl->ls;
		desk_keyboard_enter(dl->desk, dl->ls->surface);
	}
}

static void desk_layer_unmap(struct wl_listener *l, void *data) {
	struct desk_layer *dl = wl_container_of(l, dl, unmap);
	struct zt_desk *desk = dl->desk;
	dl->mapped = false;
	if (desk->focused_layer == dl->ls) {
		desk->focused_layer = NULL;
		if (desk->server->seat->keyboard_state.focused_surface == dl->ls->surface) {
			if (desk->focused) {
				desk_keyboard_enter(desk, desk_view_surface(desk->focused));
			} else {
				wlr_seat_keyboard_notify_clear_focus(desk->server->seat);
			}
		}
	}
	desk_arrange_layers(desk);
}

static void desk_popup_commit(struct wl_listener *l, void *data);
static void desk_popup_destroy(struct wl_listener *l, void *data);

static void desk_layer_new_popup(struct wl_listener *l, void *data) {
	struct desk_layer *dl = wl_container_of(l, dl, new_popup);
	struct wlr_xdg_popup *popup = data;
	struct zt_desk *desk = dl->desk;
	struct desk_popup *p = calloc(1, sizeof(*p));
	p->desk = desk;
	p->popup = popup;
	/* above the windows, wherever the panel is: a tree at the layer surface's place */
	p->wrap = wlr_scene_tree_create(desk->popup_tree);
	int lx = 0, ly = 0;
	wlr_scene_node_coords(&dl->scene->tree->node, &lx, &ly);
	wlr_scene_node_set_position(&p->wrap->node, lx, ly);
	popup->base->data = wlr_scene_xdg_surface_create(p->wrap, popup->base);
	p->commit.notify = desk_popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = desk_popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	wl_list_insert(&desk->popups, &p->link);
}

static void desk_layer_destroy(struct wl_listener *l, void *data) {
	struct desk_layer *dl = wl_container_of(l, dl, destroy);
	if (dl->desk->focused_layer == dl->ls) {
		dl->desk->focused_layer = NULL;
	}
	wl_list_remove(&dl->link);
	wl_list_remove(&dl->map.link);
	wl_list_remove(&dl->unmap.link);
	wl_list_remove(&dl->commit.link);
	wl_list_remove(&dl->destroy.link);
	wl_list_remove(&dl->new_popup.link);
	struct zt_desk *desk = dl->desk;
	free(dl);
	desk_arrange_layers(desk);
}

static void desk_new_layer(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, new_layer);
	struct wlr_layer_surface_v1 *ls = data;
	if (!desk->out || (ls->output && ls->output != desk->out->wo)) {
		/* Metro has no layers: the phone's panel takes none */
		wlr_log(WLR_INFO, "desktop: layer surface '%s' refused (no desktop output)",
			ls->namespace ? ls->namespace : "");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = desk->out->wo;
	struct desk_layer *dl = calloc(1, sizeof(*dl));
	dl->desk = desk;
	dl->ls = ls;
	dl->scene = wlr_scene_layer_surface_v1_create(desk->layer_tree[desk_layer_index(ls->pending.layer)], ls);
	dl->scene->tree->node.data = dl;
	ls->data = dl;
	dl->map.notify = desk_layer_map;
	wl_signal_add(&ls->surface->events.map, &dl->map);
	dl->unmap.notify = desk_layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &dl->unmap);
	dl->commit.notify = desk_layer_commit;
	wl_signal_add(&ls->surface->events.commit, &dl->commit);
	dl->destroy.notify = desk_layer_destroy;
	wl_signal_add(&ls->events.destroy, &dl->destroy);
	dl->new_popup.notify = desk_layer_new_popup;
	wl_signal_add(&ls->events.new_popup, &dl->new_popup);
	wl_list_insert(&desk->layers, &dl->link);
	wlr_log(WLR_INFO, "desktop: layer surface '%s'", ls->namespace ? ls->namespace : "");
}

/* ---- popups --------------------------------------------------------------------------------- */

/* The output, as a box in the popup's toplevel's (or layer surface's) coordinates. */
static void desk_popup_unconstrain(struct desk_popup *p) {
	struct zt_desk *desk = p->desk;
	if (!desk->out) {
		return;
	}
	struct wlr_xdg_popup *popup = p->popup;
	/* up to the root: a toplevel on the desktop, or a layer surface */
	struct wlr_surface *parent = popup->parent;
	struct wlr_xdg_popup *pp;
	while (parent && (pp = wlr_xdg_popup_try_from_wlr_surface(parent))) {
		parent = pp->parent;
	}
	int rx = 0, ry = 0;
	struct wlr_xdg_toplevel *t = parent ? wlr_xdg_toplevel_try_from_wlr_surface(parent) : NULL;
	struct wlr_layer_surface_v1 *ls = parent ? wlr_layer_surface_v1_try_from_wlr_surface(parent) : NULL;
	if (t && t->base->data) {
		struct zt_toplevel *top = t->base->data;
		if (!top->view) return;
		rx = top->view->x - t->base->geometry.x;
		ry = top->view->y + desk_title_h(top->view) - t->base->geometry.y;
	} else if (ls && ls->data) {
		struct desk_layer *dl = ls->data;
		wlr_scene_node_coords(&dl->scene->tree->node, &rx, &ry);
	} else {
		return;
	}
	struct wlr_box o;
	wlr_output_layout_get_box(desk->layout, desk->out->wo, &o);
	struct wlr_box box = { o.x - rx, o.y - ry, o.width, o.height };
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

static void desk_popup_commit(struct wl_listener *l, void *data) {
	struct desk_popup *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit) {
		desk_popup_unconstrain(p);
	}
}

static void desk_popup_destroy(struct wl_listener *l, void *data) {
	struct desk_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	if (p->wrap) {
		wlr_scene_node_destroy(&p->wrap->node);
	}
	free(p);
}

static bool desk_new_popup(struct zt_server *server, struct wlr_xdg_popup *popup) {
	struct zt_desk *desk = server->desk;
	if (!desk || !popup->parent) {
		return false;	/* a layer surface's comes through its new_popup */
	}
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent) {
		return false;
	}
	struct wlr_scene_tree *tree = NULL;
	if (parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct zt_toplevel *top = parent->data;
		if (!top || !top->desk || !top->view) {
			return false;
		}
		tree = top->view->content;
	} else if (parent->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		if (!desk_surface(server, parent->surface)) {
			return false;
		}
		tree = parent->data;	/* its own scene tree (set below, or by a layer's popup) */
	}
	if (!tree) {
		return false;
	}
	struct desk_popup *p = calloc(1, sizeof(*p));
	p->desk = desk;
	p->popup = popup;
	popup->base->data = wlr_scene_xdg_surface_create(tree, popup->base);
	p->commit.notify = desk_popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = desk_popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	wl_list_insert(&desk->popups, &p->link);
	return true;
}

/* ---- which surfaces are the desktop's ------------------------------------------------------- */

static bool desk_surface(struct zt_server *server, struct wlr_surface *surface) {
	if (!server->desk || !surface) {
		return false;
	}
	surface = wlr_surface_get_root_surface(surface);
	for (int depth = 0; surface && depth < 16; depth++) {
		if (wlr_layer_surface_v1_try_from_wlr_surface(surface)) {
			return true;
		}
#if WLR_HAS_XWAYLAND
		if (wlr_xwayland_surface_try_from_wlr_surface(surface)) {
			return true;	/* Metro draws no X11 windows */
		}
#endif
		struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(surface);
		if (t) {
			struct zt_toplevel *top = t->base->data;
			return top && top->desk;
		}
		struct wlr_xdg_popup *p = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (!p || !p->parent) {
			return false;
		}
		surface = wlr_surface_get_root_surface(p->parent);
	}
	return false;
}

/* ---- xdg windows -------------------------------------------------------------------------------- */

static void desk_xdg_req_move(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, req_move);
	desk_grab_begin(view->desk, view, GRAB_MOVE, 0);
}

static void desk_xdg_req_resize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, req_resize);
	struct wlr_xdg_toplevel_resize_event *e = data;
	desk_grab_begin(view->desk, view, GRAB_RESIZE, e->edges);
}

static void desk_xdg_req_maximize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, req_maximize);
	desk_view_set_maximized(view, view->top->xdg_toplevel->requested.maximized);
}

static void desk_xdg_req_minimize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, req_minimize);
	if (view->top->xdg_toplevel->requested.minimized) {
		desk_view_set_minimized(view, true);
	}
}

static void desk_xdg_req_menu(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, req_menu);
	struct wlr_xdg_toplevel_show_window_menu_event *e = data;
	struct wlr_box *g = &view->top->xdg_toplevel->base->geometry;
	desk_menu_open(view->desk, view, view->x + e->x - g->x, view->y + desk_title_h(view) + e->y - g->y);
}

static void desk_xdg_set_title(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, set_title);
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_title(view->ft, desk_view_title(view));
	}
	desk_title_draw(view, false);
}

static void desk_xdg_set_app_id(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, set_app_id);
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ft, desk_view_app_id(view));
	}
}

static struct desk_view *desk_view_new(struct zt_desk *desk) {
	struct desk_view *view = calloc(1, sizeof(*view));
	view->desk = desk;
	view->hover = DB_NONE;
	view->title_drawn_hover = DB_NONE;
	view->tree = wlr_scene_tree_create(desk->win_tree);
	view->tree->node.data = view;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_insert(&desk->views, &view->link);
	return view;
}

static void desk_view_free(struct desk_view *view) {
	struct zt_desk *desk = view->desk;
	if (desk->grab_view == view) {
		desk_grab_end(desk);
	}
	if (desk->menu_view == view) {
		desk_menu_close(desk);
	}
	if (desk->focused == view) {
		desk->focused = NULL;
	}
	desk_ft_destroy(view);
	wl_list_remove(&view->link);
	wlr_scene_node_destroy(&view->tree->node);
	free(view);
}

/* An xdg toplevel joins the desktop: its scene node, its listeners. It is not
 * on Metro's list from here on. */
static struct desk_view *desk_xdg_adopt(struct zt_desk *desk, struct zt_toplevel *top) {
	struct desk_view *view = desk_view_new(desk);
	view->top = top;
	top->desk = true;
	top->view = view;
	struct wlr_xdg_toplevel *xt = top->xdg_toplevel;
	view->content = wlr_scene_xdg_surface_create(view->tree, xt->base);
	view->content->node.data = NULL;
	view->req_move.notify = desk_xdg_req_move;
	wl_signal_add(&xt->events.request_move, &view->req_move);
	view->req_resize.notify = desk_xdg_req_resize;
	wl_signal_add(&xt->events.request_resize, &view->req_resize);
	view->req_maximize.notify = desk_xdg_req_maximize;
	wl_signal_add(&xt->events.request_maximize, &view->req_maximize);
	view->req_minimize.notify = desk_xdg_req_minimize;
	wl_signal_add(&xt->events.request_minimize, &view->req_minimize);
	view->req_menu.notify = desk_xdg_req_menu;
	wl_signal_add(&xt->events.request_show_window_menu, &view->req_menu);
	view->set_title.notify = desk_xdg_set_title;
	wl_signal_add(&xt->events.set_title, &view->set_title);
	view->set_app_id.notify = desk_xdg_set_app_id;
	wl_signal_add(&xt->events.set_app_id, &view->set_app_id);
	desk_view_ensure_title(view);
	return view;
}

static void desk_xdg_release(struct desk_view *view) {
	wl_list_remove(&view->req_move.link);
	wl_list_remove(&view->req_resize.link);
	wl_list_remove(&view->req_maximize.link);
	wl_list_remove(&view->req_minimize.link);
	wl_list_remove(&view->req_menu.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	view->top->desk = false;
	view->top->view = NULL;
	view->top = NULL;
}

/* The phone's surfaces are told which output and scale (server_new_surface);
 * on the desktop the scene does that for its output, and the phone's panel
 * is left. Without the leave, GTK 3 draws at the larger of the two scales. */
static void desk_leave_panel(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct zt_server *server = data;
	if (server->output) {
		wlr_surface_send_leave(surface, server->output->wlr_output);
	}
}

static void desk_enter_panel(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct zt_server *server = data;
	if (server->output) {
		wlr_surface_send_enter(surface, server->output->wlr_output);
	}
}

/* Which side a new window opens on (ztmetro's xdg_toplevel_commit asks at the
 * first commit):
 *   - started from the desktop session (ZT_DESKTOP=1 in its environment, which
 *     every child of ztos-desktop-session inherits): the desktop;
 *   - launched from Metro (a tile, the list): the phone;
 *   - otherwise where the user last was: the desktop when its input is the
 *     latest and recent (a file opened from Thunar by D-Bus activation). */
static bool desk_env_has(pid_t pid, const char *want) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	static char buf[65536];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return false;
	}
	buf[n] = 0;
	size_t len = strlen(want);
	for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
		if (strncmp(p, want, len) == 0) {
			return true;
		}
	}
	return false;
}

/* Metro launched something and waits for its window: is this it? (By name:
 * the window may come from another process, a D-Bus service's.) */
static bool desk_metro_waits_for(struct zt_server *server, const char *id) {
	const char *cmd = server->pending_command;
	if (!cmd[0]) {
		return false;
	}
	if (!id || !id[0]) {
		return true;	/* can't tell: Metro's */
	}
	char word[128];
	sscanf(cmd, "%127s", word);
	const char *base = strrchr(word, '/');
	base = base ? base + 1 : word;
	const char *dot = strrchr(id, '.');	/* org.xfce.Thunar: Thunar */
	return !strcasecmp(base, id) || (dot && !strcasecmp(base, dot + 1)) || strcasestr(cmd, id);
}

static bool desk_wants(struct zt_server *server, struct zt_toplevel *top) {
	struct zt_desk *desk = server->desk;
	if (!desk || !desk->out) {
		return false;
	}
	const char *id = top->xdg_toplevel->app_id;
	if (id && !strcmp(id, "ztdesk")) {
		return false;	/* the controller lives on the phone */
	}
	if (desk_metro_waits_for(server, id)) {
		return false;
	}
	pid_t pid = toplevel_pid(top);
	if (pid > 0 && desk_env_has(pid, "ZT_DESKTOP=1")) {
		return true;
	}
	bool recent = desk->last_input_ms && now_ms() - desk->last_input_ms < DESK_ACTIVE_MS &&
		(int32_t)(desk->last_input_ms - desk->phone_touch_ms) > 0;
	wlr_log(WLR_INFO, "desktop: '%s' (pid %d) opens on the %s", id ? id : "?", (int)pid,
		recent ? "desktop: the last input was there" : "phone");
	return recent;
}

static bool desk_toplevel_commit(struct zt_toplevel *top) {
	struct zt_server *server = top->server;
	struct zt_desk *desk = server->desk;
	struct wlr_xdg_toplevel *xt = top->xdg_toplevel;
	if (xt->base->initial_commit && !top->desk && desk_wants(server, top)) {
		/* off Metro's list: nothing there sees it from now on */
		wl_list_remove(&top->link);
		wl_list_init(&top->link);
		desk_xdg_adopt(desk, top);
		wlr_log(WLR_INFO, "desktop: new window '%s'", xt->app_id ? xt->app_id : "?");
	}
	if (!top->desk) {
		return false;
	}
	struct desk_view *view = top->view;
	if (xt->base->initial_commit) {
		/* its own size, unless it asked for the whole screen */
		wlr_xdg_toplevel_set_size(xt, 0, 0);
		wlr_xdg_toplevel_set_wm_capabilities(xt, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		apply_decoration(server, xt);
		desk_view_ensure_title(view);
		return true;
	}
	int w, h;
	desk_view_geometry(view, &w, &h);
	if (desk->grab == GRAB_RESIZE && desk->grab_view == view) {
		/* keep the far edge where it is while the near one moves */
		struct wlr_box b = desk->grab_box;
		int x = view->x, y = view->y;
		if (desk->grab_edges & WLR_EDGE_LEFT) x = b.x + b.width - w;
		if (desk->grab_edges & WLR_EDGE_TOP) y = b.y + b.height - h;
		if (x != view->x || y != view->y) desk_view_move(view, x, y);
	}
	if (w != view->w || h != view->h) {
		view->w = w, view->h = h;
		desk_title_draw(view, false);
	}
	if (view->mapped && !view->placed && w > 0) {
		desk_view_place(view);
	}
	return true;
}

static bool desk_toplevel_map(struct zt_toplevel *top) {
	if (!top->desk) {
		return false;
	}
	struct desk_view *view = top->view;
	struct zt_desk *desk = view->desk;
	view->mapped = true;
	wlr_xdg_surface_for_each_surface(top->xdg_toplevel->base, desk_leave_panel, desk->server);
	wlr_xdg_toplevel_set_suspended(top->xdg_toplevel, false);
	if (!view->placed) {
		desk_view_geometry(view, &view->w, &view->h);
		desk_view_place(view);
	}
	wlr_scene_node_set_enabled(&view->tree->node, true);
	desk_ft_create(view);
	desk->focused_layer = NULL;
	desk_focus_view(desk, view);
	if (top->xdg_toplevel->requested.fullscreen) {
		desk_view_set_fullscreen(view, true);
	} else if (top->xdg_toplevel->requested.maximized) {
		desk_view_set_maximized(view, true);
	}
	/* the keyboard follows the new window when the desktop was being used */
	if (desk->last_input_ms && now_ms() - desk->last_input_ms < DESK_ACTIVE_MS) {
		desk_take_keyboard(desk);
	}
	desk_title_draw(view, true);
	desk_state_write(desk);
	return true;
}

static bool desk_toplevel_unmap(struct zt_toplevel *top) {
	if (!top->desk) {
		return false;
	}
	struct desk_view *view = top->view;
	struct zt_desk *desk = view->desk;
	view->mapped = false;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	desk_ft_destroy(view);
	if (desk->grab_view == view) desk_grab_end(desk);
	if (desk->menu_view == view) desk_menu_close(desk);
	if (desk->focused == view) {
		desk->focused = NULL;
		struct desk_view *next = desk_top_view(desk, view);
		if (next) {
			desk_focus_view(desk, next);
		} else if (desk->server->seat->keyboard_state.focused_surface == desk_view_surface(view)) {
			wlr_seat_keyboard_notify_clear_focus(desk->server->seat);
		}
	}
	view->placed = false;
	desk_state_write(desk);
	return true;
}

static void desk_toplevel_destroy(struct zt_toplevel *top) {
	if (!top->desk || !top->view) {
		return;
	}
	struct desk_view *view = top->view;
	struct zt_desk *desk = view->desk;
	desk_xdg_release(view);
	desk_view_free(view);
	desk_state_write(desk);
}

static bool desk_toplevel_fullscreen(struct zt_toplevel *top) {
	if (!top->desk) {
		return false;
	}
	desk_view_set_fullscreen(top->view, top->xdg_toplevel->requested.fullscreen);
	return true;
}

/* ---- moving windows between the phone and the desktop -------------------------------------------- */

static bool desk_available(struct zt_server *server) {
	return server->desk && server->desk->out;
}

/* Metro's app goes to the desktop: Metro forgets it (as it would a closed
 * app), the desktop adopts it unmaximised at a desktop size, and the scene
 * tells it its new output and scale. Its surface and state stay the same. */
static bool desk_take_from_phone(struct zt_server *server, struct zt_toplevel *top) {
	struct zt_desk *desk = server->desk;
	if (!desk_available(server) || top->desk || !top->mapped || top->waydroid_ime || top->waydroid_boot) {
		return false;
	}
	const char *id = top->xdg_toplevel->app_id;
	if (id && !strcmp(id, "ztdesk")) {
		return false;
	}
	wlr_log(WLR_INFO, "desktop: '%s' moves from the phone", app_name(top));
	if (server->shown == top) {
		show_toplevel(server, NULL);
		server->leaving = NULL;	/* nothing to swing out: it is somewhere else now */
	}
	switcher_forget(server, top);
	if (server->leaving == top) {
		server->leaving = NULL;
	}
	top->mapped = false;
	wl_list_remove(&top->link);
	wl_list_init(&top->link);
	apps_running_write(server);
	apps_suspend_sync(server);
	struct desk_view *view = desk_xdg_adopt(desk, top);
	struct wlr_xdg_toplevel *xt = top->xdg_toplevel;
	wlr_xdg_toplevel_set_fullscreen(xt, false);
	wlr_xdg_toplevel_set_maximized(xt, false);
	wlr_xdg_toplevel_set_tiled(xt, 0);
	wlr_xdg_toplevel_set_suspended(xt, false);
	top->fullscreen = false;
	/* phone-shaped apps of ours stay phone-shaped, others get a desktop window */
	bool ours = id && (!strncmp(id, "zt", 2) || !strncmp(id, "zrn", 3));
	int w = ours ? 480 : 1024, h = ours ? 860 : 700;
	if (w > desk->usable.width) w = desk->usable.width;
	if (h > desk->usable.height - desk_title_h(view)) h = desk->usable.height - desk_title_h(view);
	wlr_xdg_toplevel_set_size(xt, w, h);
	view->w = w, view->h = h;
	desk_view_place(view);	/* at the size it was asked to take, not the phone's */
	desk_toplevel_map(top);
	desk_take_keyboard(desk);
	damage(server);
	return true;
}

/* A desktop window goes to the phone, and Metro shows it as the app in front. */
static void desk_give_to_phone(struct desk_view *view) {
	struct zt_desk *desk = view->desk;
	struct zt_server *server = desk->server;
	struct zt_toplevel *top = view->top;
	if (!top) {
		return;
	}
	wlr_log(WLR_INFO, "desktop: '%s' moves to the phone", app_name(top));
	if (view->fullscreen) {
		view->fullscreen = false;
	}
	desk_toplevel_unmap(top);
	desk_xdg_release(view);
	desk_view_free(view);
	struct wlr_xdg_toplevel *xt = top->xdg_toplevel;
	/* the desktop's output, left (the scene's node is gone, but say it) */
	struct desk_output *o;
	wl_list_for_each(o, &desk->outputs, link) {
		struct wlr_output *wo = o->wo;
		wlr_surface_send_leave(xt->base->surface, wo);
	}
	wlr_xdg_surface_for_each_surface(xt->base, desk_enter_panel, server);
	wl_list_insert(&server->toplevels, &top->link);
	top->mapped = true;
	top->zoom = app_zoom_for(server, top);
	wlr_xdg_toplevel_set_maximized(xt, true);
	wlr_xdg_toplevel_set_tiled(xt, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	app_tell_scale(server, top);
	app_configure(server, top);
	apps_running_write(server);
	show_toplevel(server, top);
	desk_state_write(desk);
}

/* ---- the phone's keyboard and touch, as they meet the desktop -------------------------------- */

static bool desk_owns_pointers(struct zt_server *server) {
	return server->desk && server->desk->out;
}

static void desk_new_input(struct zt_server *server, struct wlr_input_device *device) {
	if (server->desk && device->type == WLR_INPUT_DEVICE_POINTER) {
		desk_attach_pointer(server->desk, device);
	}
}

/* Metro shows an app. ztdesk is a window onto the desktop: with it in front the
 * keyboard (the phone's own on-screen one too) types into the desktop. */
static void desk_app_shown(struct zt_server *server, struct zt_toplevel *top) {
	struct zt_desk *desk = server->desk;
	if (!desk || !desk->out || !top) {
		return;
	}
	const char *id = top->xdg_toplevel->app_id;
	if (id && !strcmp(id, "ztdesk")) {
		desk_take_keyboard(desk);
	}
}

/* A finger on the phone: the phone's app in front has the keyboard again
 * (unless it is ztdesk, whose touches go to the desktop). */
static void desk_phone_touched(struct zt_server *server) {
	struct zt_desk *desk = server->desk;
	if (desk) {
		desk->phone_touch_ms = now_ms();
		/* A desktop menu that grabbed the seat (xdg_popup.grab) takes its
		 * touches as well as the pointer: the phone's are not the desktop's. */
		struct desk_popup *p;
		wl_list_for_each(p, &desk->popups, link) {
			if (p->popup->seat && server->seat->touch_state.grab != server->seat->touch_state.default_grab) {
				wlr_seat_touch_end_grab(server->seat);
				break;
			}
		}
	}
	if (!desk || !desk_has_keyboard(desk) || !server->shown || !server->shown->mapped) {
		return;
	}
	const char *id = server->shown->xdg_toplevel->app_id;
	if (id && !strcmp(id, "ztdesk")) {
		return;
	}
	struct wlr_surface *s = server->shown->xdg_toplevel->base->surface;
	wlr_seat_set_keyboard(server->seat, &server->vkbd);
	wlr_seat_keyboard_notify_enter(server->seat, s, NULL, 0, &server->vkbd.modifiers);
	text_inputs_focus(server, s);
}

/* Keys from a real keyboard while the desktop has the focus: the window
 * manager's own first (Alt+Tab, Alt+F4, Alt+Space, Super+arrows). */
/* The desktop is on a monitor and its window (or layer) has the keyboard. */
static bool desk_keyboard_live(struct zt_server *server) {
	struct zt_desk *desk = server->desk;
	return desk && desk->out && !desk->out->virtual && desk_has_keyboard(desk);
}

static bool desk_key(struct zt_server *server, struct wlr_keyboard *keyboard,
		const struct wlr_keyboard_key_event *event) {
	struct zt_desk *desk = server->desk;
	if (!desk || !desk->out || !desk_has_keyboard(desk)) {
		return false;
	}
	desk->last_input_ms = now_ms();
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return false;
	}
	uint32_t mods = wlr_keyboard_get_modifiers(keyboard);
	struct desk_view *f = desk->focused;
	if (desk->menu && event->keycode == KEY_ESC) {
		desk_menu_close(desk);
		return true;
	}
	if (mods & WLR_MODIFIER_ALT) {
		if (event->keycode == KEY_TAB) {
			/* the window at the back comes to the front */
			struct desk_view *last = NULL, *v;
			wl_list_for_each(v, &desk->views, link) {
				if (v->mapped) last = v;
			}
			if (last && last != f) {
				desk->focused_layer = NULL;
				desk_focus_view(desk, last);
			}
			return true;
		}
		if (event->keycode == KEY_F4 && f) {
			desk_view_close(f);
			return true;
		}
		if (event->keycode == KEY_SPACE && f) {
			desk_menu_open(desk, f, f->x, f->y + desk_title_h(f));
			return true;
		}
	}
	if ((mods & WLR_MODIFIER_LOGO) && f) {
		switch (event->keycode) {
		case KEY_UP: desk_view_set_maximized(f, true); return true;
		case KEY_DOWN:
			if (f->maximized) desk_view_set_maximized(f, false);
			else desk_view_set_minimized(f, true);
			return true;
		case KEY_P: desk_give_to_phone(f); return true;
		}
	}
	return false;
}

/* ---- Xwayland: X11 apps, on the desktop only ---------------------------------------------------- */

#if WLR_HAS_XWAYLAND
static void desk_x_commit(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_commit);
	if (view->xs->override_redirect) {
		return;
	}
	if (view->xs->width != view->w || view->xs->height != view->h) {
		view->w = view->xs->width, view->h = view->xs->height;
		desk_title_draw(view, false);
	}
}

static void desk_x_map(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_map);
	struct zt_desk *desk = view->desk;
	struct wlr_xwayland_surface *xs = view->xs;
	view->mapped = true;
	view->content = wlr_scene_subsurface_tree_create(view->tree, xs->surface);
	view->x_commit.notify = desk_x_commit;
	wl_signal_add(&xs->surface->events.commit, &view->x_commit);
	if (xs->override_redirect) {
		/* menus and tooltips: where they say, above the windows, no focus */
		wlr_scene_node_reparent(&view->tree->node, desk->x_or_tree);
		view->x = xs->x, view->y = xs->y;
		wlr_scene_node_set_position(&view->tree->node, xs->x, xs->y);
		wlr_scene_node_set_enabled(&view->tree->node, true);
		if (wlr_xwayland_surface_override_redirect_wants_focus(xs)) {
			desk_keyboard_enter(desk, xs->surface);
		}
		return;
	}
	view->w = xs->width, view->h = xs->height;
	desk_view_ensure_title(view);
	desk_view_place(view);
	desk_view_configure(view, view->x, view->y, view->w, view->h);
	wlr_scene_node_set_enabled(&view->tree->node, true);
	desk_ft_create(view);
	desk->focused_layer = NULL;
	desk_focus_view(desk, view);
	desk_take_keyboard(desk);
	desk_title_draw(view, true);
	desk_state_write(desk);
}

static void desk_x_unmap(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_unmap);
	struct zt_desk *desk = view->desk;
	view->mapped = false;
	wl_list_remove(&view->x_commit.link);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	if (view->content) {
		wlr_scene_node_destroy(&view->content->node);
		view->content = NULL;
	}
	desk_ft_destroy(view);
	if (desk->grab_view == view) desk_grab_end(desk);
	if (desk->menu_view == view) desk_menu_close(desk);
	if (desk->focused == view) {
		desk->focused = NULL;
		struct desk_view *next = desk_top_view(desk, view);
		if (next) desk_focus_view(desk, next);
	}
	view->placed = false;
	if (view->xs->override_redirect) {
		wlr_scene_node_reparent(&view->tree->node, desk->win_tree);
	}
	desk_state_write(desk);
}

static void desk_x_associate(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_associate);
	view->x_map.notify = desk_x_map;
	wl_signal_add(&view->xs->surface->events.map, &view->x_map);
	view->x_unmap.notify = desk_x_unmap;
	wl_signal_add(&view->xs->surface->events.unmap, &view->x_unmap);
	view->x_associated = true;
}

static void desk_x_dissociate(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_dissociate);
	if (view->x_associated) {
		wl_list_remove(&view->x_map.link);
		wl_list_remove(&view->x_unmap.link);
		view->x_associated = false;
	}
}

static void desk_x_configure(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_configure);
	struct wlr_xwayland_surface_configure_event *e = data;
	if (!view->mapped || view->xs->override_redirect) {
		wlr_xwayland_surface_configure(view->xs, e->x, e->y, e->width, e->height);
		return;
	}
	/* a mapped window may change its size, but not move (it is ours to place) */
	desk_view_configure(view, view->x, view->y, e->width, e->height);
}

static void desk_x_geometry(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_geometry);
	if (view->xs->override_redirect && view->mapped) {
		view->x = view->xs->x, view->y = view->xs->y;
		wlr_scene_node_set_position(&view->tree->node, view->x, view->y);
	}
}

static void desk_x_activate(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_activate);
	if (view->mapped && !view->xs->override_redirect) {
		desk_focus_view(view->desk, view);
	}
}

static void desk_x_move(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_move);
	desk_grab_begin(view->desk, view, GRAB_MOVE, 0);
}

static void desk_x_resize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_resize);
	struct wlr_xwayland_resize_event *e = data;
	desk_grab_begin(view->desk, view, GRAB_RESIZE, e->edges);
}

static void desk_x_maximize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_maximize);
	if (view->mapped) {
		desk_view_set_maximized(view, view->xs->maximized_horz || view->xs->maximized_vert);
	}
}

static void desk_x_minimize(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_minimize);
	struct wlr_xwayland_minimize_event *e = data;
	if (view->mapped) {
		desk_view_set_minimized(view, e->minimize);
	}
}

static void desk_x_fullscreen(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_fullscreen);
	if (view->mapped) {
		desk_view_set_fullscreen(view, view->xs->fullscreen);
	}
}

static void desk_x_title(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_title);
	if (view->ft) {
		wlr_foreign_toplevel_handle_v1_set_title(view->ft, desk_view_title(view));
	}
	desk_title_draw(view, false);
}

static void desk_x_destroy(struct wl_listener *l, void *data) {
	struct desk_view *view = wl_container_of(l, view, x_destroy);
	if (view->x_associated) {
		wl_list_remove(&view->x_map.link);
		wl_list_remove(&view->x_unmap.link);
	}
	if (view->mapped) {
		wl_list_remove(&view->x_commit.link);
	}
	wl_list_remove(&view->x_destroy.link);
	wl_list_remove(&view->x_associate.link);
	wl_list_remove(&view->x_dissociate.link);
	wl_list_remove(&view->x_configure.link);
	wl_list_remove(&view->x_activate.link);
	wl_list_remove(&view->x_move.link);
	wl_list_remove(&view->x_resize.link);
	wl_list_remove(&view->x_maximize.link);
	wl_list_remove(&view->x_minimize.link);
	wl_list_remove(&view->x_fullscreen.link);
	wl_list_remove(&view->x_title.link);
	wl_list_remove(&view->x_geometry.link);
	view->xs = NULL;
	desk_view_free(view);
}

static void desk_x_new_surface(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, x_new_surface);
	struct wlr_xwayland_surface *xs = data;
	struct desk_view *view = desk_view_new(desk);
	view->xs = xs;
	view->x_destroy.notify = desk_x_destroy;
	wl_signal_add(&xs->events.destroy, &view->x_destroy);
	view->x_associate.notify = desk_x_associate;
	wl_signal_add(&xs->events.associate, &view->x_associate);
	view->x_dissociate.notify = desk_x_dissociate;
	wl_signal_add(&xs->events.dissociate, &view->x_dissociate);
	view->x_configure.notify = desk_x_configure;
	wl_signal_add(&xs->events.request_configure, &view->x_configure);
	view->x_activate.notify = desk_x_activate;
	wl_signal_add(&xs->events.request_activate, &view->x_activate);
	view->x_move.notify = desk_x_move;
	wl_signal_add(&xs->events.request_move, &view->x_move);
	view->x_resize.notify = desk_x_resize;
	wl_signal_add(&xs->events.request_resize, &view->x_resize);
	view->x_maximize.notify = desk_x_maximize;
	wl_signal_add(&xs->events.request_maximize, &view->x_maximize);
	view->x_minimize.notify = desk_x_minimize;
	wl_signal_add(&xs->events.request_minimize, &view->x_minimize);
	view->x_fullscreen.notify = desk_x_fullscreen;
	wl_signal_add(&xs->events.request_fullscreen, &view->x_fullscreen);
	view->x_title.notify = desk_x_title;
	wl_signal_add(&xs->events.set_title, &view->x_title);
	view->x_geometry.notify = desk_x_geometry;
	wl_signal_add(&xs->events.set_geometry, &view->x_geometry);
}

static void desk_x_ready(struct wl_listener *l, void *data) {
	struct zt_desk *desk = wl_container_of(l, desk, x_ready);
	wlr_xwayland_set_seat(desk->xwayland, desk->server->seat);
	wlr_log(WLR_INFO, "desktop: Xwayland ready on %s", desk->xwayland->display_name);
}
#endif

/* ---- outputs ------------------------------------------------------------------------------------- */

/* A helper, not waited for: none of the shell's descriptors go with it. */
static void desk_spawn(const char *program, const char *a1, const char *a2) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		child_priority();
		close_inherited();
		execlp(program, program, a1, a2, (char *)NULL);
		_exit(127);
	}
}

static bool desk_watched(struct zt_desk *desk) {
	struct zt_server *server = desk->server;
	if ((int32_t)(desk->watch_until - now_ms()) > 0) {
		return true;
	}
	/* ztdesk in front on a lit screen is looking at it */
	return server->screen_on && server->shown && server->shown->xdg_toplevel->app_id &&
		!strcmp(server->shown->xdg_toplevel->app_id, "ztdesk");
}

static void desk_output_frame(struct wl_listener *l, void *data) {
	struct desk_output *o = wl_container_of(l, o, frame);
	struct zt_desk *desk = o->desk;
	if (o->virtual && !desk_watched(desk)) {
		/* a virtual desktop nobody looks at: its apps get a frame a second
		 * (desk_idle_tick), not 60, and the GPU draws nothing */
		return;
	}
	wlr_scene_output_commit(o->so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(o->so, &now);
}

static int desk_idle_tick(void *data) {
	struct zt_desk *desk = data;
	struct desk_output *o;
	bool any = false;
	wl_list_for_each(o, &desk->outputs, link) {
		any = any || o->virtual;
		if (o->virtual && !desk_watched(desk)) {
			wlr_scene_output_commit(o->so, NULL);
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			wlr_scene_output_send_frame_done(o->so, &now);
		} else if (o->virtual) {
			wlr_output_schedule_frame(o->wo);	/* watched again: back to every frame */
		}
	}
	/* only while there is a virtual output: the phone asleep is not woken for nothing */
	wl_event_source_timer_update(desk->idle_timer, any ? 1000 : 0);
	return 0;
}

static void desk_output_request_state(struct wl_listener *l, void *data) {
	struct desk_output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *e = data;
	wlr_output_commit_state(o->wo, e->state);
}

static void desk_use_output(struct zt_desk *desk, struct desk_output *o);

static void desk_output_destroy(struct wl_listener *l, void *data) {
	struct desk_output *o = wl_container_of(l, o, destroy);
	struct zt_desk *desk = o->desk;
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);
	wlr_output_layout_remove(desk->layout, o->wo);
	wlr_output_layout_remove(desk->server->output_layout, o->wo);
	bool was_active = desk->out == o;
	bool real = !o->virtual;
	char name[32];
	snprintf(name, sizeof(name), "%s", o->wo->name);
	if (was_active) {
		desk->out = NULL;
	}
	if (o->so) {
		wlr_scene_output_destroy(o->so);
	}
	free(o);
	wlr_log(WLR_INFO, "desktop: output %s gone", name);
	if (real) {
		/* the DP Alt Mode agent's hook (stage 6: desktop/DESIGN.md) */
		desk_spawn("ztos-desktop-hotplug", "remove", name);
	}
	if (!was_active) {
		return;
	}
	/* another desktop output, if there is one; else per the setting */
	struct desk_output *next;
	wl_list_for_each(next, &desk->outputs, link) {
		desk_use_output(desk, next);
		return;
	}
	if (real && desk->unplug == UNPLUG_PHONE) {
		/* every xdg window to the phone; X11 ones and the session stay on a
		 * virtual output, since Metro can't show them */
		struct desk_view *v, *tmp;
		wl_list_for_each_safe(v, tmp, &desk->views, link) {
			if (v->top && v->mapped) desk_give_to_phone(v);
		}
	}
	bool anything = !wl_list_empty(&desk->views) || desk->session_pid > 0;
	if (real && anything && desk->headless) {
		/* "keep": the same desktop on a virtual output, seen through ztdesk */
		wlr_headless_add_output(desk->headless, desk->virt_w, desk->virt_h);
	} else if (!desk->out) {
		/* no desktop: the windows left go to the phone */
		struct desk_view *v, *tmp;
		wl_list_for_each_safe(v, tmp, &desk->views, link) {
			if (v->top) desk_give_to_phone(v);
		}
		/* and no XFCE with nowhere to draw (it would only start again and again) */
		desk_session_stop(desk);
		struct zt_server *server = desk->server;
		wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_KEYBOARD);
	}
	desk_state_write(desk);
}

/* The desktop moves to output `o` (the first, a monitor replacing the virtual
 * one, or what is left after one went): windows keep their place relative to
 * the output's corner, layer surfaces on the old one are closed (their clients
 * make new ones; ztos-desktop-session restarts what doesn't). */
static void desk_use_output(struct zt_desk *desk, struct desk_output *o) {
	struct desk_output *old = desk->out;
	if (old == o) {
		return;
	}
	struct wlr_box ob = {0}, nb;
	if (old) {
		wlr_output_layout_get_box(desk->layout, old->wo, &ob);
	}
	wlr_output_layout_get_box(desk->layout, o->wo, &nb);
	desk->out = o;
	struct desk_layer *dl, *tmp;
	wl_list_for_each_safe(dl, tmp, &desk->layers, link) {
		if (dl->ls->output != o->wo) {
			wlr_layer_surface_v1_destroy(dl->ls);
		}
	}
	desk->usable = nb;
	wlr_scene_node_set_position(&desk->backdrop->node, nb.x, nb.y);
	wlr_scene_rect_set_size(desk->backdrop, nb.width, nb.height);
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		int x = v->x - ob.x + nb.x, y = v->y - ob.y + nb.y;
		if (!old) continue;
		if (x + 100 > nb.x + nb.width) x = nb.x + nb.width - 100;
		if (y + 40 > nb.y + nb.height) y = nb.y + nb.height - 40;
		desk_view_move(v, x, y);
		if (v->ft) {
			if (old) wlr_foreign_toplevel_handle_v1_output_leave(v->ft, old->wo);
			wlr_foreign_toplevel_handle_v1_output_enter(v->ft, o->wo);
		}
		if (v->maximized && !v->fullscreen) {
			desk_view_configure(v, nb.x, nb.y, nb.width, nb.height - desk_title_h(v));
		} else if (v->fullscreen) {
			desk_view_configure(v, nb.x, nb.y, nb.width, nb.height);
		}
	}
	wlr_cursor_map_to_output(desk->cursor, o->wo);
	wlr_cursor_warp(desk->cursor, NULL, nb.x + nb.width / 2.0, nb.y + nb.height / 2.0);
	desk->cursor_name = NULL;
	desk_cursor_set(desk, "default");
	struct zt_server *server = desk->server;
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
	wlr_xcursor_manager_load(desk->xcursor, o->wo->scale);
	desk_arrange_layers(desk);
	wlr_log(WLR_INFO, "desktop: on %s, %dx%d points at %+d,%+d", o->wo->name, nb.width, nb.height, nb.x, nb.y);
	if (old && desk->session_pid > 0) {
		/* panels on the old output were closed: the session puts them up again */
		desk_spawn("ztos-desktop-session", "--refresh", NULL);
	}
	desk_state_write(desk);
}

/* A scale for a monitor: 1 unless it is dense (a 4K panel at 2), or the setting's. */
static float desk_auto_scale(struct zt_desk *desk, struct wlr_output *wo, int px_w) {
	if (desk->scale > 0.0f) {
		return desk->scale;
	}
	if (wo->phys_width > 0 && px_w > 0) {
		float dpi = px_w / (wo->phys_width / 25.4f);
		if (dpi > 200) return 2.0f;
		if (dpi > 150) return 1.5f;
	}
	return px_w >= 3200 ? 2.0f : 1.0f;
}

/* The monitor's mode the setting (or "desktop mode") asks for, if it has it:
 * WxH at HZ (rounded), or the fastest WxH; NULL: none asked for or not there. */
static struct wlr_output_mode *desk_monitor_mode(struct zt_desk *desk, struct wlr_output *wo) {
	struct wlr_output_mode *m, *best = NULL;
	if (desk->mon_w <= 0) {
		return NULL;
	}
	wl_list_for_each(m, &wo->modes, link) {
		if (m->width != desk->mon_w || m->height != desk->mon_h) continue;
		if (desk->mon_hz > 0 && (m->refresh + 500) / 1000 != desk->mon_hz) continue;
		if (!best || m->refresh > best->refresh) best = m;
	}
	return best;
}

static void desk_new_output(struct zt_server *server, struct wlr_output *wo) {
	struct zt_desk *desk = server->desk;
	if (!desk) {
		return;
	}
	bool virtual = wlr_output_is_headless(wo);
	wlr_output_init_render(wo, server->allocator, server->renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = virtual ? NULL : desk_monitor_mode(desk, wo);
	if (!mode) {
		mode = wlr_output_preferred_mode(wo);
	}
	int px_w = wo->width;
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
		px_w = mode->width;
	} else if (virtual) {
		wlr_output_state_set_custom_mode(&state, desk->virt_w, desk->virt_h, desk->virt_hz * 1000);
		px_w = desk->virt_w;
	}
	wlr_output_state_set_scale(&state, virtual ? (desk->scale > 0 ? desk->scale : 1.0f) :
		desk_auto_scale(desk, wo, px_w));
	if (!wlr_output_commit_state(wo, &state)) {
		wlr_log(WLR_ERROR, "desktop: output %s did not take its mode", wo->name);
	}
	wlr_output_state_finish(&state);

	struct desk_output *o = calloc(1, sizeof(*o));
	o->desk = desk;
	o->wo = wo;
	o->virtual = virtual;
	o->frame.notify = desk_output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = desk_output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->destroy.notify = desk_output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);
	wl_list_insert(desk->outputs.prev, &o->link);
	if (virtual) {
		wl_event_source_timer_update(desk->idle_timer, 1000);
	}
	/* right of the phone's panel in the session's layout (what xdg-output
	 * tells clients), and at the same place in the desktop's own */
	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(server->output_layout, wo);
	int lx = lo ? lo->x : 10000, ly = lo ? lo->y : 0;
	wlr_output_layout_add(desk->layout, wo, lx, ly);
	o->so = wlr_scene_output_create(desk->scene, wo);
	wlr_scene_output_set_position(o->so, lx, ly);
	int w, h;
	wlr_output_effective_resolution(wo, &w, &h);
	wlr_log(WLR_INFO, "desktop: output %s (%s), %dx%d points at scale %.2f", wo->name,
		virtual ? "virtual" : "monitor", w, h, wo->scale);

	/* a monitor takes the desktop from a virtual output, which then goes */
	if (!desk->out || (desk->out->virtual && !virtual)) {
		struct desk_output *was = desk->out;
		desk_use_output(desk, o);
		if (was && was->virtual) {
			wlr_output_destroy(was->wo);
		}
	}
	if (!virtual) {
		desk_spawn("ztos-desktop-hotplug", "add", wo->name);
	}
	if ((virtual ? desk->autostart_virtual : desk->autostart) && desk->session_pid <= 0) {
		desk_session_start(desk);
	}
	desk_state_write(desk);
}

/* ---- the XFCE session ------------------------------------------------------------------------------ */

static int desk_session_reap(int sig, void *data) {
	/* SIGCHLD is ignored in the shell (children are reaped by the kernel);
	 * the session's end is noticed by polling (desk_session_alive) instead */
	return 0;
}

static bool desk_session_alive(struct zt_desk *desk) {
	if (desk->session_pid <= 0) {
		return false;
	}
	if (kill(desk->session_pid, 0) < 0 && errno == ESRCH) {
		desk->session_pid = 0;
		return false;
	}
	return true;
}

static void desk_session_start(struct zt_desk *desk) {
	if (!desk->out || desk_session_alive(desk)) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		child_priority();
		close_inherited();
		/* every program the session starts, and their children, open their
		 * windows on the desktop (desk_wants) */
		setenv("ZT_DESKTOP", "1", 1);
		setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
		setenv("XDG_SESSION_DESKTOP", "xfce", 1);
		setenv("DESKTOP_SESSION", "xfce", 1);
		unsetenv("ZTMETRO_SCALE");	/* the phone's scale: not for these */
		if (desk->x_display[0]) {
			setenv("DISPLAY", desk->x_display, 1);
		}
		setenv("ZT_DESKTOP_OUTPUT", desk->out->wo->name, 1);
		execlp("ztos-desktop-session", "ztos-desktop-session", (char *)NULL);
		_exit(127);
	}
	desk->session_pid = pid;
	desk->session_pgid = pid;
	desk->session_wanted = true;
	wlr_log(WLR_INFO, "desktop: session started (pid %d)", (int)pid);
}

/* A stopped session gets SIGTERM; whatever of its process group is still
 * there 3 s later (xfdesktop catches SIGTERM and has been seen to stay,
 * reparented to init) gets SIGKILL: nothing of XFCE outlives "desktop stop". */
static int desk_session_kill_tick(void *data) {
	struct zt_desk *desk = data;
	if (desk->session_pgid_dying > 0 && kill(-desk->session_pgid_dying, 0) == 0) {
		wlr_log(WLR_INFO, "desktop: session group %d still there, killed",
			(int)desk->session_pgid_dying);
		kill(-desk->session_pgid_dying, SIGKILL);
	}
	desk->session_pgid_dying = 0;
	return 0;
}

static void desk_session_stop(struct zt_desk *desk) {
	desk->session_wanted = false;
	/* the group, even when its leader (the script) has gone already */
	pid_t g = desk->session_pgid;
	if (g > 0 && kill(-g, 0) == 0) {
		kill(-g, SIGTERM);	/* its process group: setsid above */
		wlr_log(WLR_INFO, "desktop: session stopped");
		desk->session_pgid_dying = g;
		if (!desk->session_kill_timer) {
			desk->session_kill_timer = wl_event_loop_add_timer(desk->server->loop,
				desk_session_kill_tick, desk);
		}
		if (desk->session_kill_timer) {
			wl_event_source_timer_update(desk->session_kill_timer, 3000);
		}
	}
	desk->session_pid = 0;
	desk->session_pgid = 0;
}

/* ---- state for tools, and the control pipe ------------------------------------------------------- */

/* $XDG_RUNTIME_DIR/ztmetro-desktop.state: the desktop output and its windows,
 * for ztdesk, tests and scripts/ztctl.py. Replaced whole. */
static void desk_state_write(struct zt_desk *desk) {
	char path[256], tmp[280];
	runtime_file(path, sizeof(path), "ztmetro-desktop.state");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	if (desk->out) {
		struct wlr_box b;
		wlr_output_layout_get_box(desk->layout, desk->out->wo, &b);
		fprintf(f, "output %s %s %d %d %.2f %d %d\n", desk->out->wo->name,
			desk->out->virtual ? "virtual" : "monitor", desk->out->wo->width, desk->out->wo->height,
			desk->out->wo->scale, b.x, b.y);
		/* the mode it runs at (refresh in mHz; 0 when the output has none) */
		fprintf(f, "mode %d %d %d\n", desk->out->wo->width, desk->out->wo->height,
			desk->out->wo->refresh);
	} else {
		fprintf(f, "output none\n");
	}
	fprintf(f, "session %s\n", desk_session_alive(desk) ? "running" : "stopped");
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		if (!v->mapped || (
#if WLR_HAS_XWAYLAND
				v->xs && v->xs->override_redirect
#else
				false
#endif
				)) {
			continue;
		}
		fprintf(f, "window %s %s %d %d %d %d %s%s%s\t%s\n", v->top ? "xdg" : "x11", desk_view_app_id(v)[0] ?
			desk_view_app_id(v) : "-", v->x, v->y, v->w, v->h, v == desk->focused ? "focused," : "",
			v->minimized ? "minimized," : "", v->maximized ? "maximized" : "", desk_view_title(v));
	}
	fclose(f);
	rename(tmp, path);
}

static struct desk_view *desk_view_named(struct zt_desk *desk, const char *name) {
	if (!strcmp(name, "focused")) {
		return desk->focused;
	}
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		if (v->mapped && !strcmp(desk_view_app_id(v), name)) return v;
	}
	return NULL;
}

/*
 * "desktop ..." on $XDG_RUNTIME_DIR/ztmetro.ctl:
 *
 *     desktop add [WxH[@HZ]]    a virtual output (headless) for the desktop
 *     desktop remove            the virtual output goes (its windows: as on unplug)
 *     desktop start | stop      the XFCE session (ztos-desktop-session)
 *     desktop move APP|shown    a phone app (by app_id, or the one in front) to the desktop
 *     desktop phone APP|focused a desktop window to the phone
 *     desktop watch SECONDS     someone is looking at the virtual output: full frame rate
 *     desktop pointer X Y       put the pointer there (output points), for tests
 *     desktop click X Y [B]     and click (B: 1 left, 2 right, 3 middle)
 *     desktop state             write ztmetro-desktop.state now
 *     desktop mode WxH[@HZ]|preferred   the monitors' mode, now (monitor_mode = in
 *                               ~/.config/ztmetro/desktop keeps one across plugs)
 */
static void desk_ctl(struct zt_server *server, const char *args) {
	struct zt_desk *desk = server->desk;
	if (!desk) {
		return;
	}
	while (*args == ' ') args++;
	int w, h, hz, n;
	char name[128];
	double x, y;
	if (!strncmp(args, "add", 3)) {
		if (!desk->headless) {
			wlr_log(WLR_ERROR, "desktop: no headless backend");
			return;
		}
		struct desk_output *o;
		wl_list_for_each(o, &desk->outputs, link) {
			if (o->virtual) return;	/* one is enough */
		}
		n = sscanf(args + 3, " %dx%d@%d", &w, &h, &hz);
		if (n >= 2 && w >= 640 && h >= 480 && w <= 3840 && h <= 2160) {
			desk->virt_w = w, desk->virt_h = h;
			if (n == 3 && hz >= 10 && hz <= 120) desk->virt_hz = hz;
		}
		wlr_headless_add_output(desk->headless, desk->virt_w, desk->virt_h);
	} else if (!strncmp(args, "remove", 6)) {
		struct desk_output *o, *tmp;
		wl_list_for_each_safe(o, tmp, &desk->outputs, link) {
			if (o->virtual) {
				wlr_output_destroy(o->wo);
				return;
			}
		}
	} else if (!strncmp(args, "mode", 4)) {
		n = sscanf(args + 4, " %dx%d@%d", &w, &h, &hz);
		if (n >= 2 && w >= 640 && h >= 480) {
			desk->mon_w = w, desk->mon_h = h, desk->mon_hz = n == 3 ? hz : 0;
		} else {
			desk->mon_w = desk->mon_h = desk->mon_hz = 0;	/* "preferred" */
		}
		struct desk_output *o;
		wl_list_for_each(o, &desk->outputs, link) {
			if (o->virtual) continue;
			struct wlr_output_mode *m = desk_monitor_mode(desk, o->wo);
			if (!m) m = wlr_output_preferred_mode(o->wo);
			if (!m) continue;
			struct wlr_output_state state;
			wlr_output_state_init(&state);
			wlr_output_state_set_mode(&state, m);
			wlr_output_state_set_scale(&state, desk_auto_scale(desk, o->wo, m->width));
			bool ok = wlr_output_commit_state(o->wo, &state);
			wlr_output_state_finish(&state);
			wlr_log(WLR_INFO, "desktop: %s mode %dx%d@%.3f: %s", o->wo->name, m->width,
				m->height, m->refresh / 1000.0, ok ? "set" : "refused");
			if (ok && desk->out == o) {
				desk->out = NULL;	/* the desktop's size again */
				desk_use_output(desk, o);
				/* xfdesktop keeps its old size on a new mode of the same
				 * output (dp37): the session puts its pieces up again */
				if (desk->session_pid > 0) {
					desk_spawn("ztos-desktop-session", "--refresh", NULL);
				}
			}
		}
	} else if (!strcmp(args, "start")) {
		desk_session_start(desk);
		desk_state_write(desk);
	} else if (!strcmp(args, "stop")) {
		desk_session_stop(desk);
		desk_state_write(desk);
	} else if (sscanf(args, "move %127s", name) == 1) {
		struct zt_toplevel *t, *found = NULL;
		if (!strcmp(name, "shown")) {
			found = server->shown;
		} else {
			wl_list_for_each(t, &server->toplevels, link) {
				if (t->mapped && t->xdg_toplevel->app_id && !strcmp(t->xdg_toplevel->app_id, name)) {
					found = t;
					break;
				}
			}
		}
		if (found) desk_take_from_phone(server, found);
	} else if (sscanf(args, "phone %127s", name) == 1) {
		struct desk_view *v = desk_view_named(desk, name);
		if (v) desk_give_to_phone(v);
	} else if (sscanf(args, "watch %d", &n) == 1 && n >= 0 && n <= 3600) {
		desk->watch_until = now_ms() + (uint32_t)n * 1000u;
		struct desk_output *o;
		wl_list_for_each(o, &desk->outputs, link) {
			if (o->virtual) wlr_output_schedule_frame(o->wo);
		}
	} else if (sscanf(args, "pointer %lf %lf", &x, &y) == 2 && desk->out) {
		struct wlr_box b;
		wlr_output_layout_get_box(desk->layout, desk->out->wo, &b);
		wlr_cursor_warp(desk->cursor, NULL, b.x + x, b.y + y);
		desk_pointer_refocus(desk, now_ms());
	} else if (sscanf(args, "click %lf %lf %d", &x, &y, &n) >= 2 && desk->out) {
		/* a click there, for tests: left, or the button given (2 right, 3 middle) */
		int nb = sscanf(args, "click %lf %lf %d", &x, &y, &n);
		uint32_t button = nb == 3 && n == 2 ? BTN_RIGHT : nb == 3 && n == 3 ? BTN_MIDDLE : BTN_LEFT;
		struct wlr_box b;
		wlr_output_layout_get_box(desk->layout, desk->out->wo, &b);
		wlr_cursor_warp(desk->cursor, NULL, b.x + x, b.y + y);
		desk_pointer_refocus(desk, now_ms());
		struct wlr_pointer_button_event e = { .time_msec = now_ms(), .button = button,
			.state = WL_POINTER_BUTTON_STATE_PRESSED };
		desk->cursor_button.notify(&desk->cursor_button, &e);
		wlr_seat_pointer_notify_frame(server->seat);
		e.state = WL_POINTER_BUTTON_STATE_RELEASED;
		e.time_msec += 40;
		desk->cursor_button.notify(&desk->cursor_button, &e);
		wlr_seat_pointer_notify_frame(server->seat);
	} else if (!strcmp(args, "state")) {
		desk_state_write(desk);
	} else {
		wlr_log(WLR_INFO, "desktop: unknown control '%s'", args);
	}
}

static void desk_reload(struct zt_server *server) {
	struct zt_desk *desk = server->desk;
	if (!desk) {
		return;
	}
	desk_config_load(desk);
	struct desk_view *v;
	wl_list_for_each(v, &desk->views, link) {
		desk_title_draw(v, true);	/* the accent may have changed */
	}
}

/* ---- set-up ------------------------------------------------------------------------------------------ */

/* XFCE's panel and desktop draw on every monitor GTK tells them of, and take
 * the first as the primary: they are not told of the phone's panel at all.
 * (Apps are: an app moved to the phone needs it for its scale.) */
static bool desk_is_shell_client(const struct wl_client *client) {
	pid_t pid = 0;
	wl_client_get_credentials((struct wl_client *)client, &pid, NULL, NULL);
	if (pid <= 0) {
		return false;
	}
	char path[64], comm[32] = "";
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
	fclose(f);
	comm[strcspn(comm, "\n")] = 0;
	static const char *const shell[] = { "xfce4-panel", "xfdesktop", "wrapper-2.0", "xfce4-notifyd",
		"xfsettingsd", "panel-6-systray", NULL };
	for (int i = 0; shell[i]; i++) {
		if (!strncmp(comm, shell[i], 15)) return true;	/* (comm is cut at 15) */
	}
	return false;
}

static bool desk_global_filter(const struct wl_client *client, const struct wl_global *global, void *data) {
	struct zt_server *server = data;
	if (!server->output || wl_global_get_interface(global) != &wl_output_interface ||
			wl_global_get_user_data(global) != server->output->wlr_output) {
		return true;
	}
	return !desk_is_shell_client(client);
}

static void desk_find_headless(struct wlr_backend *backend, void *data) {
	struct wlr_backend **found = data;
	if (wlr_backend_is_headless(backend)) {
		*found = backend;
	}
}

static void desk_init(struct zt_server *server, struct wlr_compositor *compositor) {
	struct zt_desk *desk = calloc(1, sizeof(*desk));
	server->desk = desk;
	desk->server = server;
	wl_list_init(&desk->outputs);
	wl_list_init(&desk->views);
	wl_list_init(&desk->layers);
	wl_list_init(&desk->popups);
	desk_config_load(desk);

	/* the virtual output's backend, beside the panel's (DRM, or a window) */
	if (wlr_backend_is_multi(server->backend)) {
		wlr_multi_for_each_backend(server->backend, desk_find_headless, &desk->headless);
		if (!desk->headless) {
			desk->headless = wlr_headless_backend_create(server->loop);
			if (desk->headless && !wlr_multi_backend_add(server->backend, desk->headless)) {
				wlr_backend_destroy(desk->headless);
				desk->headless = NULL;
			}
		}
	}
	if (!desk->headless) {
		wlr_log(WLR_ERROR, "desktop: no headless backend; a monitor only");
	}

	wl_display_set_global_filter(server->wl_display, desk_global_filter, server);
	desk->layout = wlr_output_layout_create(server->wl_display);
	desk->scene = wlr_scene_create();
	float black[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
	desk->backdrop = wlr_scene_rect_create(&desk->scene->tree, 0, 0, black);
	desk->layer_tree[DL_BACKGROUND] = wlr_scene_tree_create(&desk->scene->tree);
	desk->layer_tree[DL_BOTTOM] = wlr_scene_tree_create(&desk->scene->tree);
	desk->win_tree = wlr_scene_tree_create(&desk->scene->tree);
	desk->layer_tree[DL_TOP] = wlr_scene_tree_create(&desk->scene->tree);
	desk->full_tree = wlr_scene_tree_create(&desk->scene->tree);
	desk->x_or_tree = wlr_scene_tree_create(&desk->scene->tree);
	desk->popup_tree = wlr_scene_tree_create(&desk->scene->tree);
	desk->layer_tree[DL_OVERLAY] = wlr_scene_tree_create(&desk->scene->tree);
	desk->menu_tree = wlr_scene_tree_create(&desk->scene->tree);
	desk->drag_tree = wlr_scene_tree_create(&desk->scene->tree);

	desk->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	desk->new_layer.notify = desk_new_layer;
	wl_signal_add(&desk->layer_shell->events.new_surface, &desk->new_layer);

	desk->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(desk->cursor, desk->layout);
	desk->xcursor = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24);
	desk->cursor_motion.notify = desk_cursor_motion;
	wl_signal_add(&desk->cursor->events.motion, &desk->cursor_motion);
	desk->cursor_motion_abs.notify = desk_cursor_motion_abs;
	wl_signal_add(&desk->cursor->events.motion_absolute, &desk->cursor_motion_abs);
	desk->cursor_button.notify = desk_cursor_button;
	wl_signal_add(&desk->cursor->events.button, &desk->cursor_button);
	desk->cursor_axis.notify = desk_cursor_axis;
	wl_signal_add(&desk->cursor->events.axis, &desk->cursor_axis);
	desk->cursor_frame.notify = desk_cursor_frame;
	wl_signal_add(&desk->cursor->events.frame, &desk->cursor_frame);
	desk->request_cursor.notify = desk_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &desk->request_cursor);
	desk->shape_mgr = wlr_cursor_shape_manager_v1_create(server->wl_display, 1);
	desk->request_shape.notify = desk_request_shape;
	wl_signal_add(&desk->shape_mgr->events.request_set_shape, &desk->request_shape);
	desk->primary_mgr = wlr_primary_selection_v1_device_manager_create(server->wl_display);
	desk->request_primary.notify = desk_request_primary;
	wl_signal_add(&server->seat->events.request_set_primary_selection, &desk->request_primary);
	desk->request_drag.notify = desk_request_drag;
	wl_signal_add(&server->seat->events.request_start_drag, &desk->request_drag);
	desk->start_drag.notify = desk_start_drag;
	wl_signal_add(&server->seat->events.start_drag, &desk->start_drag);

	/* ztdesk's input (and wayvnc's): routed to the desktop, never the phone */
	desk->vptr_mgr = wlr_virtual_pointer_manager_v1_create(server->wl_display);
	desk->new_vptr.notify = desk_new_vptr;
	wl_signal_add(&desk->vptr_mgr->events.new_virtual_pointer, &desk->new_vptr);
	desk->vkbd_mgr = wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	desk->new_vkbd.notify = desk_new_vkbd;
	wl_signal_add(&desk->vkbd_mgr->events.new_virtual_keyboard, &desk->new_vkbd);
	desk->ft_mgr = wlr_foreign_toplevel_manager_v1_create(server->wl_display);

#if WLR_HAS_XWAYLAND
	if (desk->xwayland_on) {
		/* lazy: Xwayland starts when the first X11 client connects, and
		 * costs nothing until then */
		desk->xwayland = wlr_xwayland_create(server->wl_display, compositor, true);
		if (desk->xwayland) {
			snprintf(desk->x_display, sizeof(desk->x_display), "%s", desk->xwayland->display_name);
			desk->x_ready.notify = desk_x_ready;
			wl_signal_add(&desk->xwayland->events.ready, &desk->x_ready);
			desk->x_new_surface.notify = desk_x_new_surface;
			wl_signal_add(&desk->xwayland->events.new_surface, &desk->x_new_surface);
		} else {
			wlr_log(WLR_INFO, "desktop: no Xwayland (X11 apps won't open)");
		}
	}
#endif
	desk->idle_timer = wl_event_loop_add_timer(server->loop, desk_idle_tick, desk);	/* (armed by a virtual output) */
	(void)desk_session_reap;
}

static void desk_session_ready(struct zt_server *server) {
	struct zt_desk *desk = server->desk;
	if (!desk) {
		return;
	}
	/* a stale state file from the last shell says nothing about this one */
	desk_state_write(desk);
	if (getenv("ZTMETRO_DESKTOP")) {
		/* ZTMETRO_DESKTOP=WxH (or 1): a virtual desktop from the start */
		int w, h;
		if (sscanf(getenv("ZTMETRO_DESKTOP"), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480) {
			desk->virt_w = w, desk->virt_h = h;
		}
		if (desk->headless) {
			wlr_headless_add_output(desk->headless, desk->virt_w, desk->virt_h);
		}
	}
}
