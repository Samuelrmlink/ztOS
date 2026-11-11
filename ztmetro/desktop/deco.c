// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Window frames as xfwm4 draws them (desktop mode, DESIGN.md "Decorations").
 *
 * xfwm4 is X11 only and never runs here, but its themes and its settings do:
 * the theme chosen in XFCE's "Window Manager" settings (xfconf's xfwm4
 * channel: theme, button_layout, title_alignment, title_font) is read and
 * drawn the way xfwm4 draws it:
 *
 *   - a theme is a directory THEMES/NAME/xfwm4 of pieces: the corners, title-1
 *     to title-5, the sides and the bottom, and a button for each of menu,
 *     stick, shade, hide, maximize and close, in the states active, inactive,
 *     prelight and pressed (and -toggled- for maximize, shade and stick);
 *   - each piece is an XPM whose colours may be symbolic ("s active_color_2"),
 *     filled from the GTK theme's colours, or a PNG when there is no XPM;
 *   - themerc sets the offsets, spacing and text colours.
 *
 * Without a theme on disk (no xfwm4 package) the same pieces are drawn here
 * in the look of xfwm4's Default theme, which is what a fresh XFCE has.
 *
 * The GTK colours are not asked of GTK (the shell does not link it): they
 * are the ones xfwm4 gets from Adwaita and Adwaita-dark, measured from xfwm4
 * itself, and the dark set is used when the GTK theme's name says "dark".
 *
 * Settings are read from xfconf's files, and from $XDG_RUNTIME_DIR/
 * ztmetro-xfwm4, which ztos-desktop-session keeps up to date from xfconf
 * as they change ("xfconf-query -m"; xfconfd writes its files late), and then
 * says "desktop theme" on the control pipe.
 *
 * Included by desktop.c.
 */

enum deco_piece {
	P_TOP_LEFT, P_TOP_RIGHT, P_TITLE_1, P_TITLE_2, P_TITLE_3, P_TITLE_4, P_TITLE_5,
	P_LEFT, P_RIGHT, P_BOTTOM_LEFT, P_BOTTOM, P_BOTTOM_RIGHT, P_COUNT
};
static const char *const deco_piece_name[P_COUNT] = {
	"top-left", "top-right", "title-1", "title-2", "title-3", "title-4", "title-5",
	"left", "right", "bottom-left", "bottom", "bottom-right",
};

/* xfwm4's button letters, in button_layout: O|SHMC */
enum deco_kind { BK_MENU, BK_STICK, BK_SHADE, BK_HIDE, BK_MAXIMIZE, BK_CLOSE, BK_COUNT };
static const char deco_kind_letter[] = "OTSHMC";
static const char *const deco_kind_name[BK_COUNT] = { "menu", "stick", "shade", "hide", "maximize", "close" };
enum deco_bstate { BS_ACTIVE, BS_INACTIVE, BS_PRELIGHT, BS_PRESSED, BS_COUNT };
static const char *const deco_bstate_name[BS_COUNT] = { "active", "inactive", "prelight", "pressed" };

#define DECO_MAX_BUTTONS 8

/* the GTK colours xfwm4 gives its themes' symbols */
struct deco_symbol {
	const char *name;
	uint32_t light, dark;	/* Adwaita, Adwaita-dark */
};
static const struct deco_symbol deco_symbols[] = {
	{ "active_text_color", 0x2e3436, 0xeeeeec },
	{ "inactive_text_color", 0x929595, 0x919190 },
	{ "active_text_shadow_color", 0xd8d5d2, 0x353535 },
	{ "inactive_text_shadow_color", 0xf6f5f4, 0x353535 },
	{ "active_border_color", 0x2e3436, 0x1b1b1b },
	{ "inactive_border_color", 0x929595, 0x1b1b1b },
	{ "active_color_1", 0x3584e4, 0x15539e },
	{ "active_color_2", 0xf6f5f4, 0x353535 },
	{ "active_hilight_1", 0x5d9ce9, 0x2a76c6 },
	{ "active_hilight_2", 0xffffff, 0x404040 },
	{ "active_mid_1", 0x2a6fc9, 0x1c4f8c },
	{ "active_mid_2", 0xd8d5d2, 0x353535 },
	{ "active_shadow_1", 0x1c5aa6, 0x0f3b71 },
	{ "active_shadow_2", 0xb1aba5, 0x343434 },
	{ "inactive_color_1", 0xf6f5f4, 0x353535 },
	{ "inactive_color_2", 0xf6f5f4, 0x353535 },
	{ "inactive_hilight_1", 0xffffff, 0x404040 },
	{ "inactive_hilight_2", 0xffffff, 0x404040 },
	{ "inactive_mid_1", 0xe8e6e3, 0x2d2d2d },
	{ "inactive_mid_2", 0xf6f5f4, 0x353535 },
	{ "inactive_shadow_1", 0xcdc7c2, 0x2a2a2a },
	{ "inactive_shadow_2", 0xdedbd8, 0x343434 },
};

struct deco_button {
	int kind;	/* deco_kind */
	int x, w, h;	/* in the title bar, points */
};

struct deco_theme {
	char name[96];		/* xfwm4's theme name */
	bool files;		/* drawn from that theme's files; false: our Default */
	bool dark;		/* the GTK theme is a dark one */
	char layout[32];	/* button_layout, the letters we draw */
	int align;		/* the title: 0 left, 1 centre, 2 right */
	char font[96];		/* title_font */
	char menu_font[96];	/* GTK's font (xsettings Gtk/FontName), for the window menu */
	cairo_surface_t *piece[P_COUNT][2];		/* [active, inactive] */
	cairo_surface_t *button[BK_COUNT][2][BS_COUNT];	/* [kind][toggled][state] */
	double text[2][3], text_shadow[2][3];	/* rgb, [active, inactive] */
	bool title_shadow[2];
	int button_offset, button_spacing, title_h_off, title_v_off[2];
	bool full_width_title;
	int top, left, right, bottom;	/* the frame around the content, points */
	time_t stamp;			/* the settings' files, when last read */
};

