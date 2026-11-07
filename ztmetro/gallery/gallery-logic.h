// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztGallery's arithmetic and byte parsing, with nothing of the window or of
 * glib in it, so that tests/gallery.c can run it on any machine: which names
 * are photos, a file's size and EXIF orientation from its first bytes, how far
 * to shrink a JPEG while decoding, the grid's cells, and the viewer's zoom.
 *
 * Everything that reads a file takes untrusted bytes: it only ever looks inside
 * the buffer it is given and says "don't know" when something does not add up.
 */
#ifndef GALLERY_LOGIC_H
#define GALLERY_LOGIC_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* ---- names ---------------------------------------------------------------- */

/* A name we would show: JPEG, PNG, GIF, and WebP when there is a loader for it. */
static inline bool gl_is_photo_name(const char *name, bool webp) {
	if (!name || name[0] == '.') return false;
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) return false;
	dot++;
	return !strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg") || !strcasecmp(dot, "png") ||
		!strcasecmp(dot, "gif") || (webp && !strcasecmp(dot, "webp"));
}

/* Whether a folder gets looked into. Hidden folders hold caches and app data. */
static inline bool gl_is_scanned_dir(const char *name) {
	return name && name[0] != '.' && strcasecmp(name, "thumbs") && strcasecmp(name, "thumbnails");
}

/* ---- what a file says about itself ----------------------------------------- */

enum gl_format { GL_UNKNOWN, GL_JPEG, GL_PNG, GL_GIF, GL_WEBP };

static inline uint32_t gl_be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t gl_le16(const uint8_t *p) { return (uint32_t)p[1] << 8 | p[0]; }
static inline uint32_t gl_be32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint32_t gl_le32(const uint8_t *p) {
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* The orientation tag in a TIFF block (what an Exif segment holds). 1..8, or 0
 * for none. */
static inline int gl_tiff_orientation(const uint8_t *t, size_t n) {
	if (n < 8) return 0;
	bool be;
	if (t[0] == 'M' && t[1] == 'M') be = true;
	else if (t[0] == 'I' && t[1] == 'I') be = false;
	else return 0;
	if ((be ? gl_be16(t + 2) : gl_le16(t + 2)) != 42) return 0;
	uint32_t ifd = be ? gl_be32(t + 4) : gl_le32(t + 4);
	if (ifd > n || n - ifd < 2) return 0;
	uint32_t count = be ? gl_be16(t + ifd) : gl_le16(t + ifd);
	const uint8_t *entry = t + ifd + 2;
	size_t left = n - ifd - 2;
	for (uint32_t i = 0; i < count && left >= 12; i++, entry += 12, left -= 12) {
		uint32_t tag = be ? gl_be16(entry) : gl_le16(entry);
		if (tag != 0x0112) continue;
		uint32_t type = be ? gl_be16(entry + 2) : gl_le16(entry + 2);
		uint32_t v;
		if (type == 3) v = be ? gl_be16(entry + 8) : gl_le16(entry + 8);	/* SHORT */
		else if (type == 4) v = be ? gl_be32(entry + 8) : gl_le32(entry + 8);	/* LONG */
		else return 0;
		return v >= 1 && v <= 8 ? (int)v : 0;
	}
	return 0;
}

struct gl_info {
	enum gl_format format;
	int w, h;		/* as stored, before orientation; 0: not found in these bytes */
	int orientation;	/* 1..8; 1 when it says nothing */
};

static inline void gl_probe_jpeg(const uint8_t *b, size_t n, struct gl_info *out) {
	size_t at = 2;
	while (at + 4 <= n) {
		if (b[at] != 0xff) return;
		uint8_t marker = b[at + 1];
		if (marker == 0xff) { at++; continue; }	/* fill byte */
		if (marker == 0xd9 || marker == 0xda) return;	/* end, or the image itself */
		if (marker >= 0xd0 && marker <= 0xd7) { at += 2; continue; }
		uint32_t len = gl_be16(b + at + 2);
		if (len < 2) return;
		const uint8_t *body = b + at + 4;
		size_t room = n - at - 4;
		size_t size = len - 2;
		if (size > room) size = room;
		if (marker == 0xe1 && size > 6 && !memcmp(body, "Exif\0\0", 6)) {
			int o = gl_tiff_orientation(body + 6, size - 6);
			if (o) out->orientation = o;
		}
		/* a frame header: SOF0..SOF15, but for DHT, JPG and DAC */
		if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			if (size >= 5) {
				out->h = (int)gl_be16(body + 1);
				out->w = (int)gl_be16(body + 3);
			}
			return;
		}
		at += 2 + (size_t)len;
	}
}

