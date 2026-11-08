/*
 * clock-themes.h: the always-on display's and the lock screen's clock themes.
 * Included by ztmetro.c just before aod_draw_place: it uses the shell's text
 * helpers (draw_text, text_width, the FONT_ names), zt_clock_parts and the
 * AOD's colours.
 *
 * A theme draws the clock block (the time, its AM/PM in 12-hour time, the
 * date) from a top edge, at a burn-in offset, and says where it ended and how
 * the shared rows under it (the battery, the waiting apps) are aligned. The
 * AOD draws those rows and the force press's mark itself, so every theme has
 * them. A theme also has a light budget: the most of the panel's light its
 * whole frame may use (the mean of the pixels' linear light, 0..1 over the
 * screen), which the AOD enforces by dimming the frame (aod_budget).
 *
 * To add one: a draw function and a line in CLOCK_THEMES. Burn-in rules for
 * the AOD: thin strokes, no large bright areas, grey for all but the time.
 */

enum { THEME_AOD = 1, THEME_LOCK = 2 };
enum theme_align { ALIGN_CENTRE, ALIGN_LEFT };

struct clock_ctx {
	cairo_t *cr;
	struct zt_server *server;
	struct tm tm;		/* the time to show */
	struct tm prev;		/* the lock flip clock: the minute before */
	double flip;		/* the lock flip clock: 0..1 through the change; 1 none */
	double w, h;		/* the screen, points */
	double dx;		/* the burn-in offset across (down is in y) */
	bool aod;		/* the always-on display: static, dim */
	bool h24;
	int place;		/* the burn-in place (the image's shift within its frame) */
	double left;		/* ALIGN_LEFT: the left edge the theme used */
};

struct clock_theme {
	const char *id, *name;
	unsigned where;			/* THEME_AOD | THEME_LOCK */
	enum theme_align align;		/* the shared rows under it */
	double light_budget;		/* the AOD frame's mean linear light at most */
	double (*draw)(struct clock_ctx *c, double y);	/* from y; returns the bottom */
};

#define CT_MARGIN	36	/* points: a left-aligned theme's edge */

static void ct_white(struct clock_ctx *c) {
	double v = c->aod ? AOD_WHITE : 1.0;
	cairo_set_source_rgb(c->cr, v, v, v);
}

static void ct_grey(struct clock_ctx *c) {
	double v = c->aod ? AOD_GREY : 0.85;
	cairo_set_source_rgb(c->cr, v, v, v);
}

/* "4:21" and "PM", or "16:21" and "" */
static void ct_time(struct clock_ctx *c, const struct tm *tm, char *hm, size_t n, char *ampm) {
	zt_clock_parts(tm, c->h24, hm, n, ampm, 4);
}

/* the time and its AM/PM after it, the AM/PM's bottom on the digits' baseline-ish;
 * centred on the screen (left < 0) or from `left`; returns the time's width */
static double ct_time_line(struct clock_ctx *c, double left, double y, const char *font, double size) {
	char hm[16], ampm[4];
	ct_time(c, &c->tm, hm, sizeof(hm), ampm);
	double tw = text_width(hm, font, size);
	double as = size * 0.26, aw = ampm[0] ? size * 0.1 + text_width(ampm, FONT_SEMILIGHT, as) : 0;
	double x = left >= 0 ? left : round((c->w - tw - aw) / 2 + c->dx);
	ct_white(c);
	/* (in a box wider than it: one only as wide is ellipsized) */
	draw_text(c->cr, hm, font, size, x - 20, y, tw + 40, size * 1.2, true);
	if (ampm[0]) {
		ct_grey(c);
		draw_text(c->cr, ampm, FONT_SEMILIGHT, as, x + tw + size * 0.1, y + size * 0.69, 0, 0, false);
	}
	return tw + aw;
}

static void ct_date(struct clock_ctx *c, const char *fmt, char *out, size_t n) {
	strftime(out, n, fmt, &c->tm);
}

/* ---- the themes ---------------------------------------------------------- */