static struct deco_theme deco;

static uint32_t deco_symbol_rgb(const char *name) {
	for (size_t i = 0; i < sizeof(deco_symbols) / sizeof(deco_symbols[0]); i++) {
		if (!strcmp(deco_symbols[i].name, name)) {
			return deco.dark ? deco_symbols[i].dark : deco_symbols[i].light;
		}
	}
	return 0x808080;
}

static void deco_rgb(uint32_t c, double out[3]) {
	out[0] = ((c >> 16) & 255) / 255.0;
	out[1] = ((c >> 8) & 255) / 255.0;
	out[2] = (c & 255) / 255.0;
}

/* "#rrggbb", "#rgb", "#rrrrggggbbbb", a few names, or a symbol's name */
static bool deco_parse_color(const char *s, uint32_t *rgb) {
	while (*s == ' ') s++;
	if (*s == '#') {
		size_t n = strspn(s + 1, "0123456789abcdefABCDEF");
		unsigned long v = strtoul(s + 1, NULL, 16);
		if (n == 6) { *rgb = (uint32_t)v; return true; }
		if (n == 3) {
			*rgb = (uint32_t)(((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11);
			return true;
		}
		if (n == 12) {
			char part[5] = { 0 };
			uint32_t r, g, b;
			memcpy(part, s + 1, 4); r = (uint32_t)strtoul(part, NULL, 16) >> 8;
			memcpy(part, s + 5, 4); g = (uint32_t)strtoul(part, NULL, 16) >> 8;
			memcpy(part, s + 9, 4); b = (uint32_t)strtoul(part, NULL, 16) >> 8;
			*rgb = r << 16 | g << 8 | b;
			return true;
		}
		return false;
	}
	if (!strcasecmp(s, "black")) { *rgb = 0; return true; }
	if (!strcasecmp(s, "white")) { *rgb = 0xffffff; return true; }
	if (!strcasecmp(s, "gray") || !strcasecmp(s, "grey")) { *rgb = 0xbebebe; return true; }
	for (size_t i = 0; i < sizeof(deco_symbols) / sizeof(deco_symbols[0]); i++) {
		if (!strcmp(deco_symbols[i].name, s)) {
			*rgb = deco_symbol_rgb(s);
			return true;
		}
	}
	return false;
}

/* ---- XPM, with xfwm4's symbolic colours ----------------------------------------- */

static char *deco_slurp(const char *path, size_t max) {
	FILE *f = fopen(path, "r");
	if (!f) return NULL;
	char *buf = malloc(max + 1);
	size_t n = fread(buf, 1, max, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

/* the quoted strings of an XPM file, in order (comments and C around them skipped) */
static int deco_xpm_strings(char *text, char **out, int max) {
	int n = 0;
	char *p = text;
	while (*p && n < max) {
		if (p[0] == '/' && p[1] == '*') {
			char *e = strstr(p + 2, "*/");
			if (!e) break;
			p = e + 2;
			continue;
		}
		if (*p == '"') {
			char *e = strchr(p + 1, '"');
			if (!e) break;
			*e = 0;
			out[n++] = p + 1;
			p = e + 1;
			continue;
		}
		p++;
	}
	return n;
}

static cairo_surface_t *deco_xpm_load(const char *path) {
	char *text = deco_slurp(path, 1 << 20);
	if (!text) return NULL;
	static char *str[4096];
	int n = deco_xpm_strings(text, str, 4096);
	int w, h, ncolors, cpp;
	if (n < 1 || sscanf(str[0], "%d %d %d %d", &w, &h, &ncolors, &cpp) != 4 || w <= 0 || h <= 0 ||
			w > 2048 || h > 2048 || cpp < 1 || cpp > 2 || ncolors < 1 || n < 1 + ncolors + h) {
		free(text);
		return NULL;
	}
	/* cpp is 1 or 2: a table of every code */
	uint32_t *table = calloc(65536, sizeof(uint32_t));	/* ARGB, premultiplied; 0 transparent */
	for (int i = 0; i < ncolors; i++) {
		const char *line = str[1 + i];
		if ((int)strlen(line) < cpp) continue;
		unsigned code = cpp == 1 ? (unsigned char)line[0] : (unsigned char)line[0] << 8 | (unsigned char)line[1];
		/* key-value pairs: c COLOR, s SYMBOL, m, g, g4 */
		char copy[256];
		snprintf(copy, sizeof(copy), "%s", line + cpp);
		char *save = NULL, *key = strtok_r(copy, " \t", &save);
		uint32_t rgb = 0;
		bool have = false, none = false, sym = false;
		while (key) {
			char *val = strtok_r(NULL, " \t", &save);
			if (!val) break;
			if (!strcmp(key, "s")) {
				for (size_t k = 0; k < sizeof(deco_symbols) / sizeof(deco_symbols[0]); k++) {
					if (!strcmp(deco_symbols[k].name, val)) {
						rgb = deco_symbol_rgb(val);
						have = sym = true;
						none = false;
					}
				}
			} else if (!strcmp(key, "c") && !sym) {
				if (!strcasecmp(val, "None")) {
					none = true;
				} else {
					have = deco_parse_color(val, &rgb);
				}
			}
			key = strtok_r(NULL, " \t", &save);
		}
		table[code] = none || !have ? 0 : 0xff000000u | rgb;
	}
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		const char *row = str[1 + ncolors + y];
		uint32_t *px = (uint32_t *)(data + y * stride);
		int len = (int)strlen(row);
		for (int x = 0; x < w; x++) {
			if ((x + 1) * cpp > len) { px[x] = 0; continue; }
			unsigned code = cpp == 1 ? (unsigned char)row[x] :
				(unsigned char)row[2 * x] << 8 | (unsigned char)row[2 * x + 1];
			px[x] = table[code];
		}
	}
	cairo_surface_mark_dirty(s);
	free(table);
	free(text);
	return s;
}

/* A piece of a theme: NAME.xpm, in the GTK theme's colours, or NAME.png when
 * there is no XPM. (Default ships both; xfwm4 4.20 draws its XPMs, with or
 * without its compositor: measured under Xvfb.) */
static cairo_surface_t *deco_piece_load(const char *dir, const char *name) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s.xpm", dir, name);
	cairo_surface_t *xpm = deco_xpm_load(path);
	if (xpm) return xpm;
	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	cairo_surface_t *png = access(path, R_OK) == 0 ? cairo_image_surface_create_from_png(path) : NULL;
	if (png && cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(png);
		png = NULL;
	}
	return png;
}

/* ---- our own Default, when no theme is on disk --------------------------------- */

static cairo_surface_t *deco_solid(int w, int h, uint32_t rgb) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(s);
	double c[3];
	deco_rgb(rgb, c);
	cairo_set_source_rgb(cr, c[0], c[1], c[2]);
	cairo_paint(cr);
	cairo_destroy(cr);
	return s;
}

/* a top corner: the title's colour, rounded outside (radius 4), the side below */
static cairo_surface_t *deco_corner(bool left, uint32_t title, uint32_t side) {
	const int w = 8, h = 29;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(s);
	double c[3];
	deco_rgb(title, c);
	cairo_set_source_rgb(cr, c[0], c[1], c[2]);
	double r = 4;
	if (left) {
		cairo_move_to(cr, 0, h);
		cairo_line_to(cr, 0, r);
		cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
		cairo_line_to(cr, w, 0);
		cairo_line_to(cr, w, h);
	} else {
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, w - r, 0);
		cairo_arc(cr, w - r, r, r, 1.5 * M_PI, 2 * M_PI);
		cairo_line_to(cr, w, h);
		cairo_line_to(cr, 0, h);
	}
	cairo_close_path(cr);
	cairo_fill(cr);
	(void)side;
	cairo_destroy(cr);
	return s;
}

