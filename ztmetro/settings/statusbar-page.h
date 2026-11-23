/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Settings > customization > status bar (ROADMAP 8.77): the status bar's
 * icons in a list, each with a switch and a handle to drag it to another
 * place, and the Bluetooth icon's own page, a colour for each of its states
 * and what shows while sound streams. The model is ../statusbar.h; every
 * change goes to the shell at once as `look KEY VALUE` on its control pipe
 * and into the file without a SIGHUP (config_save_look).
 *
 * Included into ztsettings.c, after the appearance pages: it uses app, the
 * control pipe (ctl_tell), the config's save and the shared list picker.
 */
#ifndef ZTSETTINGS_STATUSBAR_PAGE_H
#define ZTSETTINGS_STATUSBAR_PAGE_H

#define SB_ACTIONS	121000
enum {
	SB_ACT_SHOW = SB_ACTIONS,	/* arg: the icon, its switch */
	SB_ACT_COLOUR_OPEN,		/* arg: a state of Bluetooth's, or SB_PICK_DOT: its picker */
	SB_ACT_COLOUR,			/* arg: a swatch of SB_PALETTE, chosen */
	SB_ACT_STREAM,			/* the streaming style's list picker (arg -1 opens it) */
	SB_ACT_RESET,			/* Bluetooth's colours and style as they were */
	SB_ACT_ORDER_RESET,		/* every icon shown, in the usual order */
	SB_ACTIONS_END
};
#define SB_PICK_DOT	SB_MAX_STATES	/* the colour picker is the dot's */

/* the shell told every key; it redraws only for one that changed */
static void sb_tell_all(void) {
	for (int k = 0; k < sb_n_keys(); k++) {
		char line[400], value[300];
		sb_format(&app.sb, k, value, sizeof(value));
		snprintf(line, sizeof(line), "look %s %s\n", sb_key_name(k), value);
		ctl_tell(line);
	}
}

static void sb_changed(void) {
	sb_tell_all();
	config_save_look();
}

/* ---- the icons, small, as the bar draws them --------------------------------- */

#define SB_ROW_H	66.0
#define SB_HANDLE_W	52.0	/* the handle's strip at the left of each row */

static uint32_t sb_ink(void) { return zmc_fg_color(); }

/* halfway to the background: the shell's dim */
static uint32_t sb_dim(void) {
	uint32_t f = zmc_fg_color(), b = zmc_bg_color(), out = 0;
	for (int sh = 0; sh <= 16; sh += 8) out |= ((((f >> sh) & 0xff) + ((b >> sh) & 0xff)) / 2) << sh;
	return out;
}

static bool sb_bt_colour(int state, uint32_t *out) {
	return sb_resolve(&app.sb, SB_COLOURED_BT, state, sb_ink(), sb_dim(), ACCENTS[app.accent].color, out);
}

/* The Bluetooth rune (the shell's draw_status_bt), ink `h` tall from x on base. */
static double sb_draw_rune(cairo_t *cr, double x, double base, double h) {
	double lw = fmax(1.0, round(h * 0.115 * 2) / 2);
	double u = (h - lw) / 1.8;
	double cx = x + lw / 2 + 0.45 * u, cy = base - lw / 2 - 0.9 * u;
	cairo_save(cr);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
	cairo_move_to(cr, cx - u * 0.45, cy - u * 0.42);
	cairo_line_to(cr, cx + u * 0.4, cy + u * 0.42);
	cairo_line_to(cr, cx, cy + u * 0.9);
	cairo_line_to(cr, cx, cy - u * 0.9);
	cairo_line_to(cr, cx + u * 0.4, cy - u * 0.42);
	cairo_line_to(cr, cx - u * 0.45, cy + u * 0.42);
	cairo_stroke(cr);
	cairo_restore(cr);
	return 0.85 * u + lw;
}

/* One icon `h` tall standing on base from x, in the source colour (Bluetooth
 * in its connected colour); returns its width. */