/* Metro: the time large and thin, centred, the date under it */
static double theme_metro(struct clock_ctx *c, double y) {
	char line[64];
	ct_time_line(c, -1, y, FONT_LIGHT, 84);
	y += 104;
	ct_grey(c);
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(c->cr, line, FONT_SEMILIGHT, 20, c->dx + 24, y, c->w - 48, 28, true);
	return y + 44;
}

/* Compact: small and to the left, as Samsung's own */
static double theme_compact(struct clock_ctx *c, double y) {
	char line[64];
	c->left = CT_MARGIN + c->dx;
	ct_time_line(c, c->left, y, FONT_LIGHT, 58);
	y += 70;
	ct_grey(c);
	ct_date(c, "%a, %B %-d", line, sizeof(line));
	draw_text(c->cr, line, FONT_SEMILIGHT, 24, c->left, y, 0, 0, false);
	return y + 44;
}

/* Digital: the time as large as it fits, thin */
static double theme_digital(struct clock_ctx *c, double y) {
	char line[64];
	ct_time_line(c, -1, y, FONT_LIGHT, 124);
	y += 148;
	ct_grey(c);
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(c->cr, line, FONT_SEMILIGHT, 20, c->dx + 24, y, c->w - 48, 28, true);
	return y + 44;
}

/* Stacked: the hours over the minutes, to the left */
static double theme_stacked(struct clock_ctx *c, double y) {
	char hm[16], ampm[4], line[64];
	ct_time(c, &c->tm, hm, sizeof(hm), ampm);
	char *colon = strchr(hm, ':');
	const char *minutes = colon ? colon + 1 : "";
	if (colon) *colon = 0;
	c->left = CT_MARGIN + c->dx;
	ct_white(c);
	draw_text(c->cr, hm, FONT_LIGHT, 112, c->left - 4, y, 0, 0, false);
	if (ampm[0]) {
		ct_grey(c);
		draw_text(c->cr, ampm, FONT_SEMILIGHT, 22, c->left + text_width(hm, FONT_LIGHT, 112) + 4, y + 26, 0, 0, false);
	}
	ct_white(c);
	draw_text(c->cr, minutes, FONT_LIGHT, 112, c->left - 4, y + 112, 0, 0, false);
	y += 250;
	ct_grey(c);
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(c->cr, line, FONT_SEMILIGHT, 20, c->left, y, 0, 0, false);
	return y + 44;
}

/* Analogue: a thin dial, two hands, no seconds; the time in words under it */
static double theme_analogue(struct clock_ctx *c, double y) {
	cairo_t *cr = c->cr;
	double r = 74, cx = c->w / 2 + c->dx, cy = y + r + 4;
	char hm[16], ampm[4], line[64];
	cairo_save(cr);
	ct_grey(c);
	cairo_set_line_width(cr, 1.0);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_stroke(cr);
	for (int i = 0; i < 12; i++) {
		double a = i * M_PI / 6, l = i % 3 ? 5 : 10;
		cairo_move_to(cr, cx + sin(a) * (r - 3), cy - cos(a) * (r - 3));
		cairo_line_to(cr, cx + sin(a) * (r - 3 - l), cy - cos(a) * (r - 3 - l));
	}
	cairo_stroke(cr);
	double mins = c->tm.tm_min, hours = c->tm.tm_hour % 12 + mins / 60.0;
	double ha = hours * M_PI / 6, ma = mins * M_PI / 30;
	ct_white(c);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, 2.2);
	cairo_move_to(cr, cx, cy);
	cairo_line_to(cr, cx + sin(ha) * r * 0.5, cy - cos(ha) * r * 0.5);
	cairo_stroke(cr);
	cairo_set_line_width(cr, 1.4);
	cairo_move_to(cr, cx, cy);
	cairo_line_to(cr, cx + sin(ma) * r * 0.82, cy - cos(ma) * r * 0.82);
	cairo_stroke(cr);
	cairo_arc(cr, cx, cy, 2.2, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
	y = cy + r + 16;
	ct_time(c, &c->tm, hm, sizeof(hm), ampm);
	snprintf(line, sizeof(line), ampm[0] ? "%s %s" : "%s", hm, ampm);
	ct_grey(c);
	draw_text(cr, line, FONT_SEMILIGHT, 18, c->dx + 24, y, c->w - 48, 26, true);
	y += 28;
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(cr, line, FONT_SEMILIGHT, 18, c->dx + 24, y, c->w - 48, 26, true);
	return y + 40;
}