/* a button of Default's: 21 x 29 (the menu 22), the glyph in the middle, a
 * rounded box behind it when the pointer is on it or it is pressed */
static cairo_surface_t *deco_button_draw(int kind, bool toggled, int state) {
	int w = kind == BK_MENU ? 22 : 21, h = 29;
	bool active = state != BS_INACTIVE;
	uint32_t bg = deco_symbol_rgb(active ? "active_mid_2" : "inactive_color_2");
	uint32_t fg = deco_symbol_rgb(active ? "active_text_color" : "inactive_text_color");
	cairo_surface_t *s = deco_solid(w, h, bg);
	cairo_t *cr = cairo_create(s);
	double c[3];
	if (state == BS_PRELIGHT || state == BS_PRESSED) {
		deco_rgb(fg, c);
		cairo_set_source_rgba(cr, c[0], c[1], c[2], state == BS_PRESSED ? 0.28 : 0.14);
		double x = 1.5, y = 5.5, bw = w - 3, bh = 18, r = 3;
		cairo_new_sub_path(cr);
		cairo_arc(cr, x + bw - r, y + r, r, -M_PI / 2, 0);
		cairo_arc(cr, x + bw - r, y + bh - r, r, 0, M_PI / 2);
		cairo_arc(cr, x + r, y + bh - r, r, M_PI / 2, M_PI);
		cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	deco_rgb(fg, c);
	cairo_set_source_rgb(cr, c[0], c[1], c[2]);
	double cx = w / 2.0, cy = h / 2.0;
	switch (kind) {
	case BK_CLOSE:	/* an X of 3-pixel strokes, 9 across */
		cairo_set_line_width(cr, 2.6);
		cairo_move_to(cr, cx - 4, cy - 4);
		cairo_line_to(cr, cx + 4, cy + 4);
		cairo_move_to(cr, cx + 4, cy - 4);
		cairo_line_to(cr, cx - 4, cy + 4);
		cairo_stroke(cr);
		break;
	case BK_MAXIMIZE:
		cairo_set_line_width(cr, 2);
		if (toggled) {	/* two boxes: restore */
			cairo_rectangle(cr, cx - 4, cy - 1, 7, 6);
			cairo_stroke(cr);
			cairo_move_to(cr, cx - 1, cy - 3);
			cairo_line_to(cr, cx + 5, cy - 3);
			cairo_line_to(cr, cx + 5, cy + 2);
			cairo_stroke(cr);
		} else {
			cairo_rectangle(cr, cx - 4, cy - 4, 8, 8);
			cairo_stroke(cr);
		}
		break;
	case BK_HIDE:
		cairo_rectangle(cr, cx - 4.5, cy + 3, 9, 2);
		cairo_fill(cr);
		break;
	case BK_SHADE:	/* a chevron, up; down when rolled up */
		cairo_set_line_width(cr, 2.2);
		cairo_move_to(cr, cx - 4.5, cy + (toggled ? -2 : 2.5));
		cairo_line_to(cr, cx, cy + (toggled ? 2.5 : -2));
		cairo_line_to(cr, cx + 4.5, cy + (toggled ? -2 : 2.5));
		cairo_stroke(cr);
		break;
	case BK_STICK:
		cairo_arc(cr, cx, cy, toggled ? 3.5 : 2.5, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case BK_MENU:	/* a small window, its title bar filled */
		cairo_set_line_width(cr, 1.5);
		cairo_rectangle(cr, cx - 6, cy - 5, 12, 10);
		cairo_stroke(cr);
		cairo_rectangle(cr, cx - 6, cy - 5, 12, 3);
		cairo_fill(cr);
		break;
	}
	cairo_destroy(cr);
	return s;
}

static void deco_builtin(void) {
	for (int a = 0; a < 2; a++) {
		uint32_t title = deco_symbol_rgb(a == 0 ? "active_mid_2" : "inactive_color_2");
		uint32_t side = deco_symbol_rgb(a == 0 ? "active_color_2" : "inactive_color_2");
		deco.piece[P_TOP_LEFT][a] = deco_corner(true, title, side);
		deco.piece[P_TOP_RIGHT][a] = deco_corner(false, title, side);
		for (int p = P_TITLE_1; p <= P_TITLE_5; p++) deco.piece[p][a] = deco_solid(4, 29, title);
		deco.piece[P_LEFT][a] = deco_solid(5, 24, side);
		deco.piece[P_RIGHT][a] = deco_solid(5, 24, side);
		deco.piece[P_BOTTOM_LEFT][a] = deco_solid(5, 5, side);
		deco.piece[P_BOTTOM][a] = deco_solid(24, 5, side);
		deco.piece[P_BOTTOM_RIGHT][a] = deco_solid(5, 5, side);
	}
	for (int k = 0; k < BK_COUNT; k++) {
		for (int t = 0; t < 2; t++) {
			for (int st = 0; st < BS_COUNT; st++) {
				deco.button[k][t][st] = deco_button_draw(k, t, st);
			}
		}
	}
	deco.button_offset = 0;
	deco.button_spacing = 2;
	deco.full_width_title = true;
	deco.title_h_off = 1;
	deco.title_v_off[0] = deco.title_v_off[1] = 1;
}

/* ---- settings ------------------------------------------------------------------------ */

/* <property name="NAME" type="string" value="VALUE"/> in an xfconf channel's file */
static bool deco_xfconf(const char *channel, const char *name, char *out, size_t size) {
	char path[512];
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (cfg && cfg[0]) snprintf(path, sizeof(path), "%s/xfce4/xfconf/xfce-perchannel-xml/%s.xml", cfg, channel);
	else snprintf(path, sizeof(path), "%s/.config/xfce4/xfconf/xfce-perchannel-xml/%s.xml", home ? home : "", channel);
	char *text = deco_slurp(path, 1 << 20);
	if (!text) return false;
	char want[96];
	snprintf(want, sizeof(want), "name=\"%s\"", name);
	char *p = strstr(text, want);
	bool ok = false;
	if (p) {
		char *end = strchr(p, '>');
		char *v = strstr(p, "value=\"");
		if (v && (!end || v < end)) {
			v += 7;
			char *q = strchr(v, '"');
			if (q && (size_t)(q - v) < size) {
				memcpy(out, v, (size_t)(q - v));
				out[q - v] = 0;
				/* the few entities xfconf writes */
				char *r = out, *wr = out;
				while (*r) {
					if (!strncmp(r, "&amp;", 5)) { *wr++ = '&'; r += 5; }
					else if (!strncmp(r, "&lt;", 4)) { *wr++ = '<'; r += 4; }
					else if (!strncmp(r, "&gt;", 4)) { *wr++ = '>'; r += 4; }
					else if (!strncmp(r, "&quot;", 6)) { *wr++ = '"'; r += 6; }
					else *wr++ = *r++;
				}
				*wr = 0;
				ok = out[0] != 0;
			}
		}
	}
	free(text);
	return ok;
}

/* KEY=VALUE lines that ztos-desktop-session writes from xfconf as it changes */
static bool deco_live(const char *key, char *out, size_t size) {
	char path[300], line[256];
	runtime_file(path, sizeof(path), "ztmetro-xfwm4");
	FILE *f = fopen(path, "r");
	if (!f) return false;
	bool ok = false;
	size_t n = strlen(key);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, n) && line[n] == '=') {
			line[strcspn(line, "\n")] = 0;
			if (line[n + 1]) {
				snprintf(out, size, "%s", line + n + 1);
				ok = true;
			}
		}
	}
	fclose(f);
	return ok;
}

