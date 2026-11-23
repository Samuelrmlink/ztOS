// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The action centre's quick-action editor (ROADMAP 8.49; see shade-edit.h).
 * Included at the end of ztmetro.c.
 *
 * Drawn as one layer over the panel's tiles and links (redrawn only when the
 * places change or a drag starts or ends) and, while a tile is carried, that
 * tile as a layer of its own under the finger. The places are the panel's own
 * (shade_tile_rect); the tray's tiles are smaller, five across, so that a
 * turned phone's side sheet (one portrait screen wide, the view's height)
 * holds two rows of them. In the theme's colours (shell_light).
 */

#define SE_BADGE	22.0	/* a tile's minus, in its top right corner */
#define SE_DRAG_SLOP	8.0	/* a finger moving this far carries the tile */
#define SE_HINT_H	34.0	/* the row with the hint and DONE */
#define SE_TRAY_HEAD	28.0	/* "add a tile" */

enum { SE_HIT_SLOT = 0, SE_HIT_BADGE = 100, SE_HIT_TRAY = 200, SE_HIT_DONE = 300 };

static struct {
	bool on;
	struct zt_layer layer, drag_layer;
	double top;			/* the layer's top in the panel */
	int carrying;			/* 0 nothing, 1 a place's tile, 2 a tray tile */
	int from;			/* the place, or the tile (q) from the tray */
	int over;			/* the place under the carried tile, -1 none */
	double cx, cy;			/* the carried tile's top left */
	double gx, gy;			/* where on it the finger holds */
	int tray[Q_COUNT];
	int n_tray;
} se;

#define se_light(server) shell_light(server)
#define SE_TRAY_ACROSS	5

/* the panel's last usable row: upright above its bar, turned (the bar is
 * along its side) the view's height */
static double se_bottom(struct zt_server *server) {
	return shade_panel_h(server) - (server->orient ? 0 : SHADE_HANDLE);
}

/* a point of the view in the panel's own points */
static void se_panel(struct zt_server *server, double vx, double vy, double *x, double *y) {
	if (server->orient != 0) {
		*x = vx - shade_sheet_x0(server);
		*y = vy;
	} else {
		*x = vx;
		*y = vy - (server->shade_h - SHADE_MAX_H);
	}
}

static bool shade_edit_active(void) {
	return se.on;
}

static double se_tile_w(struct zt_server *server, double *h) {
	double x, y, w;
	shade_tile_rect(server, 0, &x, &y, &w, h);
	return w;
}

static double se_hint_y(struct zt_server *server) {
	return shade_links_y(server) - 6;
}

static double se_tray_top(struct zt_server *server) {
	return se_hint_y(server) + SE_HINT_H + SE_TRAY_HEAD;
}

static void se_tray_tile(struct zt_server *server, int k, double *x, double *y, double *w, double *h) {
	*w = (server->width - 2 * SHADE_SIDE - (SE_TRAY_ACROSS - 1) * SHADE_GAP) / (double)SE_TRAY_ACROSS;
	*h = round(*w * 0.66);
	se_tray_rect(k, SE_TRAY_ACROSS, SHADE_SIDE, se_tray_top(server), *w, *h, SHADE_GAP, x, y);
}

static void se_slot_rects(struct zt_server *server, double rects[SE_SLOTS][4]) {
	for (int i = 0; i < SE_SLOTS; i++) {
		shade_tile_rect(server, i, &rects[i][0], &rects[i][1], &rects[i][2], &rects[i][3]);
	}
}

/* Keep the places: `quick` in the config, as Settings writes it. */
static void se_save(struct zt_server *server) {
	static const char *ids[Q_COUNT];
	for (int q = 0; q < Q_COUNT; q++) ids[q] = QUICK[q].id;
	char list[200];
	se_format(server->quick, ids, list, sizeof(list));
	config_keep_text("quick", list);
	wlr_log(WLR_INFO, "quick actions edited: %s", list);
}

static void se_dashed(cairo_t *cr, double x, double y, double w, double h, double ink, double alpha) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, ink, ink, ink, alpha);
	cairo_set_line_width(cr, 1.5);
	static const double dash[] = { 4, 3 };
	cairo_set_dash(cr, dash, 2, 0);
	cairo_rectangle(cr, x + 1, y + 1, w - 2, h - 2);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* The editor's layer: the places with their minus, the hint and DONE, and
 * the tray. */