static inline void gl_probe_webp(const uint8_t *b, size_t n, struct gl_info *out) {
	if (n < 30) return;
	if (!memcmp(b + 12, "VP8 ", 4)) {
		if (b[23] == 0x9d && b[24] == 0x01 && b[25] == 0x2a) {
			out->w = (int)(gl_le16(b + 26) & 0x3fff);
			out->h = (int)(gl_le16(b + 28) & 0x3fff);
		}
	} else if (!memcmp(b + 12, "VP8L", 4)) {
		if (b[20] == 0x2f) {
			uint32_t v = gl_le32(b + 21);
			out->w = (int)(v & 0x3fff) + 1;
			out->h = (int)((v >> 14) & 0x3fff) + 1;
		}
	} else if (!memcmp(b + 12, "VP8X", 4)) {
		out->w = (int)(b[24] | (uint32_t)b[25] << 8 | (uint32_t)b[26] << 16) + 1;
		out->h = (int)(b[27] | (uint32_t)b[28] << 8 | (uint32_t)b[29] << 16) + 1;
	}
}

/* From a file's first bytes (64 KB or so is plenty for almost every file). The
 * format comes from the bytes, never from the name. */
static inline struct gl_info gl_probe(const uint8_t *b, size_t n) {
	struct gl_info out = { GL_UNKNOWN, 0, 0, 1 };
	if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
		out.format = GL_JPEG;
		gl_probe_jpeg(b, n, &out);
	} else if (n >= 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
		out.format = GL_PNG;
		uint32_t w = gl_be32(b + 16), h = gl_be32(b + 20);
		if (w && h && w < 0x7fffffff && h < 0x7fffffff) { out.w = (int)w; out.h = (int)h; }
	} else if (n >= 10 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) {
		out.format = GL_GIF;
		out.w = (int)gl_le16(b + 6);
		out.h = (int)gl_le16(b + 8);
	} else if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) {
		out.format = GL_WEBP;
		gl_probe_webp(b, n, &out);
	}
	return out;
}

/* ---- orientation ------------------------------------------------------------ */

static inline bool gl_orientation_swaps(int orientation) { return orientation >= 5 && orientation <= 8; }

/* Where the pixel at x, y of a w x h image lands for an EXIF orientation. */
static inline void gl_orient_xy(int o, int x, int y, int w, int h, int *nx, int *ny) {
	switch (o) {
	case 2: *nx = w - 1 - x; *ny = y; break;		/* mirrored */
	case 3: *nx = w - 1 - x; *ny = h - 1 - y; break;	/* upside down */
	case 4: *nx = x; *ny = h - 1 - y; break;		/* flipped */
	case 5: *nx = y; *ny = x; break;			/* transposed */
	case 6: *nx = h - 1 - y; *ny = x; break;		/* a quarter turn clockwise */
	case 7: *nx = h - 1 - y; *ny = w - 1 - x; break;	/* transverse */
	case 8: *nx = y; *ny = w - 1 - x; break;		/* a quarter turn back */
	default: *nx = x; *ny = y; break;
	}
}

/* ---- sizes ------------------------------------------------------------------ */

/* How big a photo stored w x h gets, upright, inside box_w x box_h, keeping its
 * shape and never growing. */