static void deco_setting(const char *channel, const char *prop, const char *key, const char *fallback,
		char *out, size_t size) {
	if (deco_live(key, out, size) || deco_xfconf(channel, prop, out, size)) return;
	snprintf(out, size, "%s", fallback);
}

static bool deco_theme_dir(const char *name, char *out, size_t size) {
	const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
	char dirs[4][256];
	snprintf(dirs[0], sizeof(dirs[0]), "%s/.themes", home ? home : "");
	if (data && data[0]) snprintf(dirs[1], sizeof(dirs[1]), "%s/themes", data);
	else snprintf(dirs[1], sizeof(dirs[1]), "%s/.local/share/themes", home ? home : "");
	snprintf(dirs[2], sizeof(dirs[2]), "/usr/local/share/themes");
	snprintf(dirs[3], sizeof(dirs[3]), "/usr/share/themes");
	if (strchr(name, '/') || !strcmp(name, "..")) return false;
	for (int i = 0; i < 4; i++) {
		snprintf(out, size, "%s/%s/xfwm4", dirs[i], name);
		char rc[700];
		snprintf(rc, sizeof(rc), "%s/themerc", out);
		if (access(rc, R_OK) == 0) return true;
	}
	return false;
}

static void deco_themerc(const char *dir) {
	char path[700], line[256];
	snprintf(path, sizeof(path), "%s/themerc", dir);
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		char key[64], value[128];
		if (line[0] == '#' || sscanf(line, " %63[^= ] = %127[^\n]", key, value) != 2) continue;
		uint32_t rgb;
		int a = !strncmp(key, "inactive", 8);
		if (!strcmp(key, "button_offset")) deco.button_offset = atoi(value);
		else if (!strcmp(key, "button_spacing")) deco.button_spacing = atoi(value);
		else if (!strcmp(key, "full_width_title")) deco.full_width_title = !strcmp(value, "true");
		else if (!strcmp(key, "title_horizontal_offset")) deco.title_h_off = atoi(value);
		else if (!strcmp(key, "title_vertical_offset_active")) deco.title_v_off[0] = atoi(value);
		else if (!strcmp(key, "title_vertical_offset_inactive")) deco.title_v_off[1] = atoi(value);
		else if (!strcmp(key, "title_shadow_active")) deco.title_shadow[0] = strcmp(value, "false") != 0;
		else if (!strcmp(key, "title_shadow_inactive")) deco.title_shadow[1] = strcmp(value, "false") != 0;
		else if ((!strcmp(key, "active_text_color") || !strcmp(key, "inactive_text_color")) &&
				deco_parse_color(value, &rgb)) {
			deco_rgb(rgb, deco.text[a]);
		} else if ((!strcmp(key, "active_text_shadow_color") || !strcmp(key, "inactive_text_shadow_color")) &&
				deco_parse_color(value, &rgb)) {
			deco_rgb(rgb, deco.text_shadow[a]);
		}
	}
	fclose(f);
}