static double sb_draw_icon(cairo_t *cr, int icon, double x, double base, double h) {
	switch (icon) {
	case SB_SIGNAL: {
		double w = round(h * 1.5), pitch = w / 4.8;
		for (int i = 0; i < 5; i++) {
			double bh = h * (0.2 + 0.2 * i);
			cairo_rectangle(cr, x + i * pitch, base - bh, pitch * 0.8, bh);
		}
		cairo_fill(cr);
		return w;
	}
	case SB_DATA: {
		cairo_save(cr);
		double w = zmc_text_width("4G", FONT_SEMIBOLD, h * 1.25);
		zmc_text(cr, "4G", FONT_SEMIBOLD, h * 1.25, x, base - h * 1.25 * 1.02, 0);
		cairo_restore(cr);
		return w;
	}
	case SB_WIFI: {
		static const double outer[3] = { 1.0, 0.756, 0.513 };
		double cx = x + h, cy = base, band = h * 0.115;
		cairo_save(cr);
		cairo_set_line_width(cr, band);
		for (int i = 0; i < 3; i++) {
			cairo_new_sub_path(cr);
			cairo_arc(cr, cx, cy, h * outer[i] - band / 2, -M_PI, -M_PI / 2);
			cairo_stroke(cr);
		}
		cairo_arc(cr, cx - h * 0.115, cy - h * 0.115, h * 0.115, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_restore(cr);
		return h;
	}
	case SB_LOCATION: {
		double r = h * 0.36;
		cairo_save(cr);
		cairo_set_line_width(cr, fmax(1.0, round(h * 0.11)));
		cairo_new_sub_path(cr);
		cairo_arc(cr, x + r, base - h / 2, r, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, x + r, base - h / 2, r * 0.45, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_restore(cr);
		return 2 * r;
	}
	case SB_BT: {
		uint32_t c;
		cairo_save(cr);
		if (sb_bt_colour(SB_BT_CONNECTED, &c)) zmc_set_rgb(cr, c);
		double w = sb_draw_rune(cr, x, base, h * 1.08);
		cairo_restore(cr);
		return w;
	}
	case SB_BATTERY: {
		double cell = h * 0.87, bw = round(cell * 1.95), line = fmax(1.0, round(cell * 0.085));
		cairo_save(cr);
		cairo_set_line_width(cr, line);
		cairo_rectangle(cr, x + line / 2, base - cell + line / 2, bw - line, cell - line);
		cairo_stroke(cr);
		cairo_rectangle(cr, x + 2 * line, base - cell + 2 * line, (bw - 4 * line) * 0.7, cell - 4 * line);
		cairo_rectangle(cr, x + bw + line, base - cell * 0.78, round(cell * 0.16), cell * 0.56);
		cairo_fill(cr);
		cairo_restore(cr);
		return bw + line + round(cell * 0.16);
	}
	case SB_CLOCK: {
		double size = h * 1.13;
		double w = zmc_text_width("12:34", FONT_REGULAR, size);
		zmc_text(cr, "12:34", FONT_REGULAR, size, x, base - size * 1.02, 0);
		return w;
	}
	}
	return 0;
}

/* The bar as it will look: on the shell's background, the icons shown in
 * their order, the battery and the clock at the right. */
static double sb_preview(cairo_t *cr, double y, const struct zt_statusbar *s) {
	double h = 15, bar_h = 34, x = MARGIN + 12, base = y + bar_h / 2 + h / 2;
	double w = zmc.width - 2 * MARGIN;
	zmc_bg(cr);
	cairo_rectangle(cr, MARGIN, y, w, bar_h);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.5);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, MARGIN + 0.5, y + 0.5, w - 1, bar_h - 1);
	cairo_stroke(cr);
	zmc_fg(cr);
	for (int i = 0; i < SB_N_MOVABLE; i++) {
		int icon = s->order[i];
		if (!s->shown[icon]) continue;
		zmc_fg(cr);
		x += sb_draw_icon(cr, icon, x, base, h) + round(h * 0.5);
	}
	zmc_fg(cr);
	double clock_w = zmc_text_width("12:34", FONT_REGULAR, h * 1.13);
	double right = MARGIN + w - 12;
	sb_draw_icon(cr, SB_CLOCK, right - clock_w, base, h);
	sb_draw_icon(cr, SB_BATTERY, right - clock_w - h * 0.9 - 32, base, h);
	return y + bar_h + 18;
}

/* ---- the list: a switch each, and a handle to drag ------------------------------ */

static struct {
	bool down;
	int from, to;		/* the place it was lifted from, and where it would go now */
	double top;		/* the list's first row, on the page */
	double grab;		/* the finger's distance below the lifted row's top */
	double finger;		/* the finger, on the page */
} sbd;

static double sb_list_top;	/* where the rows started, last drawn */