/* Calendar: the time, and the month with today ringed */
static double theme_calendar(struct clock_ctx *c, double y) {
	cairo_t *cr = c->cr;
	char line[64];
	ct_time_line(c, -1, y, FONT_LIGHT, 60);
	y += 74;
	ct_grey(c);
	ct_date(c, "%B %Y", line, sizeof(line));
	draw_text(cr, line, FONT_SEMILIGHT, 16, c->dx + 24, y, c->w - 48, 22, true);
	y += 28;
	const double cw = 30, rh = 22;
	double x0 = round((c->w - 7 * cw) / 2 + c->dx);
	static const char *const days[] = { "S", "M", "T", "W", "T", "F", "S" };
	for (int i = 0; i < 7; i++) {
		draw_text(cr, days[i], FONT_SEMILIGHT, 11, x0 + i * cw, y, cw, 16, true);
	}
	y += 20;
	struct tm first = c->tm;
	first.tm_mday = 1;
	mktime(&first);	/* (its weekday) */
	static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int year = c->tm.tm_year + 1900, n = mdays[c->tm.tm_mon];
	if (c->tm.tm_mon == 1 && ((year % 4 == 0 && year % 100) || year % 400 == 0)) n = 29;
	int col = first.tm_wday, row = 0;
	for (int d = 1; d <= n; d++) {
		snprintf(line, sizeof(line), "%d", d);
		double x = x0 + col * cw, ty = y + row * rh;
		if (d == c->tm.tm_mday) {
			ct_white(c);
			cairo_set_line_width(cr, 1.0);
			cairo_arc(cr, x + cw / 2, ty + 9, 10, 0, 2 * M_PI);
			cairo_stroke(cr);
		} else {
			ct_grey(c);
		}
		draw_text(cr, line, FONT_SEMILIGHT, 13, x, ty, cw, 18, true);
		if (++col == 7) {
			col = 0;
			row++;
		}
	}
	return y + (row + (col ? 1 : 0)) * rh + 16;
}