static void deco_free(void) {
	for (int p = 0; p < P_COUNT; p++) {
		for (int a = 0; a < 2; a++) {
			if (deco.piece[p][a]) cairo_surface_destroy(deco.piece[p][a]);
			deco.piece[p][a] = NULL;
		}
	}
	for (int k = 0; k < BK_COUNT; k++) {
		for (int t = 0; t < 2; t++) {
			for (int s = 0; s < BS_COUNT; s++) {
				if (deco.button[k][t][s]) cairo_surface_destroy(deco.button[k][t][s]);
				deco.button[k][t][s] = NULL;
			}
		}
	}
}

static int deco_w(cairo_surface_t *s) { return s ? cairo_image_surface_get_width(s) : 0; }
static int deco_h(cairo_surface_t *s) { return s ? cairo_image_surface_get_height(s) : 0; }

/* Read the settings and the theme again. Returns true when what is drawn changes. */
static bool deco_load(void) {
	char theme[96], layout[64], align[32], font[96], gtk[96], menu_font[96];
	deco_setting("xfwm4", "theme", "theme", "Default", theme, sizeof(theme));
	deco_setting("xfwm4", "button_layout", "button_layout", "O|SHMC", layout, sizeof(layout));
	deco_setting("xfwm4", "title_alignment", "title_alignment", "center", align, sizeof(align));
	deco_setting("xfwm4", "title_font", "title_font", "Sans Bold 9", font, sizeof(font));
	deco_setting("xsettings", "ThemeName", "gtk_theme", "Adwaita", gtk, sizeof(gtk));
	deco_setting("xsettings", "FontName", "gtk_font", "Sans 10", menu_font, sizeof(menu_font));
	bool dark = strcasestr(gtk, "dark") != NULL;
	bool same = deco.piece[P_TITLE_3][0] && !strcmp(theme, deco.name) && !strcmp(layout, deco.layout) &&
		!strcmp(font, deco.font) && dark == deco.dark && !strcmp(menu_font, deco.menu_font) &&
		deco.align == (!strcmp(align, "left") ? 0 : !strcmp(align, "right") ? 2 : 1);
	if (same) {
		return false;
	}
	deco_free();
	snprintf(deco.name, sizeof(deco.name), "%s", theme);
	snprintf(deco.font, sizeof(deco.font), "%s", font);
	snprintf(deco.menu_font, sizeof(deco.menu_font), "%s", menu_font);
	deco.align = !strcmp(align, "left") ? 0 : !strcmp(align, "right") ? 2 : 1;
	deco.dark = dark;
	/* the letters we draw: the stick button is left out (one workspace) */
	int n = 0;
	for (const char *c = layout; *c && n < (int)sizeof(deco.layout) - 1; c++) {
		if (strchr("OSHMC|", *c) && (*c != '|' || !strchr(deco.layout, '|'))) {
			deco.layout[n++] = *c;
			deco.layout[n] = 0;
		}
	}
	deco.layout[n] = 0;
	if (!strchr(deco.layout, '|')) {
		memmove(deco.layout + 1, deco.layout, strlen(deco.layout) + 1);
		deco.layout[0] = '|';
	}
	deco_rgb(deco_symbol_rgb("active_text_color"), deco.text[0]);
	deco_rgb(deco_symbol_rgb("inactive_text_color"), deco.text[1]);
	deco_rgb(deco_symbol_rgb("active_text_shadow_color"), deco.text_shadow[0]);
	deco_rgb(deco_symbol_rgb("inactive_text_shadow_color"), deco.text_shadow[1]);
	deco.title_shadow[0] = deco.title_shadow[1] = false;
	char dir[600];
	deco.files = deco_theme_dir(theme, dir, sizeof(dir)) || deco_theme_dir("Default", dir, sizeof(dir));
	if (deco.files) {
		deco.button_offset = deco.button_spacing = deco.title_h_off = 0;
		deco.title_v_off[0] = deco.title_v_off[1] = 0;
		deco.full_width_title = true;
		deco_themerc(dir);
		char name[96];
		for (int p = 0; p < P_COUNT; p++) {
			for (int a = 0; a < 2; a++) {
				snprintf(name, sizeof(name), "%s-%s", deco_piece_name[p], a ? "inactive" : "active");
				deco.piece[p][a] = deco_piece_load(dir, name);
			}
		}
		for (int k = 0; k < BK_COUNT; k++) {
			for (int t = 0; t < 2; t++) {
				for (int s = 0; s < BS_COUNT; s++) {
					snprintf(name, sizeof(name), "%s%s-%s", deco_kind_name[k], t ? "-toggled" : "",
						deco_bstate_name[s]);
					deco.button[k][t][s] = deco_piece_load(dir, name);
				}
				/* a state the theme lacks: the active one; toggled: the plain one */
				for (int s = 0; s < BS_COUNT; s++) {
					if (!deco.button[k][t][s] && deco.button[k][t][BS_ACTIVE]) {
						deco.button[k][t][s] = cairo_surface_reference(deco.button[k][t][BS_ACTIVE]);
					}
				}
			}
			for (int s = 0; s < BS_COUNT; s++) {
				if (!deco.button[k][1][s] && deco.button[k][0][s]) {
					deco.button[k][1][s] = cairo_surface_reference(deco.button[k][0][s]);
				}
			}
		}
		/* a theme without its title pieces is no theme */
		if (!deco.piece[P_TITLE_3][0] || !deco.piece[P_TITLE_3][1]) {
			deco_free();
			deco.files = false;
		}
	}
	if (!deco.files) {
		deco_builtin();
	}
	for (int p = 0; p < P_COUNT; p++) {
		if (!deco.piece[p][1] && deco.piece[p][0]) deco.piece[p][1] = cairo_surface_reference(deco.piece[p][0]);
	}
	deco.top = deco_h(deco.piece[P_TITLE_3][0]);
	if (deco.top < 12) deco.top = 12;
	deco.left = deco_w(deco.piece[P_LEFT][0]);
	deco.right = deco_w(deco.piece[P_RIGHT][0]);
	deco.bottom = deco_h(deco.piece[P_BOTTOM][0]);
	wlr_log(WLR_INFO, "desktop: frames from xfwm4 theme '%s' (%s), buttons %s, %s GTK colours, title %d, sides %d/%d/%d",
		deco.name, deco.files ? dir : "built in", deco.layout, deco.dark ? "dark" : "light", deco.top,
		deco.left, deco.right, deco.bottom);
	return true;
}