/* the three bars of a handle, centred in its strip */
static void sb_handle(cairo_t *cr, double y, bool lifted) {
	double cx = MARGIN + 14, cy = y + SB_ROW_H / 2 - 4;
	if (lifted) zmc_accent(cr);
	else cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	for (int i = -1; i <= 1; i++) cairo_rectangle(cr, cx - 10, cy + i * 6 - 1, 20, 2.5);
	cairo_fill(cr);
}

/* an icon in a row's slot, 34 points across at most */
static void sb_row_icon(cairo_t *cr, int icon, double x, double base) {
	double h = icon == SB_CLOCK ? 8 : icon == SB_BATTERY ? 13 : icon == SB_DATA ? 14 : 16;
	sb_draw_icon(cr, icon, x, base, h);
}

static void sb_row(cairo_t *cr, int icon, double y, bool lifted, bool hits) {
	bool shown = app.sb.shown[icon];
	if (lifted) {
		/* held up: a band behind it, a little out from the page */
		cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.22);
		cairo_rectangle(cr, MARGIN - 6, y - 4, zmc.width - 2 * MARGIN + 12, SB_ROW_H - 4);
		cairo_fill(cr);
	}
	sb_handle(cr, y, lifted);
	double tx = MARGIN + SB_HANDLE_W;
	/* the icon itself, in a box as wide as the widest */
	if (shown) zmc_fg(cr);
	else cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
	sb_row_icon(cr, icon, tx, y + 28);
	tx += 40;
	double tw = zmc.width - MARGIN - 64 - tx;
	if (shown) zmc_fg(cr);
	else cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	zmc_text(cr, SB_ICONS[icon].name, FONT_SEMILIGHT, 21, tx, y + 2, tw);
	if (icon == SB_BT && shown) {
		zmc_accent(cr);
		zmc_text(cr, "Colours and streaming ›", FONT_REGULAR, 12.5, tx, y + 32, tw);
	} else {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text_line(cr, shown ? SB_ICONS[icon].about : "Hidden", FONT_REGULAR, 12.5, tx, y + 32, tw);
	}
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 10, shown);
	if (hits) {
		zmc_add_hit(zmc.width - MARGIN - 70, y - 2, 70 + MARGIN, SB_ROW_H - 6, SB_ACT_SHOW, icon);
		if (icon == SB_BT && shown) {
			zmc_add_hit(tx - 40, y - 2, zmc.width - MARGIN - 70 - (tx - 40), SB_ROW_H - 6, ACT_OPEN_PAGE,
				PAGE_STATUSBAR_BT);
		} else {
			/* the row is one strip of the page's turn */
			zmc_add_hit(0, y - 2, zmc.width, SB_ROW_H - 6, 0, 0);
		}
	}
}

static double draw_statusbar(cairo_t *cr) {
	double y = zmc_header(cr, "status bar");
	/* while one is lifted, the bar shows where it would go */
	struct zt_statusbar view = app.sb;
	if (sbd.down) sb_move(&view, sbd.from, sbd.to);
	y = sb_preview(cr, y, &view);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "Hold ≡ and drag an icon to move it. The battery and the clock stay at the right.",
		FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN) + 16;
	sb_list_top = y;
	for (int i = 0; i < SB_N_MOVABLE; i++) {
		int icon = view.order[i];
		if (sbd.down && icon == app.sb.order[sbd.from]) continue;	/* drawn last, over the rest */
		sb_row(cr, icon, y + i * SB_ROW_H, false, !sbd.down);
	}
	if (sbd.down) {
		double ly = sbd.finger - sbd.grab;
		double lo = sb_list_top, hi = sb_list_top + (SB_N_MOVABLE - 1) * SB_ROW_H;
		sb_row(cr, app.sb.order[sbd.from], ly < lo ? lo : ly > hi ? hi : ly, true, false);
	}
	y += SB_N_MOVABLE * SB_ROW_H;
	/* the two that do not move */
	for (int icon = SB_BATTERY; icon <= SB_CLOCK; icon++) {
		cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
		sb_row_icon(cr, icon, MARGIN + SB_HANDLE_W, y + 28);
		zmc_fg(cr);
		zmc_text(cr, SB_ICONS[icon].name, FONT_SEMILIGHT, 21, MARGIN + SB_HANDLE_W + 40, y + 2, 0);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, SB_ICONS[icon].about, FONT_REGULAR, 12.5, MARGIN + SB_HANDLE_W + 40, y + 32, 0);
		zmc_add_hit(0, y - 2, zmc.width, SB_ROW_H - 6, 0, 0);
		y += SB_ROW_H;
	}
	y += 6;
	y = zmc_entry(cr, y, "reset", "Every icon shown, in the usual order", SB_ACT_ORDER_RESET, 0);
	return y + 20;
}