static void se_draw(struct zt_server *server) {
	bool light = se_light(server);
	double ink = light ? 0 : 1, bg = light ? 1 : 0;
	se.top = SHADE_TOP - 6;
	double H = se_bottom(server) - se.top;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, H, &cr);
	cairo_set_source_rgb(cr, bg, bg, bg);
	cairo_paint(cr);
	cairo_translate(cr, 0, -se.top);	/* drawn in the panel's coordinates */
	double rects[SE_SLOTS][4];
	se_slot_rects(server, rects);
	for (int i = 0; i < SE_SLOTS; i++) {
		double x = rects[i][0], y = rects[i][1], w = rects[i][2], h = rects[i][3];
		bool lifted = se.carrying == 1 && se.from == i;
		if (server->quick[i] == Q_NONE || lifted) {
			se_dashed(cr, x, y, w, h, ink, se.over == i ? 0.9 : 0.4);
			continue;
		}
		cairo_save(cr);
		cairo_translate(cr, x, y);
		shade_quick_draw(server, cr, server->quick[i], w, h);
		cairo_restore(cr);
		if (se.over == i) {
			set_rgb(cr, server->accent);
			cairo_set_line_width(cr, 3);
			cairo_rectangle(cr, x + 1.5, y + 1.5, w - 3, h - 3);
			cairo_stroke(cr);
		}
		/* the minus: a disc in the corner, its bar across */
		double bx = x + w - SE_BADGE / 2 - 2, by = y + SE_BADGE / 2 + 2;
		cairo_set_source_rgb(cr, bg, bg, bg);
		cairo_arc(cr, bx, by, 9, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, ink, ink, ink);
		cairo_set_line_width(cr, 1.5);
		cairo_arc(cr, bx, by, 8, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_rectangle(cr, bx - 4.5, by - 0.9, 9, 1.8);
		cairo_fill(cr);
	}
	/* the hint, and DONE */
	double hy = se_hint_y(server);
	cairo_set_source_rgba(cr, ink, ink, ink, 0.6);
	draw_text(cr, "Drag to arrange \xc2\xb7 \xe2\x88\x92 removes", FONT_REGULAR, 11, 14, hy + 10,
		server->width - 110, 0, false);
	double done_w = text_width("DONE", FONT_SEMIBOLD, 12) + 28;
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, server->width - 10 - done_w, hy + 2, done_w, SE_HINT_H - 6);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_text(cr, "DONE", FONT_SEMIBOLD, 12, server->width - 10 - done_w, hy + 2, done_w, SE_HINT_H - 6, true);
	/* the tray */
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, se.n_tray ? "add a tile" : "every tile is in use", FONT_SEMILIGHT, 16, 14,
		se_tray_top(server) - SE_TRAY_HEAD + 2, 0, 0, false);
	for (int k = 0; k < se.n_tray; k++) {
		double x, y, w, h;
		se_tray_tile(server, k, &x, &y, &w, &h);
		if (y + h > se_bottom(server)) break;	/* (a short panel: what fits) */
		if (se.carrying == 2 && se.from == se.tray[k]) {
			se_dashed(cr, x, y, w, h, ink, 0.4);
			continue;
		}
		cairo_save(cr);
		cairo_translate(cr, x, y);
		shade_quick_draw(server, cr, se.tray[k], w, h);
		cairo_restore(cr);
		/* a plus over it: a tap adds it */
		double bx = x + w - SE_BADGE / 2 - 2, by = y + SE_BADGE / 2 + 2;
		cairo_set_source_rgb(cr, ink, ink, ink);
		cairo_rectangle(cr, bx - 4.5, by - 0.9, 9, 1.8);
		cairo_rectangle(cr, bx - 0.9, by - 4.5, 1.8, 9);
		cairo_fill(cr);
	}
	zt_buffer_show(server, &se.layer, buffer, cr, server->width, H);
	damage(server);
}

static void se_refresh(struct zt_server *server) {
	se.n_tray = se_tray(server->quick, Q_COUNT, Q_NONE, se.tray);
	se_draw(server);
}

static void shade_edit_begin(struct zt_server *server) {
	if (se.on || server->width <= 0) return;
	se.on = true;
	se.carrying = 0;
	se.over = -1;
	hap(server, HAP_TAP, HG_SYSTEM);
	se_refresh(server);
	wlr_log(WLR_INFO, "quick actions: editing");
}

static void shade_edit_end(struct zt_server *server) {
	if (!se.on) return;
	se.on = false;
	se.carrying = 0;
	se.over = -1;
	draw_shade(server);	/* the tiles as they now are */
	damage(server);
}