/* where the buttons go in a title bar `w` wide: left group from the left,
 * right group from the right, as button_layout has them */
static int deco_buttons(int w, bool can_shade, struct deco_button *out, int *title_x0, int *title_x1) {
	int n = 0;
	const char *bar = strchr(deco.layout, '|');
	int x = deco.button_offset;
	for (const char *c = deco.layout; c < bar && n < DECO_MAX_BUTTONS; c++) {
		int k = (int)(strchr(deco_kind_letter, *c) - deco_kind_letter);
		if (k == BK_SHADE && !can_shade) continue;
		cairo_surface_t *s = deco.button[k][0][BS_ACTIVE];
		if (!s) continue;
		out[n++] = (struct deco_button){ k, x, deco_w(s), deco_h(s) };
		x += deco_w(s) + deco.button_spacing;
	}
	*title_x0 = x;
	int rx = w - deco.button_offset;
	int first_right = n;
	for (const char *c = deco.layout + strlen(deco.layout) - 1; c > bar && n < DECO_MAX_BUTTONS; c--) {
		int k = (int)(strchr(deco_kind_letter, *c) - deco_kind_letter);
		if (k == BK_SHADE && !can_shade) continue;
		cairo_surface_t *s = deco.button[k][0][BS_ACTIVE];
		if (!s) continue;
		rx -= deco_w(s);
		out[n++] = (struct deco_button){ k, rx, deco_w(s), deco_h(s) };
		rx -= deco.button_spacing;
	}
	(void)first_right;
	*title_x1 = rx;
	return n;
}

/* ---- the app's icon, on the menu button (xfwm4's show_app_icon) --------------------- */

/* Icon= of the app's .desktop file (by app_id), or the app_id itself */
static void deco_icon_name(const char *app_id, char *out, size_t size) {
	snprintf(out, size, "%s", app_id);
	const char *home = getenv("HOME");
	char dirs[6][256];
	snprintf(dirs[0], sizeof(dirs[0]), "%s/.local/share/applications", home ? home : "");
	snprintf(dirs[1], sizeof(dirs[1]), "%s/.local/share/flatpak/exports/share/applications", home ? home : "");
	snprintf(dirs[2], sizeof(dirs[2]), "/var/lib/flatpak/exports/share/applications");
	snprintf(dirs[3], sizeof(dirs[3]), "/usr/local/share/applications");
	snprintf(dirs[4], sizeof(dirs[4]), "/usr/share/applications");
	dirs[5][0] = 0;
	for (int i = 0; dirs[i][0]; i++) {
		char path[600], line[512];
		snprintf(path, sizeof(path), "%s/%s.desktop", dirs[i], app_id);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "Icon=", 5)) {
				line[strcspn(line, "\r\n")] = 0;
				snprintf(out, size, "%s", line + 5);
				break;
			}
		}
		fclose(f);
		return;
	}
}