/* A finger on a handle: the row is lifted and follows it, the others make
 * room, and where it is let go it stays. Elsewhere the page has the finger. */
static bool sb_touch(int phase, int id, double x, double y, uint32_t time) {
	(void)id;
	(void)time;
	double py = y + zmc.scroll;
	if (phase == 0) {
		if (y < zmc_header_h() || x > MARGIN + SB_HANDLE_W || py < sb_list_top ||
				py >= sb_list_top + SB_N_MOVABLE * SB_ROW_H) {
			return false;
		}
		sbd.down = true;
		sbd.from = sbd.to = (int)((py - sb_list_top) / SB_ROW_H);
		sbd.top = sb_list_top;
		sbd.grab = py - (sb_list_top + sbd.from * SB_ROW_H);
		sbd.finger = py;
		zmc_haptic("tick");
		zmc_redraw();
		return true;
	}
	if (!sbd.down) return false;
	if (phase == 1) {
		sbd.finger = py;
		int to = (int)lround((py - sbd.grab - sbd.top) / SB_ROW_H);
		to = to < 0 ? 0 : to >= SB_N_MOVABLE ? SB_N_MOVABLE - 1 : to;
		if (to != sbd.to) {
			sbd.to = to;
			zmc_haptic("tick");
		}
		zmc_redraw();
		return true;
	}
	/* let go (2), or taken away (3): only a lift puts it there */
	sbd.down = false;
	if (phase == 2 && sbd.to != sbd.from) {
		sb_move(&app.sb, sbd.from, sbd.to);
		sb_changed();
	}
	zmc_redraw();
	return true;
}

/* ---- Bluetooth's page ------------------------------------------------------------ */

static int sb_pick;		/* the state whose colour the picker chooses, or SB_PICK_DOT */
static double sb_pick_box[SB_MAX_STATES + 1];	/* each state's row, on the page */
static int sb_choice;
static bool sb_board_unsaved;

static int *sb_pick_slot(void) {
	return sb_pick == SB_PICK_DOT ? &app.sb.bt_dot : &app.sb.colour[SB_COLOURED_BT][sb_pick];
}

/* a swatch: its colour, or what the theme's choices mean, with its name */
static void sb_swatch(cairo_t *cr, int i, double x, double y, double w, double h, int state) {
	int c = SB_PALETTE[i].colour;
	uint32_t rgb = 0;
	bool hidden = false;
	if (c == SB_C_DEFAULT) {
		/* what default means here */
		if (state == SB_PICK_DOT) rgb = ACCENTS[app.accent].color;
		else {
			struct zt_statusbar d = app.sb;
			d.colour[SB_COLOURED_BT][state] = SB_C_DEFAULT;
			hidden = !sb_resolve(&d, SB_COLOURED_BT, state, sb_ink(), sb_dim(), ACCENTS[app.accent].color, &rgb);
		}
	} else {
		hidden = !sb_colour_rgb(c, sb_ink(), sb_dim(), ACCENTS[app.accent].color, 0, &rgb);
	}
	if (hidden) {
		cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, x + 1, y + 1, w - 2, h - 2);
		cairo_move_to(cr, x + 4, y + h - 4);
		cairo_line_to(cr, x + w - 4, y + 4);
		cairo_stroke(cr);
	} else {
		zmc_set_rgb(cr, rgb);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		if (rgb == zmc_bg_color()) {
			cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
			cairo_set_line_width(cr, 1);
			cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
			cairo_stroke(cr);
		}
	}
	if (i < SB_PALETTE_THEMED && h > 30) {
		/* the theme's own: named, on a band so it reads over any colour */
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
		cairo_rectangle(cr, x, y + h - 18, w, 18);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, SB_PALETTE[i].label, FONT_REGULAR, 11, x + 4, y + h - 16, w - 8);
	}
}

static void sb_board_cell(cairo_t *cr, int i, double w, double h) {
	sb_swatch(cr, i, 0, 0, w, h, sb_pick);
	if (*sb_pick_slot() == SB_PALETTE[i].colour) {
		zmc_fg(cr);
		cairo_set_line_width(cr, 3);
		cairo_rectangle(cr, 1.5, 1.5, w - 3, h - 3);
		cairo_stroke(cr);
	}
}