/* Image: the user's picture, muted, in a frame, and the time under it */
static cairo_surface_t *aod_image_surface(struct zt_server *server, int pw, int ph);
#define AOD_IMAGE_W	200	/* points */
#define AOD_IMAGE_H	200
#define AOD_IMAGE_SHIFT	5	/* points: the picture moves this far within its frame */
static double theme_image(struct clock_ctx *c, double y) {
	cairo_t *cr = c->cr;
	double k = c->server->scale > 0 ? c->server->scale : 1;
	double x0 = round((c->w - AOD_IMAGE_W) / 2 + c->dx);
	/* the picture, a little larger than its frame: it moves within it */
	int pw = (int)ceil((AOD_IMAGE_W + 2 * AOD_IMAGE_SHIFT) * k), ph = (int)ceil((AOD_IMAGE_H + 2 * AOD_IMAGE_SHIFT) * k);
	cairo_surface_t *img = aod_image_surface(c->server, pw, ph);
	if (img) {
		static const signed char SHIFT[][2] = { { 0, 0 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 },
			{ 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
		const signed char *s = SHIFT[c->place % 9];
		cairo_save(cr);
		cairo_new_path(cr);
		double rr = 14;
		cairo_arc(cr, x0 + AOD_IMAGE_W - rr, y + rr, rr, -M_PI / 2, 0);
		cairo_arc(cr, x0 + AOD_IMAGE_W - rr, y + AOD_IMAGE_H - rr, rr, 0, M_PI / 2);
		cairo_arc(cr, x0 + rr, y + AOD_IMAGE_H - rr, rr, M_PI / 2, M_PI);
		cairo_arc(cr, x0 + rr, y + rr, rr, M_PI, 3 * M_PI / 2);
		cairo_close_path(cr);
		cairo_clip(cr);
		cairo_translate(cr, x0 - AOD_IMAGE_SHIFT * (1 + s[0]), y - AOD_IMAGE_SHIFT * (1 + s[1]));
		cairo_scale(cr, 1 / k, 1 / k);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		y += AOD_IMAGE_H + 18;
	}
	char line[64];
	ct_time_line(c, -1, y, FONT_LIGHT, 56);
	y += 70;
	ct_grey(c);
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(cr, line, FONT_SEMILIGHT, 18, c->dx + 24, y, c->w - 48, 26, true);
	return y + 40;
}

/* ---- the flip clock: split-flap cards, after HTC's (an original drawing) -- */

#define FLIP_MS		560	/* the lock screen's flip on the minute */
#define FLIP_FRAME_MS	33

/* A rounded rectangle's path. */
static void ct_round_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* One half of a card (top or bottom) with `text` on it, folded about the
 * hinge by `squash` (1 flat, 0 edge on), and darkened as it folds. */
static void flip_half(struct clock_ctx *c, double x, double y, double w, double h, const char *text,
		const char *font, double size, bool top, double squash) {
	cairo_t *cr = c->cr;
	double hinge = y + h / 2;
	if (squash <= 0.01) return;
	cairo_save(cr);
	cairo_rectangle(cr, x - 1, top ? y - 1 : hinge, w + 2, h / 2 + 1);
	cairo_clip(cr);
	cairo_translate(cr, 0, hinge);
	cairo_scale(cr, 1, squash);
	cairo_translate(cr, 0, -hinge);
	ct_round_rect(cr, x, y, w, h, 10);
	if (c->aod) {
		/* the AOD: an outline on near black, no light in the card itself */
		cairo_set_source_rgb(cr, 0.03, 0.03, 0.03);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0.32, 0.32, 0.32);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
		cairo_pattern_add_color_stop_rgb(g, 0, 0.24, 0.24, 0.25);
		cairo_pattern_add_color_stop_rgb(g, 0.5, 0.15, 0.15, 0.16);
		cairo_pattern_add_color_stop_rgb(g, 0.5, 0.12, 0.12, 0.13);
		cairo_pattern_add_color_stop_rgb(g, 1, 0.07, 0.07, 0.08);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
	}
	if (c->aod) {
		ct_white(c);
	} else {
		cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
	}
	draw_text(cr, text, font, size, x, y, w, h, true);
	if (squash < 1) {
		cairo_rectangle(cr, x, y, w, h);
		cairo_set_source_rgba(cr, 0, 0, 0, (1 - squash) * 0.55);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* A card that turns from `from` to `to` at `t` (0..1; 1 still). */
static void flip_card(struct clock_ctx *c, double x, double y, double w, double h, const char *from,
		const char *to, const char *font, double size, double t) {
	cairo_t *cr = c->cr;
	if (!c->aod) {
		/* its shadow */
		ct_round_rect(cr, x + 1, y + 4, w, h, 10);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		cairo_fill(cr);
	}
	bool turning = t < 1 && strcmp(from, to) != 0;
	flip_half(c, x, y, w, h, to, font, size, true, 1);			/* the new top, behind */
	flip_half(c, x, y, w, h, turning ? from : to, font, size, false, 1);	/* the old bottom */
	if (turning) {
		if (t < 0.5) {
			flip_half(c, x, y, w, h, from, font, size, true, cos(t * M_PI));	/* the old top falls */
		} else {
			flip_half(c, x, y, w, h, to, font, size, false, -cos(t * M_PI));	/* the new bottom lands */
		}
	}
	/* the hinge: a gap across, and the pins at its ends */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, x, y + h / 2 - (c->aod ? 1 : 1.5), w, c->aod ? 2 : 3);
	cairo_fill(cr);
	if (!c->aod) {
		cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
		cairo_rectangle(cr, x - 2, y + h / 2 - 7, 4, 14);
		cairo_rectangle(cr, x + w - 2, y + h / 2 - 7, 4, 14);
		cairo_fill(cr);
	}
}

/* The two cards and the date under them. */
static double theme_flip(struct clock_ctx *c, double y) {
	char hm[16], ampm[4], phm[16], pampm[4], line[64];
	ct_time(c, &c->tm, hm, sizeof(hm), ampm);
	ct_time(c, &c->prev, phm, sizeof(phm), pampm);
	char *m = strchr(hm, ':'), *pm = strchr(phm, ':');
	if (!m || !pm) return y;
	*m++ = 0;
	*pm++ = 0;
	double cw = c->aod ? 118 : 158, chh = c->aod ? 104 : 140, gap = c->aod ? 10 : 12;
	double size = c->aod ? 76 : 104;
	const char *font = c->aod ? FONT_LIGHT : FONT_SEMIBOLD;
	double x0 = round((c->w - 2 * cw - gap) / 2 + c->dx);
	double t = c->aod ? 1 : c->flip;
	flip_card(c, x0, y, cw, chh, phm, hm, font, size, t);
	flip_card(c, x0 + cw + gap, y, cw, chh, pm, m, font, size, t);
	if (ampm[0]) {
		if (c->aod) ct_grey(c); else cairo_set_source_rgba(c->cr, 1, 1, 1, 0.8);
		draw_text(c->cr, ampm, c->aod ? FONT_SEMILIGHT : FONT_SEMIBOLD, c->aod ? 12 : 15,
			x0 + 9, y + chh - (c->aod ? 20 : 24), 0, 0, false);
	}
	y += chh + 14;
	if (c->aod) ct_grey(c); else cairo_set_source_rgb(c->cr, 1, 1, 1);
	ct_date(c, "%A, %B %-d", line, sizeof(line));
	draw_text(c->cr, line, FONT_SEMILIGHT, c->aod ? 20 : 26, c->dx + 16, y, c->w - 32, 34, true);
	return y + (c->aod ? 44 : 40);
}

/* ---- the list ------------------------------------------------------------ */

/* light budgets: about a third over each theme's own light with its longest
 * time and five waiting apps (ztmetro --render prints them, aod-themes.txt) */
static const struct clock_theme CLOCK_THEMES[] = {
	{ "metro", "Metro", THEME_AOD | THEME_LOCK, ALIGN_CENTRE, 0.0060, theme_metro },
	{ "compact", "Compact", THEME_AOD, ALIGN_LEFT, 0.0045, theme_compact },
	{ "digital", "Digital", THEME_AOD, ALIGN_CENTRE, 0.0090, theme_digital },
	{ "analogue", "Analogue", THEME_AOD, ALIGN_CENTRE, 0.0050, theme_analogue },
	{ "stacked", "Stacked", THEME_AOD, ALIGN_LEFT, 0.0110, theme_stacked },
	{ "calendar", "Calendar", THEME_AOD, ALIGN_CENTRE, 0.0070, theme_calendar },
	{ "flip", "Flip clock", THEME_AOD | THEME_LOCK, ALIGN_CENTRE, 0.0080, theme_flip },
	{ "image", "Image", THEME_AOD, ALIGN_CENTRE, 0.0120, theme_image },
};
#define N_CLOCK_THEMES	(int)(sizeof(CLOCK_THEMES) / sizeof(CLOCK_THEMES[0]))

/* The theme called `id` that can go `where`, or the first that can. */
static const struct clock_theme *clock_theme(const char *id, unsigned where) {
	for (int i = 0; i < N_CLOCK_THEMES; i++) {
		if ((CLOCK_THEMES[i].where & where) && id && strcmp(CLOCK_THEMES[i].id, id) == 0) {
			return &CLOCK_THEMES[i];
		}
	}
	for (int i = 0; i < N_CLOCK_THEMES; i++) {
		if (CLOCK_THEMES[i].where & where) return &CLOCK_THEMES[i];
	}
	return &CLOCK_THEMES[0];
}