static cairo_surface_t *deco_icon_file(const char *path, int px) {
	size_t n = strlen(path);
	cairo_surface_t *out = NULL;
	if (n > 4 && !strcmp(path + n - 4, ".png")) {
		cairo_surface_t *png = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS && deco_w(png) > 0) {
			out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
			cairo_t *cr = cairo_create(out);
			cairo_scale(cr, (double)px / deco_w(png), (double)px / deco_h(png));
			cairo_set_source_surface(cr, png, 0, 0);
			cairo_paint(cr);
			cairo_destroy(cr);
		}
		cairo_surface_destroy(png);
	} else if (n > 4 && !strcmp(path + n - 4, ".svg")) {
		GError *error = NULL;
		RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
		if (handle) {
			out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
			cairo_t *cr = cairo_create(out);
			RsvgRectangle viewport = { 0, 0, px, px };
			rsvg_handle_render_document(handle, cr, &viewport, NULL);
			cairo_destroy(cr);
			g_object_unref(handle);
		}
		if (error) g_error_free(error);
	}
	return out;
}

/* The app's icon, `px` pixels square, or NULL. Kept per app_id. */
static cairo_surface_t *deco_app_icon(const char *app_id, int px) {
	static struct { char id[96]; int px; bool done; cairo_surface_t *s; } cache[24];
	static int next;
	if (!app_id || !app_id[0]) return NULL;
	for (int i = 0; i < 24; i++) {
		if (cache[i].done && cache[i].px == px && !strcmp(cache[i].id, app_id)) return cache[i].s;
	}
	char name[256];
	deco_icon_name(app_id, name, sizeof(name));
	cairo_surface_t *s = NULL;
	if (name[0] == '/') {
		s = deco_icon_file(name, px);
	} else if (name[0] && !strchr(name, '/')) {
		const char *home = getenv("HOME");
		char bases[5][256];
		snprintf(bases[0], sizeof(bases[0]), "%s/.local/share/icons/hicolor", home ? home : "");
		snprintf(bases[1], sizeof(bases[1]), "%s/.local/share/flatpak/exports/share/icons/hicolor", home ? home : "");
		snprintf(bases[2], sizeof(bases[2]), "/var/lib/flatpak/exports/share/icons/hicolor");
		snprintf(bases[3], sizeof(bases[3]), "/usr/local/share/icons/hicolor");
		snprintf(bases[4], sizeof(bases[4]), "/usr/share/icons/hicolor");
		static const char *const sizes[] = { "16x16", "22x22", "24x24", "32x32", "48x48", "64x64", "128x128",
			"256x256", NULL };
		char path[700];
		for (int b = 0; b < 5 && !s; b++) {
			for (int z = 0; sizes[z] && !s; z++) {
				snprintf(path, sizeof(path), "%s/%s/apps/%s.png", bases[b], sizes[z], name);
				if (access(path, R_OK) == 0) s = deco_icon_file(path, px);
			}
			snprintf(path, sizeof(path), "%s/scalable/apps/%s.svg", bases[b], name);
			if (!s && access(path, R_OK) == 0) s = deco_icon_file(path, px);
		}
		static const char *const others[] = { "/usr/share/icons/Adwaita/scalable/apps/%s.svg",
			"/usr/share/icons/Adwaita/48x48/apps/%s.png", "/usr/share/pixmaps/%s.png", "/usr/share/pixmaps/%s.svg",
			NULL };
		for (int i = 0; others[i] && !s; i++) {
			snprintf(path, sizeof(path), others[i], name);
			if (access(path, R_OK) == 0) s = deco_icon_file(path, px);
		}
	}
	if (cache[next].s) cairo_surface_destroy(cache[next].s);
	snprintf(cache[next].id, sizeof(cache[next].id), "%s", app_id);
	cache[next].px = px;
	cache[next].s = s;
	cache[next].done = true;
	next = (next + 1) % 24;
	return s;
}