static const char *sb_colour_name(int c, char *out, size_t size) {
	int i = sb_swatch_of(c);
	if (i >= 0) snprintf(out, size, "%s", SB_PALETTE[i].label);
	else snprintf(out, size, "#%06x", (unsigned)c & 0xffffff);
	return out;
}

static const char *sb_pick_title(int pick) {
	return pick == SB_PICK_DOT ? "Streaming dot" : SB_BT_STATES[pick].name;
}

/* the box lit while the picker is up: what it chooses for */
static void sb_board_box(cairo_t *cr, double w, double h) {
	zmc_accent(cr);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, sb_pick_title(sb_pick), FONT_REGULAR, 16, 12, 10, 0);
}

static void sb_board_closed(void) {
	if (sb_board_unsaved) {
		sb_board_unsaved = false;
		config_save_look();
	}
}

/* chosen: in, and the shell told (the file once the picker has gone) */
static void sb_board_apply(void) {
	*sb_pick_slot() = SB_PALETTE[sb_choice].colour;
	sb_tell_all();
	sb_board_unsaved = true;
}

static void sb_board_open(int pick) {
	sb_pick = pick;
	double gap = 8, cols = 5;
	double side = (zmc.width - 2 * MARGIN - (cols - 1) * gap) / cols;
	static char label[64];
	snprintf(label, sizeof(label), "BLUETOOTH: %s", sb_pick_title(pick));
	for (char *c = label; *c; c++) *c = (char)toupper((unsigned char)*c);
	struct zmc_board b = { .count = SB_N_PALETTE, .cols = (int)cols,
		.x0 = MARGIN, .top = 46, .cell_w = side, .cell_h = side, .gap = gap,
		.draw_cell = sb_board_cell, .action = SB_ACT_COLOUR,
		.label = label, .picker = true, .draw_box = sb_board_box,
		.box_x = MARGIN, .box_y = sb_pick_box[pick] - zmc.scroll, .box_w = zmc.width - 2 * MARGIN, .box_h = 46,
		.closed = sb_board_closed };
	zmc_board_open(&b);
}

/* one state: the icon as it will look, its name, and its colour; tapped,
 * the palette */
static double sb_state_row(cairo_t *cr, double y, int pick) {
	double w = zmc.width - 2 * MARGIN, h = 46;
	sb_pick_box[pick] = y;
	zmc_fg(cr);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, MARGIN + 1, y + 1, w - 2, h - 2);
	cairo_stroke(cr);
	/* the sample: on the bar's own background */
	zmc_bg(cr);
	cairo_rectangle(cr, MARGIN + 6, y + 6, 34, h - 12);
	cairo_fill(cr);
	uint32_t c;
	char name[32];
	if (pick == SB_PICK_DOT) {
		uint32_t icon = 0;
		sb_bt_colour(SB_BT_STREAMING, &icon);
		zmc_set_rgb(cr, icon);
		double rw = sb_draw_rune(cr, MARGIN + 13, y + h - 13, 20);
		if (sb_colour_rgb(app.sb.bt_dot, sb_ink(), sb_dim(), ACCENTS[app.accent].color, ACCENTS[app.accent].color, &c)) {
			zmc_set_rgb(cr, c);
			cairo_arc(cr, MARGIN + 13 + rw + 3, y + h - 13 - 20 + 3, 3.2, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	} else if (sb_bt_colour(pick, &c)) {
		zmc_set_rgb(cr, c);
		sb_draw_rune(cr, MARGIN + 16, y + h - 13, 20);
	}
	zmc_fg(cr);
	zmc_text(cr, sb_pick_title(pick), FONT_REGULAR, 16, MARGIN + 52, y + 4, w - 60);
	int v = pick == SB_PICK_DOT ? app.sb.bt_dot : app.sb.colour[SB_COLOURED_BT][pick];
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	if (v == SB_C_DEFAULT) {
		/* what default is, here */
		const struct sb_state_def *d = pick == SB_PICK_DOT ? NULL : &SB_BT_STATES[pick];
		snprintf(name, sizeof(name), "Default (%s)", !d ? "accent" : d->like >= 0 ? "as connected" :
			d->def == SB_C_HIDDEN ? "hidden" : d->def == SB_C_DIM ? "dim" : "white");
	} else {
		sb_colour_name(v, name, sizeof(name));
	}
	zmc_text(cr, name, FONT_REGULAR, 12.5, MARGIN + 52, y + 25, w - 60);
	zmc_add_hit(0, y - 4, zmc.width, h + 8, SB_ACT_COLOUR_OPEN, pick);
	return y + h + 14;
}

static double draw_statusbar_bt(cairo_t *cr) {
	double y = zmc_header(cr, "bluetooth icon");
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "A colour for each state. Default is the icon as it was: dim while on, white with a "
		"device connected, hidden while off.", FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN) + 16;
	for (int s = 0; s < SB_BT_N_STATES; s++) y = sb_state_row(cr, y, s);
	y += 6;
	y = picker(cr, y, "While audio streams", SB_STREAM_NAMES, SB_N_STREAM, app.sb.bt_stream, SB_ACT_STREAM);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	const char *about = app.sb.bt_stream == SB_STREAM_DOT ? "A small dot at the icon's top right" :
		app.sb.bt_stream == SB_STREAM_COLOUR ? "The icon in its streaming colour, nothing else" :
		zt_anim_off(&app.anim, ZT_ANIM_BARS) ?
			"Animations for the status bar are off (display, animations): shown steady instead" :
			"The icon slowly fades and returns, about every three seconds";
	y += zmc_text(cr, about, FONT_REGULAR, 12.5, MARGIN, y - 4, zmc.width - 2 * MARGIN) + 14;
	if (app.sb.bt_stream == SB_STREAM_DOT) y = sb_state_row(cr, y, SB_PICK_DOT);
	y += 6;
	y = zmc_entry(cr, y, "reset", "The icon as it was: dim, white, an accent dot", SB_ACT_RESET, 0);
	return y + 20;
}