static inline void gl_fit_down(int w, int h, int box_w, int box_h, int *out_w, int *out_h) {
	if (w <= 0 || h <= 0) { *out_w = *out_h = 0; return; }
	double k = fmin((double)box_w / w, (double)box_h / h);
	if (k >= 1) { *out_w = w; *out_h = h; return; }
	*out_w = (int)lround(w * k);
	*out_h = (int)lround(h * k);
	if (*out_w < 1) *out_w = 1;
	if (*out_h < 1) *out_h = 1;
}

/* libjpeg decodes at n/8 of the size for a fraction of the time. The smallest n
 * that is still at least `want` of the original (0..1). */
static inline int gl_jpeg_eighths_at_least(double want) {
	int n = (int)ceil(want * 8 - 1e-9);
	return n < 1 ? 1 : n > 8 ? 8 : n;
}

/* The largest n that is at most `limit` of the original: for a size cap. */
static inline int gl_jpeg_eighths_at_most(double limit) {
	int n = (int)floor(limit * 8 + 1e-9);
	return n < 1 ? 1 : n > 8 ? 8 : n;
}

/* Whether decoding this is safe. A JPEG shrinks while it decodes; the others are
 * decoded whole, in pixels of 4 bytes, first. */
static inline bool gl_size_allowed(enum gl_format f, int w, int h, int64_t limit_jpeg, int64_t limit_other) {
	if (w <= 0 || h <= 0) return true;	/* not known: the decoder will say */
	int64_t px = (int64_t)w * h;
	return px <= (f == GL_JPEG ? limit_jpeg : limit_other);
}

/* ---- the grid --------------------------------------------------------------- */

struct gl_grid {
	int cols;
	double x0, cell, gap, row_h;
};

/* `cols` square cells across `width` points: the left margin the page's title
 * has, a smaller one on the right, as Windows Phone's lists have. */
static inline struct gl_grid gl_grid_for(double width, int cols, double left, double right, double gap, double below) {
	struct gl_grid g = { cols, left, 0, gap, 0 };
	g.cell = floor((width - left - right - (cols - 1) * gap) / cols);
	g.row_h = g.cell + below;
	return g;
}

static inline int gl_grid_rows(const struct gl_grid *g, int count) {
	return count <= 0 ? 0 : (count + g->cols - 1) / g->cols;
}

/* The cell under x in a row: the nearest, as a finger is wider than the gaps. */
static inline int gl_grid_col(const struct gl_grid *g, double x) {
	int c = (int)floor((x - g->x0 + g->gap / 2) / (g->cell + g->gap));
	return c < 0 ? 0 : c >= g->cols ? g->cols - 1 : c;
}

static inline double gl_grid_x(const struct gl_grid *g, int col) {
	return g->x0 + col * (g->cell + g->gap);
}

/* Rows first..last are what a list starting at `top` shows between view_top and
 * view_bottom (page heights). False when none are. */
static inline bool gl_rows_in(const struct gl_grid *g, int rows, double top, double view_top, double view_bottom,
		int *first, int *last) {
	if (rows <= 0) return false;
	int a = (int)floor((view_top - top) / g->row_h);
	int b = (int)floor((view_bottom - 0.001 - top) / g->row_h);
	if (a < 0) a = 0;
	if (b >= rows) b = rows - 1;
	if (a > b) return false;
	*first = a;
	*last = b;
	return true;
}

/* ---- the viewer: zoom and pan ------------------------------------------------ */

/* zoom 1 fits the photo on the screen; x, y is where the photo's middle is from
 * the screen's middle, in points. */
struct zv {
	double zoom, x, y;
};

#define ZV_RUBBER_MIN	0.6	/* how small a pinch can make it, before it springs back */
#define ZV_MIN_MAX	3.0

/* The size of a photo at zoom 1. */
static inline void zv_fit(double vw, double vh, double iw, double ih, double *fw, double *fh) {
	if (iw <= 0 || ih <= 0) { *fw = vw; *fh = vh; return; }
	double k = fmin(vw / iw, vh / ih);
	*fw = iw * k;
	*fh = ih * k;
}