static int shade_edit_links_hit(struct zt_server *server, double x) {
	double edit_w = 70;
	if (fabs(x - server->width / 2.0) < edit_w / 2) return 11;
	return x < server->width / 2.0 ? 8 : 9;
}

static void shade_edit_draw_button(struct zt_server *server, cairo_t *cr, double links) {
	/* a pencil and EDIT, between CLEAR ALL and ALL SETTINGS */
	double tw = text_width("EDIT", FONT_SEMIBOLD, 10), x = server->width / 2.0 - (tw + 16) / 2;
	cairo_save(cr);
	cairo_set_line_width(cr, 1.4);
	cairo_move_to(cr, x + 1, links + 12);
	cairo_line_to(cr, x + 2, links + 9);
	cairo_line_to(cr, x + 9, links + 2);
	cairo_line_to(cr, x + 11, links + 4);
	cairo_line_to(cr, x + 4, links + 11);
	cairo_close_path(cr);
	cairo_stroke(cr);
	cairo_restore(cr);
	draw_text(cr, "EDIT", FONT_SEMIBOLD, 10, x + 16, links, 0, 0, false);
}

/* What is under (x, y) of the panel while editing. */
static int se_hit(struct zt_server *server, double x, double y) {
	double rects[SE_SLOTS][4];
	se_slot_rects(server, rects);
	for (int i = 0; i < SE_SLOTS; i++) {
		const double *r = rects[i];
		if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
			bool badge = server->quick[i] != Q_NONE && x >= r[0] + r[2] - SE_BADGE - 6 && y < r[1] + SE_BADGE + 6;
			return (badge ? SE_HIT_BADGE : SE_HIT_SLOT) + i;
		}
	}
	double hy = se_hint_y(server);
	if (y >= hy - 4 && y < hy + SE_HINT_H + 4 && x >= server->width - 100) return SE_HIT_DONE;
	for (int k = 0; k < se.n_tray; k++) {
		double tx, ty, tw, th;
		se_tray_tile(server, k, &tx, &ty, &tw, &th);
		if (x >= tx && x < tx + tw && y >= ty && y < ty + th) return SE_HIT_TRAY + k;
	}
	return -1;
}

/* The carried tile's own layer, drawn once as it is picked up. */
static void se_pick_up(struct zt_server *server, int kind, int from, double x, double y, double tx, double ty) {
	double w, h;
	w = se_tile_w(server, &h);	/* a place's size: where it is going */
	se.carrying = kind;
	se.from = from;
	se.gx = x - tx;
	se.gy = y - ty;
	se.cx = tx;
	se.cy = ty;
	se.over = -1;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	shade_quick_draw(server, cr, kind == 1 ? server->quick[from] : from, w, h);
	zt_buffer_show(server, &se.drag_layer, buffer, cr, w, h);
	hap(server, HAP_TICK, HG_SYSTEM);
	se_draw(server);
}

static bool shade_edit_long_press(struct zt_server *server, struct touch_point *p) {
	if (!server->shade_open || p->index < 0 || p->index >= SE_SLOTS || quick_holds(server->quick[p->index])) {
		return false;
	}
	double tx, ty, tw, th, panel_x, panel_y;
	se_panel(server, p->x, p->y, &panel_x, &panel_y);
	shade_tile_rect(server, p->index, &tx, &ty, &tw, &th);
	shade_tile_release(server, p->index);
	server->shade_pressed = -1;
	p->long_pressed = true;
	shade_edit_begin(server);
	if (server->quick[p->index] != Q_NONE) {
		se_pick_up(server, 1, p->index, panel_x, panel_y, tx, ty);
	}
	p->index = SE_HIT_SLOT + p->index;
	p->moved = false;
	p->x0 = p->x;
	p->y0 = p->y;
	return true;
}

static void shade_edit_down(struct zt_server *server, struct touch_point *p, double x, double y) {
	p->index = x < 0 ? -1 : se_hit(server, x, y);	/* (x < 0: beside a turned sheet) */
	p->long_pressed = false;
}