/* customization's row */
static const char *sb_summary(char *out, size_t size) {
	int hidden = 0;
	for (int i = 0; i < SB_N_MOVABLE; i++) hidden += !app.sb.shown[i];
	bool moved = false;
	for (int i = 0; i < SB_N_MOVABLE; i++) moved = moved || app.sb.order[i] != i;
	const char *bt = app.sb.bt_stream == SB_STREAM_PULSE ? ", Bluetooth pulses" : "";
	if (!hidden && !moved) snprintf(out, size, "Every icon, in the usual order%s", bt);
	else if (hidden) snprintf(out, size, "%d hidden%s%s", hidden, moved ? ", rearranged" : "", bt);
	else snprintf(out, size, "Rearranged%s", bt);
	return out;
}

/* a tap on either page; true if it was one of these */
static bool sb_activate(int action, int arg) {
	if (action < SB_ACTIONS || action >= SB_ACTIONS_END) return false;
	if (action == SB_ACT_STREAM && arg < 0) {
		open_picker = open_picker == action ? 0 : action;
		zmc_redraw();
		return true;
	}
	open_picker = 0;
	switch (action) {
	case SB_ACT_SHOW:
		if (arg >= 0 && arg < SB_N_MOVABLE) {
			app.sb.shown[arg] = !app.sb.shown[arg];
			sb_changed();
		}
		break;
	case SB_ACT_COLOUR_OPEN:
		if (arg >= 0 && arg <= SB_PICK_DOT) sb_board_open(arg);
		return true;
	case SB_ACT_COLOUR:
		if (arg < 0 || arg >= SB_N_PALETTE) break;
		if (zmc_board_up()) {
			sb_choice = arg;
			zmc_board_choose(sb_board_apply);
			return true;
		}
		*sb_pick_slot() = SB_PALETTE[arg].colour;
		sb_changed();
		break;
	case SB_ACT_STREAM:
		if (arg >= 0 && arg < SB_N_STREAM) {
			app.sb.bt_stream = arg;
			sb_changed();
		}
		break;
	case SB_ACT_RESET: {
		struct zt_statusbar d;
		sb_defaults(&d);
		memcpy(app.sb.colour, d.colour, sizeof(d.colour));
		app.sb.bt_stream = d.bt_stream;
		app.sb.bt_dot = d.bt_dot;
		sb_changed();
		break;
	}
	case SB_ACT_ORDER_RESET: {
		struct zt_statusbar d;
		sb_defaults(&d);
		memcpy(app.sb.order, d.order, sizeof(d.order));
		memcpy(app.sb.shown, d.shown, sizeof(d.shown));
		sb_changed();
		break;
	}
	}
	zmc_redraw();
	return true;
}

#endif