/* a piece, tiled over x, y, w, h */
static void deco_tile(cairo_t *cr, cairo_surface_t *s, double x, double y, double w, double h) {
	if (!s || w <= 0 || h <= 0) return;
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_set_source_surface(cr, s, x, y);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void deco_put(cairo_t *cr, cairo_surface_t *s, double x, double y) {
	if (!s) return;
	cairo_set_source_surface(cr, s, x, y);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
}

/* The title bar, `w` wide (corners included), into cr (in points). */
static void deco_draw_title(cairo_t *cr, int w, const char *title, const char *app_id, float scale, bool focus,
		int hover, int pressed, bool maximized, bool shaded, bool can_shade) {
	int a = focus ? 0 : 1;
	int h = deco.top;
	cairo_surface_t *tl = deco.piece[P_TOP_LEFT][a], *tr = deco.piece[P_TOP_RIGHT][a];
	int x0 = deco_w(tl), x1 = w - deco_w(tr);
	if (x1 < x0) x1 = x0;
	struct deco_button b[DECO_MAX_BUTTONS];
	int tx0, tx1;
	int nb = deco_buttons(w, can_shade, b, &tx0, &tx1);
	/* the text, where full_width_title says (between the button groups) */
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_cairo_context_set_resolution(pango_layout_get_context(layout), 96);
	PangoFontDescription *fd = pango_font_description_from_string(deco.font);
	pango_layout_set_font_description(layout, fd);
	pango_layout_set_text(layout, title, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	int room = tx1 - tx0 - 2 * deco.title_h_off - 4;
	if (room < 1) room = 1;
	pango_layout_set_width(layout, room * PANGO_SCALE);
	pango_layout_set_single_paragraph_mode(layout, true);
	int lw, lh;
	pango_layout_get_pixel_size(layout, &lw, &lh);
	int text_x = deco.align == 0 ? tx0 + deco.title_h_off + 2 :
		deco.align == 2 ? tx1 - deco.title_h_off - 2 - lw : tx0 + (tx1 - tx0 - lw) / 2;
	if (deco.align == 1) {
		/* centred on the bar, as xfwm4 does, unless the buttons are in the way */
		int c = (w - lw) / 2;
		if (c >= tx0 && c + lw <= tx1) text_x = c;
	}
	/* the background: title-3 under the text, 2 and 4 either side, 1 and 5 at the ends */
	cairo_surface_t *t1 = deco.piece[P_TITLE_1][a], *t2 = deco.piece[P_TITLE_2][a], *t3 = deco.piece[P_TITLE_3][a],
		*t4 = deco.piece[P_TITLE_4][a], *t5 = deco.piece[P_TITLE_5][a];
	int a1 = x0 + deco_w(t1), a5 = x1 - deco_w(t5);
	int s3 = text_x - 2, e3 = text_x + lw + 2;
	if (s3 < a1) s3 = a1;
	if (e3 > a5) e3 = a5;
	if (e3 < s3) e3 = s3;
	deco_tile(cr, t2 ? t2 : t3, a1, 0, s3 - a1, h);
	deco_tile(cr, t3, s3, 0, e3 - s3, h);
	deco_tile(cr, t4 ? t4 : t3, e3, 0, a5 - e3, h);
	deco_put(cr, t1, x0, 0);
	deco_put(cr, t5, a5, 0);
	deco_put(cr, tl, 0, 0);
	deco_put(cr, tr, x1, 0);
	/* the text */
	int ty = (h - lh) / 2 + deco.title_v_off[a];
	if (deco.title_shadow[a]) {
		cairo_set_source_rgb(cr, deco.text_shadow[a][0], deco.text_shadow[a][1], deco.text_shadow[a][2]);
		cairo_move_to(cr, text_x + 1, ty + 1);
		pango_cairo_show_layout(cr, layout);
	}
	cairo_set_source_rgb(cr, deco.text[a][0], deco.text[a][1], deco.text[a][2]);
	cairo_move_to(cr, text_x, ty);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(fd);
	g_object_unref(layout);
	/* the buttons */
	for (int i = 0; i < nb; i++) {
		int k = b[i].kind;
		bool toggled = (k == BK_MAXIMIZE && maximized) || (k == BK_SHADE && shaded);
		int st = !focus ? BS_INACTIVE : BS_ACTIVE;
		if (pressed == k) st = BS_PRESSED;
		else if (hover == k) st = BS_PRELIGHT;
		deco_put(cr, deco.button[k][toggled][st], b[i].x, (h - b[i].h) / 2);
		if (k == BK_MENU) {
			/* the app's own icon on it, 16 points, as xfwm4's show_app_icon */
			int px = (int)ceilf(16 * scale);
			cairo_surface_t *icon = deco_app_icon(app_id, px);
			if (icon) {
				cairo_save(cr);
				cairo_translate(cr, b[i].x + (b[i].w - 16) / 2.0, (h - 16) / 2.0);
				cairo_scale(cr, 16.0 / px, 16.0 / px);
				cairo_set_source_surface(cr, icon, 0, 0);
				cairo_paint_with_alpha(cr, focus ? 1.0 : 0.6);
				cairo_restore(cr);
			}
		}
	}
}

/* the left or right side, `h` tall (the bottom corner's top part too) */
static void deco_draw_side(cairo_t *cr, bool left, int h, bool focus) {
	int a = focus ? 0 : 1;
	cairo_surface_t *side = deco.piece[left ? P_LEFT : P_RIGHT][a];
	cairo_surface_t *corner = deco.piece[left ? P_BOTTOM_LEFT : P_BOTTOM_RIGHT][a];
	int w = left ? deco.left : deco.right;
	deco_tile(cr, side, 0, 0, w, h);
	/* the corner, bottom-aligned on the bottom edge (below this), its top part here */
	if (corner) {
		int cw = deco_w(corner), ch = deco_h(corner);
		deco_put(cr, corner, left ? 0 : w - cw, h + deco.bottom - ch);
	}
}

static void deco_draw_bottom(cairo_t *cr, int w, bool focus) {
	int a = focus ? 0 : 1;
	cairo_surface_t *bl = deco.piece[P_BOTTOM_LEFT][a], *br = deco.piece[P_BOTTOM_RIGHT][a];
	deco_tile(cr, deco.piece[P_BOTTOM][a], 0, 0, w, deco.bottom);
	deco_put(cr, bl, 0, deco.bottom - deco_h(bl));
	deco_put(cr, br, w - deco_w(br), deco.bottom - deco_h(br));
}

/* the colours of the window menu, as GTK's menus look in this GTK theme */
static void deco_menu_colors(double bg[3], double fg[3], double sel[3], double sel_fg[3], double border[3],
		double dim[3]) {
	if (deco.dark) {
		deco_rgb(0x353535, bg); deco_rgb(0xeeeeec, fg); deco_rgb(0x15539e, sel);
		deco_rgb(0xffffff, sel_fg); deco_rgb(0x1b1b1b, border); deco_rgb(0x919190, dim);
	} else {
		deco_rgb(0xffffff, bg); deco_rgb(0x2e3436, fg); deco_rgb(0x3584e4, sel);
		deco_rgb(0xffffff, sel_fg); deco_rgb(0xcdc7c2, border); deco_rgb(0x929595, dim);
	}
}