static void shade_edit_move(struct zt_server *server, struct touch_point *p, double vx, double vy) {
	double x, y, x0, y0;
	se_panel(server, vx, vy, &x, &y);
	se_panel(server, p->x0, p->y0, &x0, &y0);
	if (!se.carrying && hypot(x - x0, y - y0) > SE_DRAG_SLOP) {
		double tx, ty, tw, th;
		if (p->index >= SE_HIT_SLOT && p->index < SE_HIT_SLOT + SE_SLOTS &&
				server->quick[p->index - SE_HIT_SLOT] != Q_NONE) {
			shade_tile_rect(server, p->index - SE_HIT_SLOT, &tx, &ty, &tw, &th);
			se_pick_up(server, 1, p->index - SE_HIT_SLOT, x0, y0, tx, ty);
		} else if (p->index >= SE_HIT_TRAY && p->index < SE_HIT_TRAY + se.n_tray) {
			se_tray_tile(server, p->index - SE_HIT_TRAY, &tx, &ty, &tw, &th);
			/* held where it was held, at a place's size */
			double pw, ph;
			pw = se_tile_w(server, &ph);
			se_pick_up(server, 2, se.tray[p->index - SE_HIT_TRAY], x0, y0,
				x0 - (x0 - tx) * pw / tw, y0 - (y0 - ty) * ph / th);
		}
	}
	if (!se.carrying) return;
	double w, h;
	w = se_tile_w(server, &h);
	se.cx = x - se.gx;
	se.cy = y - se.gy;
	double rects[SE_SLOTS][4];
	se_slot_rects(server, rects);
	int over = se_slot_at(rects, se.cx + w / 2, se.cy + h / 2, SHADE_GAP + 6);
	if (over != se.over) {
		se.over = over;
		se_draw(server);	/* the place it would go, outlined */
	}
	damage(server);
}

static void shade_edit_up(struct zt_server *server, struct touch_point *p, bool cancelled) {
	int index = p->index;
	if (se.carrying) {
		int kind = se.carrying, from = se.from, over = se.over;
		double w, h;
		w = se_tile_w(server, &h);
		bool in_tray = se.cy + h / 2 >= se_hint_y(server) + SE_HINT_H;
		se.carrying = 0;
		se.over = -1;
		bool changed = false;
		if (!cancelled && kind == 1 && over >= 0 && over != from) {
			se_move(server->quick, from, over);
			changed = true;
		} else if (!cancelled && kind == 1 && over < 0 && in_tray) {
			se_remove(server->quick, from, Q_NONE);
			changed = true;
		} else if (!cancelled && kind == 2 && over >= 0) {
			se_place(server->quick, over, from, Q_NONE);
			changed = true;
		}
		if (changed) {
			hap(server, HAP_TAP, HG_SYSTEM);
			se_save(server);
		}
		se_refresh(server);
		return;
	}
	if (cancelled || p->moved) return;
	if (index == SE_HIT_DONE) {
		hap(server, HAP_TAP, HG_SYSTEM);
		shade_edit_end(server);
	} else if (index >= SE_HIT_BADGE && index < SE_HIT_BADGE + SE_SLOTS) {
		hap(server, HAP_TAP, HG_SYSTEM);
		se_remove(server->quick, index - SE_HIT_BADGE, Q_NONE);
		se_save(server);
		se_refresh(server);
	} else if (index >= SE_HIT_TRAY && index < SE_HIT_TRAY + se.n_tray) {
		if (se_add(server->quick, se.tray[index - SE_HIT_TRAY], Q_NONE)) {
			hap(server, HAP_TAP, HG_SYSTEM);
			se_save(server);
			se_refresh(server);
		} else {
			hap(server, HAP_HEAVY, HG_SYSTEM);	/* no empty place: take one out first */
		}
	}
}

static void shade_edit_draw(struct zt_server *server, float x0) {
	if (!se.on || !se.layer.tex) return;
	layer_draw(server, &se.layer, x0, (float)se.top, 1.0f);
	if (se.carrying && se.drag_layer.tex) {
		/* the carried tile, a little larger and lifted */
		float m[16];
		double w = se.drag_layer.w, h = se.drag_layer.h;
		mat_identity(m);
		mat_translate(m, (float)(x0 + se.cx + w / 2), (float)(se.cy + h / 2));
		mat_scale(m, 1.08f, 1.08f);
		mat_translate(m, (float)(-w / 2), (float)(-h / 2));
		mat_scale(m, (float)w, (float)h);
		quad(server, GL_TEXTURE_2D, se.drag_layer.tex, m, 0.92f, true, false,
			se.drag_layer.tw ? (float)se.drag_layer.pw / se.drag_layer.tw : 1.0f,
			se.drag_layer.th ? (float)se.drag_layer.ph / se.drag_layer.th : 1.0f);
	}
}