/* How far in it goes: to twice its real pixels, at least ZV_MIN_MAX, at most 12. */
static inline double zv_max_zoom(double fw, double scale, double orig_w) {
	if (fw <= 0) return ZV_MIN_MAX;
	double native = orig_w / (fw * scale);
	double m = native * 2;
	return m < ZV_MIN_MAX ? ZV_MIN_MAX : m > 12 ? 12 : m;
}

/* Inside the screen: a side that is smaller than the screen is centred, a side
 * that is larger keeps the screen covered. */
static inline struct zv zv_clamped(struct zv s, double vw, double vh, double fw, double fh, double max_zoom) {
	if (s.zoom < 1) s.zoom = 1;
	if (s.zoom > max_zoom) s.zoom = max_zoom;
	double mx = (fw * s.zoom - vw) / 2, my = (fh * s.zoom - vh) / 2;
	if (mx < 0) mx = 0;
	if (my < 0) my = 0;
	s.x = fmax(-mx, fmin(mx, s.x));
	s.y = fmax(-my, fmin(my, s.y));
	return s;
}

/* Two fingers: the photo's point that was between them at d0 stays between them
 * as they move. Past the ends the zoom gives only a little. Positions are from
 * the screen's middle. */
static inline struct zv zv_pinch(struct zv from, double d0, double d, double m0x, double m0y, double mx, double my,
		double max_zoom) {
	struct zv s = from;
	if (d0 < 1) d0 = 1;
	double z = from.zoom * d / d0;
	if (z < 1) z = 1 - (1 - z) * 0.5;
	if (z < ZV_RUBBER_MIN) z = ZV_RUBBER_MIN;
	if (z > max_zoom) z = max_zoom * (1 + (z / max_zoom - 1) * 0.25);
	s.zoom = z;
	s.x = mx - (m0x - from.x) * z / from.zoom;
	s.y = my - (m0y - from.y) * z / from.zoom;
	return s;
}

/* A double tap: in on the point tapped, or back out to the whole photo. */
static inline struct zv zv_double_tap(struct zv from, double px, double py, double vw, double vh, double fw, double fh,
		double in_zoom, double max_zoom) {
	struct zv s = { 1, 0, 0 };
	if (from.zoom > 1.05) return s;
	if (in_zoom > max_zoom) in_zoom = max_zoom;
	s.zoom = in_zoom;
	s.x = px - (px - from.x) * in_zoom / from.zoom;
	s.y = py - (py - from.y) * in_zoom / from.zoom;
	return zv_clamped(s, vw, vh, fw, fh, max_zoom);
}

/* A drag across at zoom 1, let go at dx points and vx points/ms: the next photo
 * (+1), the one before (-1), or this one still (0). */
static inline int zv_swipe_result(double dx, double vx, double vw, bool has_prev, bool has_next) {
	int to = 0;
	if (dx < -vw / 3 || vx < -0.4) to = 1;
	else if (dx > vw / 3 || vx > 0.4) to = -1;
	if (to > 0 && (!has_next || dx > 0)) return 0;
	if (to < 0 && (!has_prev || dx < 0)) return 0;
	return to;
}

/* ---- easing ----------------------------------------------------------------- */

static inline double gl_ease_out(double t) {
	if (t <= 0) return 0;
	if (t >= 1) return 1;
	double u = 1 - t;
	return 1 - u * u * u;
}

/* ---- a picker's answer ------------------------------------------------------- */

/* What goes on stdout: the path as it is, every byte, and its terminator. */
static inline size_t gl_pick_line(const char *path, bool nul, char *out, size_t room) {
	size_t n = strlen(path);
	if (n + 1 > room) return 0;
	memcpy(out, path, n);
	out[n] = nul ? '\0' : '\n';
	return n + 1;
}

#endif
