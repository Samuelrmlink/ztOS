// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztGallery: the phone's photos, as Windows Phone's Photos hub had them. A pivot
 * of "photos" (everything, newest first) and "albums" (by folder), and a full
 * screen viewer with swipe, pinch, double tap and delete. It also answers other
 * programs: `ztgallery --pick` lets the user choose a photo and prints its path.
 * See README.md for that, and for where photos are looked for.
 *
 *	ztgallery [PATH]		the gallery, or one photo open in it
 *	ztgallery --pick		choose a photo: its path on stdout, exit 0;
 *					backed out: nothing, exit 1
 *	ztgallery --pick-null		the same, ended by a NUL instead of a newline
 *	ztgallery --pick-out FILE	the same, the path written to FILE instead
 *	ztgallery --render PAGE FILE	a page as a PNG (photos, albums), no display
 *
 * What never happens on the main thread: reading a directory or decoding a
 * photo. A scan thread walks the folders, two workers make thumbnails, one more
 * decodes what the viewer wants; they hand finished, immutable surfaces over a
 * queue, and an eventfd wakes the main loop.
 *
 * What a frame costs: rows of the grid are only grey squares and are drawn on the
 * core's own list thread; every thumbnail is a small texture of its own, drawn
 * over its square from .overlay, so a new thumbnail never redraws the page (the
 * way Settings' wallpaper grid works). Photos in the viewer are horizontal bands
 * of textures, uploaded a few per frame, and their pixels are let go of once the
 * GPU has them.
 */
#define _GNU_SOURCE
#include "zrnmetrocore.h"
#include "zrnmetrogpu.h"
#include "gallery-logic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBJPEG
#include <jpeglib.h>
#endif

#define THUMB_PX	264	/* a side of a cached thumbnail, pixels */
#define THUMB_SLOTS	72	/* thumbnails held at once, in memory and on the GPU */
#define THUMB_WORKERS	2
#define UPLOADS_PER_FRAME 4	/* thumbnails sent to the GPU in one frame */
#define WANT_MAX	64
#define WANT_AHEAD_ROWS	5	/* rows made ahead of the screen, either way */
#define SCAN_MAX_PHOTOS	50000
#define SCAN_MAX_DIRS	4096
#define SCAN_MAX_DEPTH	8
#define MAX_WATCHES	512
#define CACHE_MAX_BYTES	(256 * 1024 * 1024LL)
#define CACHE_TRIM_BYTES (200 * 1024 * 1024LL)
#define HIRES_LONG_SIDE	4096	/* the most of a photo ever held, zoomed in */
#define LIMIT_JPEG_PX	500000000LL	/* these decode shrunk: the file is the limit */
#define LIMIT_OTHER_PX	100000000LL	/* these decode whole first */
#define BAND_ROWS	256	/* a photo's rows to a texture */
#define UPLOAD_BYTES_PER_FRAME (6 * 1024 * 1024)
#define PHOTO_GAP	24	/* between two photos, swiping */
#define OPEN_MS		220
#define SLIDE_MS	240
#define ZOOM_MS		200
#define THUMB_FADE_MS	140
#define DOUBLE_TAP_MS	280
#define DOUBLE_TAP_ZOOM	2.5
#define SLOP_PT		10
#define HIRES_ABOVE_ZOOM 1.2
#define BAR_H		78
#define TOP_H		58

enum page { PAGE_PHOTOS, PAGE_ALBUMS, PAGE_ALBUM };
enum action { ACT_PHOTO = 1, ACT_ALBUM };
enum thumb_state { T_NONE, T_QUEUED, T_READY, T_BAD };

struct album;

struct photo {
	char *path;
	const char *name;	/* the last part of path */
	int64_t mtime_ns, size;
	struct album *album;
	int slot;		/* its thumbnail's place, or -1 */
	uint8_t state;		/* enum thumb_state */
	bool seen;		/* (while a new scan is merged) */
};

struct album {
	char *dir, *name;
	struct photo **items;	/* newest first */
	int n, room;
};

struct slot {
	struct photo *photo;
	cairo_surface_t *cpu;
	unsigned tex;
	bool uploaded;
	uint32_t arrive;
	uint64_t used;
};

/* the app's own world: every one of these is only touched on the main thread,
 * and with zmc_list_lock held wherever .render reads it */
static struct photo **all;
static int n_all;
static struct album **albums;
static int n_albums;
static GHashTable *by_path;	/* path -> struct photo */
static struct slot slots[THUMB_SLOTS];
static uint64_t frame_no;
static uint64_t list_rev = 1;
static bool scanned, in_render;
static double list_top[3];
static char *album_open_dir;	/* the album on PAGE_ALBUM */
static struct album *album_open;
static const char *home;
static char *thumb_dir;

/* the picker */
static bool pick_mode, pick_null;
static const char *pick_out;
static char *picked;

/* opened on one photo: `ztgallery PATH`. It stays one of the photos wherever it
 * is, so it is found again by every scan (the scan thread reads extra_path) */
static char *open_path, *extra_path;

static int wake_fd = -1;

extern struct zmc_app gallery_app;

static const struct gl_grid *grid_photos(void);
static const struct gl_grid *grid_albums(void);
static void viewer_overlay(void);
static void viewer_open(struct photo **list, int n, const char *album_dir, int index);
static void viewer_rebuilt(void);
static bool viewer_is_open(void);
static bool viewer_covers_grid(void);
static void viewer_got_image(const char *path, int level, cairo_surface_t *s, int ow, int oh);

static void wake_main(void) {
	uint64_t one = 1;
	if (write(wake_fd, &one, sizeof(one)) < 0) { /* it is awake already */ }
}

/* ---- decoding: any thread ---------------------------------------------------- */

enum decode_mode {
	DECODE_COVER,	/* its shorter side at least a */
	DECODE_FIT,	/* inside a x b, upright */
	DECODE_CAP,	/* its longer side at most a */
};

static bool surface_good(cairo_surface_t *s) {
	return s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *surface_new(int w, int h) {
	if (w <= 0 || h <= 0 || w > 32767 || h > 32767) return NULL;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	if (!surface_good(s)) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

/* The file's first bytes, for what it says about itself. */
static struct gl_info read_head(const char *path) {
	static const size_t room = 256 * 1024;
	struct gl_info info = { GL_UNKNOWN, 0, 0, 1 };
	uint8_t *b = malloc(room);
	if (!b) return info;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd >= 0) {
		size_t have = 0;
		for (;;) {
			ssize_t n = read(fd, b + have, room - have);
			if (n <= 0) break;
			have += (size_t)n;
			if (have == room) break;
		}
		info = gl_probe(b, have);
		close(fd);
	}
	free(b);
	return info;
}

/* upright, in a new surface */
static cairo_surface_t *surface_oriented(cairo_surface_t *src, int o) {
	if (o <= 1 || o > 8) return src;
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	bool swap = gl_orientation_swaps(o);
	cairo_surface_t *out = surface_new(swap ? h : w, swap ? w : h);
	if (!out) return src;	/* no memory for it: sideways rather than nothing */
	cairo_surface_flush(src);
	const uint8_t *from = cairo_image_surface_get_data(src);
	uint8_t *to = cairo_image_surface_get_data(out);
	int fs = cairo_image_surface_get_stride(src), ts = cairo_image_surface_get_stride(out);
	for (int y = 0; y < h; y++) {
		const uint32_t *line = (const uint32_t *)(from + (size_t)y * fs);
		for (int x = 0; x < w; x++) {
			int nx, ny;
			gl_orient_xy(o, x, y, w, h, &nx, &ny);
			((uint32_t *)(to + (size_t)ny * ts))[nx] = line[x];
		}
	}
	cairo_surface_mark_dirty(out);
	cairo_surface_destroy(src);
	return out;
}

/* A part of src (sx, sy, sw, sh) to tw x th, by plain bilinear. Not Cairo: under
 * DotMatrix its painting goes through the style's wrappers, which are not this
 * thread's to use. */
static cairo_surface_t *surface_resampled(cairo_surface_t *src, double sx, double sy, double sw, double sh, int tw, int th) {
	cairo_surface_t *out = surface_new(tw, th);
	if (!out) return NULL;
	cairo_surface_flush(src);
	const uint8_t *from = cairo_image_surface_get_data(src);
	int fs = cairo_image_surface_get_stride(src);
	int iw = cairo_image_surface_get_width(src), ih = cairo_image_surface_get_height(src);
	uint8_t *to = cairo_image_surface_get_data(out);
	int ts = cairo_image_surface_get_stride(out);
	for (int y = 0; y < th; y++) {
		double fy = sy + (y + 0.5) * sh / th - 0.5;
		int y0 = (int)floor(fy);
		uint32_t wy = (uint32_t)((fy - y0) * 256);
		if (y0 < 0) { y0 = 0; wy = 0; }
		int y1 = y0 + 1 < ih ? y0 + 1 : y0;
		if (y0 >= ih) y0 = y1 = ih - 1;
		const uint32_t *a = (const uint32_t *)(from + (size_t)y0 * fs);
		const uint32_t *b = (const uint32_t *)(from + (size_t)y1 * fs);
		uint32_t *line = (uint32_t *)(to + (size_t)y * ts);
		for (int x = 0; x < tw; x++) {
			double fx = sx + (x + 0.5) * sw / tw - 0.5;
			int x0 = (int)floor(fx);
			uint32_t wx = (uint32_t)((fx - x0) * 256);
			if (x0 < 0) { x0 = 0; wx = 0; }
			int x1 = x0 + 1 < iw ? x0 + 1 : x0;
			if (x0 >= iw) x0 = x1 = iw - 1;
			uint32_t p00 = a[x0], p01 = a[x1], p10 = b[x0], p11 = b[x1], v = 0;
			for (int c = 0; c < 24; c += 8) {
				uint32_t top = ((p00 >> c & 255) * (256 - wx) + (p01 >> c & 255) * wx) >> 8;
				uint32_t bot = ((p10 >> c & 255) * (256 - wx) + (p11 >> c & 255) * wx) >> 8;
				v |= ((top * (256 - wy) + bot * wy) >> 8) << c;
			}
			line[x] = v;
		}
	}
	cairo_surface_mark_dirty(out);
	return out;
}

/* The whole of src, a bit too large for its box after decoding: to the box. */
static cairo_surface_t *surface_within(cairo_surface_t *src, int box_w, int box_h) {
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	if (w <= box_w * 5 / 4 && h <= box_h * 5 / 4) return src;
	int tw, th;
	gl_fit_down(w, h, box_w, box_h, &tw, &th);
	cairo_surface_t *out = surface_resampled(src, 0, 0, w, h, tw, th);
	if (!out) return src;
	cairo_surface_destroy(src);
	return out;
}

/* How much of the photo (0..1) it takes to give what the mode asks; upright sizes. */
static double decode_share(enum decode_mode mode, int a, int b, int w, int h) {
	if (mode == DECODE_COVER) return fmin(1, (double)a / (w < h ? w : h));
	if (mode == DECODE_FIT) return fmin(1, fmin((double)a / w, (double)b / h));
	return fmin(1, (double)a / (w > h ? w : h));
}

#ifdef HAVE_LIBJPEG
/* libjpeg shrinks while it decodes, which is why a thumbnail of 12 MP costs tens
 * of milliseconds here and most of a second through glycin (a whole decode in a
 * sandboxed process). */
struct jpeg_fail {
	struct jpeg_error_mgr mgr;
	jmp_buf jump;
};

static void jpeg_fail_exit(j_common_ptr info) {
	longjmp(((struct jpeg_fail *)info->err)->jump, 1);
}

static void jpeg_fail_quiet(j_common_ptr info, int level) {
	(void)info;
	(void)level;
}

static cairo_surface_t *jpeg_decode(const char *path, enum decode_mode mode, int a, int b, int orientation,
		int *orig_w, int *orig_h) {
	FILE *volatile f = fopen(path, "rbe");
	if (!f) return NULL;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_fail fail;
	cairo_surface_t *volatile out = NULL;
	cinfo.err = jpeg_std_error(&fail.mgr);
	fail.mgr.error_exit = jpeg_fail_exit;
	fail.mgr.emit_message = jpeg_fail_quiet;
	if (setjmp(fail.jump)) {
		if (out) cairo_surface_destroy(out);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	bool swap = gl_orientation_swaps(orientation);
	int uw = swap ? (int)cinfo.image_height : (int)cinfo.image_width;
	int uh = swap ? (int)cinfo.image_width : (int)cinfo.image_height;
	*orig_w = uw;
	*orig_h = uh;
	double share = decode_share(mode, a, b, uw, uh);
	cinfo.scale_num = (unsigned)(mode == DECODE_CAP ? gl_jpeg_eighths_at_most(share) : gl_jpeg_eighths_at_least(share));
	cinfo.scale_denom = 8;
	cinfo.out_color_space = JCS_EXT_BGRX;	/* Cairo's RGB24, on this machine */
	/* smoothing colour costs much of the time, for nothing one can see when shrunk */
	cinfo.do_fancy_upsampling = cinfo.scale_num >= 8;
	jpeg_calc_output_dimensions(&cinfo);
	out = surface_new((int)cinfo.output_width, (int)cinfo.output_height);
	if (!out) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}
	jpeg_start_decompress(&cinfo);
	uint8_t *data = cairo_image_surface_get_data(out);
	int stride = cairo_image_surface_get_stride(out);
	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row = data + (size_t)cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);
	cairo_surface_mark_dirty(out);
	return out;
}

/* A thumbnail for the cache. */
static bool jpeg_write(const char *path, cairo_surface_t *s) {
	FILE *f = fopen(path, "wbe");
	if (!f) return false;
	struct jpeg_compress_struct cinfo;
	struct jpeg_fail fail;
	cinfo.err = jpeg_std_error(&fail.mgr);
	fail.mgr.error_exit = jpeg_fail_exit;
	fail.mgr.emit_message = jpeg_fail_quiet;
	if (setjmp(fail.jump)) {
		jpeg_destroy_compress(&cinfo);
		fclose(f);
		return false;
	}
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, f);
	cinfo.image_width = (JDIMENSION)cairo_image_surface_get_width(s);
	cinfo.image_height = (JDIMENSION)cairo_image_surface_get_height(s);
	cinfo.input_components = 4;
	cinfo.in_color_space = JCS_EXT_BGRX;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, 85, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	cairo_surface_flush(s);
	uint8_t *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = data + (size_t)cinfo.next_scanline * stride;
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	return fclose(f) == 0;
}
#endif

/* Opaque, over black: a photo with holes in it has nothing else behind it here. */
static cairo_surface_t *pixbuf_to_surface(GdkPixbuf *p) {
	int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
	cairo_surface_t *s = surface_new(w, h);
	if (!s) return NULL;
	int channels = gdk_pixbuf_get_n_channels(p), from_stride = gdk_pixbuf_get_rowstride(p);
	const guchar *from = gdk_pixbuf_read_pixels(p);
	uint8_t *to = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	bool alpha = gdk_pixbuf_get_has_alpha(p);
	for (int y = 0; y < h; y++) {
		const guchar *in = from + (size_t)y * from_stride;
		uint32_t *out = (uint32_t *)(to + (size_t)y * stride);
		for (int x = 0; x < w; x++, in += channels) {
			uint32_t r = in[0], g = in[1], b = in[2];
			if (alpha) {
				uint32_t k = in[3];
				r = r * k / 255;
				g = g * k / 255;
				b = b * k / 255;
			}
			out[x] = 0xff000000u | r << 16 | g << 8 | b;
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

/* Through gdk-pixbuf: GIF (its first frame), PNG, WebP, and a JPEG libjpeg did
 * not take. It decodes the whole picture and then scales it. */
static cairo_surface_t *pixbuf_decode(const char *path, struct gl_info info, enum decode_mode mode, int a, int b,
		int *orig_w, int *orig_h) {
	int w = info.w, h = info.h;
	if ((w <= 0 || h <= 0) && !gdk_pixbuf_get_file_info(path, &w, &h)) return NULL;
	if (w <= 0 || h <= 0) return NULL;
	if (!gl_size_allowed(GL_PNG, w, h, LIMIT_JPEG_PX, LIMIT_OTHER_PX)) return NULL;
	bool swap = gl_orientation_swaps(info.orientation);
	int uw = swap ? h : w, uh = swap ? w : h;
	double share = decode_share(mode, a, b, uw, uh);
	int want_w = (int)ceil(w * share), want_h = (int)ceil(h * share);
	if (want_w < 1) want_w = 1;
	if (want_h < 1) want_h = 1;
	GError *error = NULL;
	GdkPixbuf *p = gdk_pixbuf_new_from_file_at_scale(path, want_w, want_h, TRUE, &error);
	if (!p) {
		g_clear_error(&error);
		return NULL;
	}
	GdkPixbuf *up = gdk_pixbuf_apply_embedded_orientation(p);
	g_object_unref(p);
	if (!up) return NULL;
	cairo_surface_t *s = pixbuf_to_surface(up);
	/* the option's orientation is what counts here: a PNG may carry one too */
	const char *option = gdk_pixbuf_get_option(up, "orientation");
	if (option && atoi(option) >= 5 && atoi(option) <= 8 && !swap) {
		int t = uw;
		uw = uh;
		uh = t;
	}
	g_object_unref(up);
	*orig_w = uw;
	*orig_h = uh;
	return s;
}

/* A photo, upright. Corrupt or too large: NULL, and nothing else happens. */
static cairo_surface_t *decode_photo(const char *path, enum decode_mode mode, int a, int b, int *orig_w, int *orig_h) {
	struct gl_info info = read_head(path);
	if (!gl_size_allowed(info.format, info.w, info.h, LIMIT_JPEG_PX, LIMIT_OTHER_PX)) return NULL;
	cairo_surface_t *s = NULL;
#ifdef HAVE_LIBJPEG
	if (info.format == GL_JPEG) {
		s = jpeg_decode(path, mode, a, b, info.orientation, orig_w, orig_h);
		if (s) s = surface_oriented(s, info.orientation);
	}
#endif
	if (!s) s = pixbuf_decode(path, info, mode, a, b, orig_w, orig_h);
	if (s && mode == DECODE_FIT) s = surface_within(s, a, b);
	if (s && mode == DECODE_CAP) s = surface_within(s, a, a);
	return s;
}

/* ---- the thumbnail cache: worker threads ------------------------------------ */

/* The name of a photo's thumbnail: of its path, time and size, so that a changed
 * photo makes a new one, and any file name at all makes a safe one. */
static char *thumb_key(const char *path, int64_t mtime_ns, int64_t size) {
	char *identity = g_strdup_printf("v1:%d:%s:%lld:%lld", THUMB_PX, path, (long long)mtime_ns, (long long)size);
	char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
	g_free(identity);
	return key;
}

static char *thumb_file(const char *key, const char *ending) {
	return g_strdup_printf("%s/%s%s", thumb_dir, key, ending);
}

#ifdef HAVE_LIBJPEG
#define THUMB_ENDING ".jpg"
#else
#define THUMB_ENDING ".png"
#endif

static cairo_surface_t *thumb_cover(const char *path) {
	int ow = 0, oh = 0;
	cairo_surface_t *src = decode_photo(path, DECODE_COVER, THUMB_PX, THUMB_PX, &ow, &oh);
	if (!src) return NULL;
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	int side = w < h ? w : h;
	if (w == THUMB_PX && h == THUMB_PX) return src;
	cairo_surface_t *out = surface_resampled(src, (w - side) / 2.0, (h - side) / 2.0, side, side, THUMB_PX, THUMB_PX);
	cairo_surface_destroy(src);
	return out;
}

static cairo_surface_t *thumb_read(const char *file) {
	cairo_surface_t *s = NULL;
#ifdef HAVE_LIBJPEG
	int ow, oh;
	s = jpeg_decode(file, DECODE_CAP, THUMB_PX, THUMB_PX, 1, &ow, &oh);
#else
	s = cairo_image_surface_create_from_png(file);
	if (!surface_good(s)) {
		cairo_surface_destroy(s);
		s = NULL;
	}
#endif
	if (s && (cairo_image_surface_get_width(s) != THUMB_PX || cairo_image_surface_get_height(s) != THUMB_PX)) {
		cairo_surface_destroy(s);
		s = NULL;
	}
	return s;
}

static void thumb_write(const char *file, cairo_surface_t *s) {
	char *tmp = g_strdup_printf("%s.%ld.tmp", file, (long)syscall(SYS_gettid));
#ifdef HAVE_LIBJPEG
	bool ok = jpeg_write(tmp, s);
#else
	bool ok = cairo_surface_write_to_png(s, tmp) == CAIRO_STATUS_SUCCESS;
#endif
	if (!ok || rename(tmp, file) != 0) unlink(tmp);
	g_free(tmp);
}

/* From the cache, or made and put there. A file that cannot be read leaves a
 * marker, so that it costs one attempt until it changes. */
static cairo_surface_t *thumb_get(const char *path, int64_t mtime_ns, int64_t size) {
	char *key = thumb_key(path, mtime_ns, size);
	char *file = thumb_file(key, THUMB_ENDING), *bad = thumb_file(key, ".bad");
	cairo_surface_t *s = NULL;
	if (access(bad, F_OK) != 0) {
		s = thumb_read(file);
		if (!s) {
			s = thumb_cover(path);
			if (s) {
				thumb_write(file, s);
			} else {
				int fd = open(bad, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
				if (fd >= 0) close(fd);
			}
		}
	}
	g_free(key);
	g_free(file);
	g_free(bad);
	return s;
}

static void thumb_forget(const char *path, int64_t mtime_ns, int64_t size) {
	char *key = thumb_key(path, mtime_ns, size);
	char *file = thumb_file(key, THUMB_ENDING), *bad = thumb_file(key, ".bad");
	unlink(file);
	unlink(bad);
	g_free(key);
	g_free(file);
	g_free(bad);
}

struct cache_entry {
	char *path;
	time_t when;
	off_t size;
};

static int cache_entry_cmp(const void *a, const void *b) {
	const struct cache_entry *x = a, *y = b;
	return x->when < y->when ? -1 : x->when > y->when;
}

/* The cache stays under its size: the oldest go first. */
static void cache_trim(void) {
	DIR *d = opendir(thumb_dir);
	if (!d) return;
	GArray *entries = g_array_new(FALSE, FALSE, sizeof(struct cache_entry));
	long long total = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		struct stat st;
		if (e->d_name[0] == '.' || fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
		struct cache_entry entry = { g_build_filename(thumb_dir, e->d_name, NULL), st.st_mtime, st.st_size };
		g_array_append_val(entries, entry);
		total += st.st_size;
	}
	closedir(d);
	if (total > CACHE_MAX_BYTES) {
		g_array_sort(entries, cache_entry_cmp);
		for (guint i = 0; i < entries->len && total > CACHE_TRIM_BYTES; i++) {
			struct cache_entry *entry = &g_array_index(entries, struct cache_entry, i);
			if (unlink(entry->path) == 0) total -= entry->size;
		}
	}
	for (guint i = 0; i < entries->len; i++) g_free(g_array_index(entries, struct cache_entry, i).path);
	g_array_free(entries, TRUE);
}

/* ---- jobs and results -------------------------------------------------------- */

struct thumb_job {
	char *path;
	int64_t mtime_ns, size;
	struct photo *photo;	/* for the main thread only: the workers never look */
};

struct image_job {
	char *path;
	int level;		/* 0: fits the screen, 1: for zooming in */
	int box_w, box_h;
};

enum result_kind { R_THUMB, R_IMAGE, R_SCAN };

struct scan_file {
	char *path;
	int64_t mtime_ns, size;
};

struct result {
	enum result_kind kind;
	char *path;
	cairo_surface_t *surface;
	int level, orig_w, orig_h;
	GArray *files;		/* R_SCAN */
};

static GAsyncQueue *results;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_wake = PTHREAD_COND_INITIALIZER;
static struct thumb_job thumb_jobs[WANT_MAX];	/* a stack: the last is next */
static int n_thumb_jobs;
#define IMAGE_JOBS 4
static struct image_job image_jobs[IMAGE_JOBS];	/* in order */
static int n_image_jobs;
static pthread_cond_t image_wake = PTHREAD_COND_INITIALIZER;
static char *image_busy;	/* what the worker has in hand now */
static int image_busy_level;
static bool workers_started;

static void result_push(struct result *r) {
	g_async_queue_push(results, r);
	wake_main();
}

static void worker_background(void) {
	/* photos come second to the finger */
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
}

static void *thumb_worker(void *unused) {
	(void)unused;
	worker_background();
	pthread_mutex_lock(&job_lock);
	for (;;) {
		if (!n_thumb_jobs) {
			pthread_cond_wait(&job_wake, &job_lock);
			continue;
		}
		struct thumb_job job = thumb_jobs[--n_thumb_jobs];
		pthread_mutex_unlock(&job_lock);
		struct result *r = g_new0(struct result, 1);
		r->kind = R_THUMB;
		r->path = job.path;
		r->surface = thumb_get(job.path, job.mtime_ns, job.size);
		result_push(r);
		pthread_mutex_lock(&job_lock);
	}
	return NULL;
}

static void *image_worker(void *unused) {
	(void)unused;
	worker_background();
	pthread_mutex_lock(&job_lock);
	for (;;) {
		if (!n_image_jobs) {
			pthread_cond_wait(&image_wake, &job_lock);
			continue;
		}
		struct image_job job = image_jobs[0];
		memmove(image_jobs, image_jobs + 1, sizeof(image_jobs[0]) * (size_t)--n_image_jobs);
		image_busy = g_strdup(job.path);
		image_busy_level = job.level;
		pthread_mutex_unlock(&job_lock);
		struct result *r = g_new0(struct result, 1);
		r->kind = R_IMAGE;
		r->path = job.path;
		r->level = job.level;
		r->surface = job.level == 0 ?
			decode_photo(job.path, DECODE_FIT, job.box_w, job.box_h, &r->orig_w, &r->orig_h) :
			decode_photo(job.path, DECODE_CAP, HIRES_LONG_SIDE, HIRES_LONG_SIDE, &r->orig_w, &r->orig_h);
		pthread_mutex_lock(&job_lock);
		g_free(image_busy);
		image_busy = NULL;
		pthread_mutex_unlock(&job_lock);
		result_push(r);	/* (after: it is not seen as in hand once its result is in) */
		pthread_mutex_lock(&job_lock);
	}
	return NULL;
}

static void workers_start(void) {
	if (workers_started) return;
	workers_started = true;
	for (int i = 0; i < THUMB_WORKERS; i++) {
		pthread_t t;
		if (pthread_create(&t, NULL, thumb_worker, NULL) == 0) pthread_detach(t);
	}
	pthread_t t;
	if (pthread_create(&t, NULL, image_worker, NULL) == 0) pthread_detach(t);
}

/* ---- finding the photos: a thread of its own --------------------------------- */

static int ino_fd = -1;
static bool scan_running, scan_again;
static bool webp_loadable;

struct scan_state {
	GArray *files;
	GHashTable *dirs;	/* dev:inode seen, so a bind or a link cannot loop */
	int n_dirs, n_watches;
};

static void scan_add_file(struct scan_state *st, const char *path, const struct stat *s) {
	struct scan_file f = { g_strdup(path), s->st_mtim.tv_sec * 1000000000LL + s->st_mtim.tv_nsec, s->st_size };
	g_array_append_val(st->files, f);
}

static void scan_dir(struct scan_state *st, const char *dir, int depth) {
	if (st->n_dirs >= SCAN_MAX_DIRS || (int)st->files->len >= SCAN_MAX_PHOTOS) return;
	struct stat ds;
	if (stat(dir, &ds) != 0 || !S_ISDIR(ds.st_mode)) return;
	char *id = g_strdup_printf("%llu:%llu", (unsigned long long)ds.st_dev, (unsigned long long)ds.st_ino);
	if (g_hash_table_contains(st->dirs, id)) {
		g_free(id);
		return;
	}
	g_hash_table_add(st->dirs, id);
	st->n_dirs++;
	if (ino_fd >= 0 && st->n_watches < MAX_WATCHES) {
		/* a folder that is added, or a photo in one, is noticed */
		if (inotify_add_watch(ino_fd, dir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
				IN_DELETE_SELF | IN_ONLYDIR) >= 0) {
			st->n_watches++;
		}
	}
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		struct stat s;
		if (fstatat(dirfd(d), e->d_name, &s, AT_SYMLINK_NOFOLLOW) != 0) continue;
		/* (links are not followed: the real file is found where it lives, or not) */
		if (S_ISDIR(s.st_mode)) {
			if (depth > 0 && gl_is_scanned_dir(e->d_name)) {
				char *sub = g_build_filename(dir, e->d_name, NULL);
				scan_dir(st, sub, depth - 1);
				g_free(sub);
			}
		} else if (S_ISREG(s.st_mode) && gl_is_photo_name(e->d_name, webp_loadable)) {
			if ((int)st->files->len >= SCAN_MAX_PHOTOS) break;
			char *path = g_build_filename(dir, e->d_name, NULL);
			scan_add_file(st, path, &s);
			g_free(path);
		}
	}
	closedir(d);
}

/* Where photos live. Camera saves to ~/Pictures/ztCamera; Android's folders, as
 * Waydroid and ATL share them, are in ~/Android. ~/Downloads only at its top. */
static void scan_roots(struct scan_state *st) {
	const char *xdg = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	static const struct { const char *sub; int depth; } roots[] = {
		{ "Pictures", SCAN_MAX_DEPTH }, { "DCIM", SCAN_MAX_DEPTH }, { "Screenshots", SCAN_MAX_DEPTH },
		{ "Photos", SCAN_MAX_DEPTH }, { "Android/DCIM", SCAN_MAX_DEPTH }, { "Android/Pictures", SCAN_MAX_DEPTH },
		{ "Downloads", 0 },
	};
	if (xdg) scan_dir(st, xdg, SCAN_MAX_DEPTH);
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		char *path = g_build_filename(home, roots[i].sub, NULL);
		scan_dir(st, path, roots[i].depth);
		g_free(path);
	}
	if (extra_path) {
		struct stat s;
		bool known = false;
		for (guint i = 0; i < st->files->len && !known; i++) {
			known = !strcmp(g_array_index(st->files, struct scan_file, i).path, extra_path);
		}
		if (!known && lstat(extra_path, &s) == 0 && S_ISREG(s.st_mode)) scan_add_file(st, extra_path, &s);
	}
}

static GArray *scan_files(void) {
	struct scan_state st = { g_array_new(FALSE, FALSE, sizeof(struct scan_file)),
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), 0, 0 };
	scan_roots(&st);
	g_hash_table_destroy(st.dirs);
	return st.files;
}

static void *scan_thread(void *unused) {
	(void)unused;
	worker_background();
	struct result *r = g_new0(struct result, 1);
	r->kind = R_SCAN;
	r->files = scan_files();
	result_push(r);
	cache_trim();
	return NULL;
}

static void scan_start(void) {
	if (scan_running) {
		scan_again = true;
		return;
	}
	scan_running = true;
	pthread_t t;
	if (pthread_create(&t, NULL, scan_thread, NULL) == 0) {
		pthread_detach(t);
	} else {
		scan_running = false;
	}
}

/* ---- what the app knows: main thread ----------------------------------------- */

static char *display_name(const char *raw) {
	return g_utf8_make_valid(raw, -1);	/* a file name may be any bytes; Pango's text may not */
}

static int photo_cmp(const void *a, const void *b) {
	const struct photo *x = *(struct photo *const *)a, *y = *(struct photo *const *)b;
	if (x->mtime_ns != y->mtime_ns) return x->mtime_ns > y->mtime_ns ? -1 : 1;
	return strcmp(x->path, y->path);
}

static int album_cmp(const void *a, const void *b) {
	const struct album *x = *(struct album *const *)a, *y = *(struct album *const *)b;
	return photo_cmp(&x->items[0], &y->items[0]);
}

static void albums_free(void) {
	for (int i = 0; i < n_albums; i++) {
		g_free(albums[i]->dir);
		g_free(albums[i]->name);
		g_free(albums[i]->items);
		g_free(albums[i]);
	}
	g_free(albums);
	albums = NULL;
	n_albums = 0;
}

static char *album_name_for(const char *dir) {
	char *base = g_path_get_basename(dir);
	char *name;
	if (!strcmp(base, "ztCamera")) name = g_strdup("camera roll");
	else if (!strcasecmp(base, "Screenshots")) name = g_strdup("screenshots");
	else name = display_name(base);
	g_free(base);
	return name;
}

/* The albums, from `all` (which is in order), and where the open one has got to. */
static void albums_build(void) {
	albums_free();
	GHashTable *found = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	int room = 0;
	for (int i = 0; i < n_all; i++) {
		struct photo *p = all[i];
		char *dir = g_path_get_dirname(p->path);
		struct album *a = g_hash_table_lookup(found, dir);
		if (!a) {
			if (n_albums == room) {
				room = room ? room * 2 : 16;
				albums = g_renew(struct album *, albums, room);
			}
			a = g_new0(struct album, 1);
			a->dir = g_strdup(dir);
			a->name = album_name_for(dir);
			albums[n_albums++] = a;
			g_hash_table_insert(found, g_strdup(dir), a);
		}
		g_free(dir);
		if (a->n == a->room) {
			a->room = a->room ? a->room * 2 : 8;
			a->items = g_renew(struct photo *, a->items, a->room);
		}
		a->items[a->n++] = p;
		p->album = a;
	}
	g_hash_table_destroy(found);
	if (n_albums) qsort(albums, (size_t)n_albums, sizeof(albums[0]), album_cmp);
	album_open = NULL;
	for (int i = 0; album_open_dir && i < n_albums; i++) {
		if (!strcmp(albums[i]->dir, album_open_dir)) album_open = albums[i];
	}
}

static void slot_clear(struct slot *s) {
	if (s->photo) {
		s->photo->slot = -1;
		s->photo->state = T_NONE;
		s->photo = NULL;
	}
	if (s->cpu) cairo_surface_destroy(s->cpu);
	s->cpu = NULL;
	s->uploaded = false;
}

static void photo_free(struct photo *p) {
	if (p->slot >= 0) slot_clear(&slots[p->slot]);
	g_hash_table_remove(by_path, p->path);
	g_free(p->path);
	g_free(p);
}

/* every thumbnail not begun forgets it was asked for (they are asked for again) */
static void jobs_clear(void) {
	pthread_mutex_lock(&job_lock);
	for (int i = 0; i < n_thumb_jobs; i++) {
		thumb_jobs[i].photo->state = T_NONE;
		g_free(thumb_jobs[i].path);
	}
	n_thumb_jobs = 0;
	pthread_mutex_unlock(&job_lock);
}

static void want_forget(void);

/* A scan's result in place of the last one's, what is unchanged kept as it is. */
static void apply_scan(GArray *files) {
	zmc_list_lock(true);
	jobs_clear();
	want_forget();
	for (int i = 0; i < n_all; i++) all[i]->seen = false;
	struct photo **next = g_new(struct photo *, files->len + 1);
	int n = 0;
	for (guint i = 0; i < files->len; i++) {
		struct scan_file *f = &g_array_index(files, struct scan_file, i);
		struct photo *p = g_hash_table_lookup(by_path, f->path);
		if (p && p->seen) {	/* two roots with the same place in them */
			g_free(f->path);
			continue;
		}
		if (!p) {
			p = g_new0(struct photo, 1);
			p->path = f->path;
			const char *slash = strrchr(p->path, '/');
			p->name = slash ? slash + 1 : p->path;
			p->slot = -1;
			g_hash_table_insert(by_path, p->path, p);
		} else {
			if (p->mtime_ns != f->mtime_ns || p->size != f->size) {
				if (p->slot >= 0) slot_clear(&slots[p->slot]);
				p->state = T_NONE;
			}
			g_free(f->path);
		}
		p->mtime_ns = f->mtime_ns;
		p->size = f->size;
		p->seen = true;
		next[n++] = p;
	}
	for (int i = 0; i < n_all; i++) {
		if (!all[i]->seen) photo_free(all[i]);
	}
	g_free(all);
	all = next;
	n_all = n;
	if (n_all) qsort(all, (size_t)n_all, sizeof(all[0]), photo_cmp);
	albums_build();
	list_rev++;
	scanned = true;
	zmc_list_lock(false);
	g_array_free(files, TRUE);
	zmc_busy(false);
	if (zmc.page == PAGE_ALBUM && !album_open) zmc_go(PAGE_ALBUMS, -1);
	viewer_rebuilt();
	if (open_path) {
		for (int i = 0; i < n_all; i++) {
			if (!strcmp(all[i]->path, open_path)) {
				viewer_open(all, n_all, NULL, i);
				break;
			}
		}
		g_free(open_path);
		open_path = NULL;
	}
	zmc_redraw();
}

/* ---- thumbnails on the main thread -------------------------------------------- */

static struct slot *slot_take(void) {
	struct slot *best = NULL;
	for (int i = 0; i < THUMB_SLOTS; i++) {
		struct slot *s = &slots[i];
		if (!s->photo) return s;
		if (s->used + 1 >= frame_no) continue;	/* on the screen */
		if (!best || s->used < best->used) best = s;
	}
	return best;
}

static void thumb_arrived(char *path, cairo_surface_t *surface) {
	struct photo *p = g_hash_table_lookup(by_path, path);
	g_free(path);
	if (!p) {
		if (surface) cairo_surface_destroy(surface);
		return;
	}
	if (!surface) {
		p->state = T_BAD;
		return;
	}
	zmc_list_lock(true);
	struct slot *s = p->slot >= 0 ? &slots[p->slot] : slot_take();
	if (!s) {
		p->state = T_NONE;	/* asked for again if it is still wanted */
		zmc_list_lock(false);
		cairo_surface_destroy(surface);
		return;
	}
	if (s->photo != p) {
		slot_clear(s);
		s->photo = p;
		p->slot = (int)(s - slots);
	} else if (s->cpu) {
		cairo_surface_destroy(s->cpu);
	}
	s->cpu = surface;
	s->uploaded = false;
	s->arrive = zmc_now_ms();
	s->used = frame_no;
	p->state = T_READY;
	zmc_list_lock(false);
}

static struct photo *wanted[WANT_MAX], *wanted_last[WANT_MAX];
static int n_wanted, n_wanted_last;

static void want_forget(void) {
	n_wanted = n_wanted_last = 0;
}

static void want(struct photo *p) {
	if (!p || n_wanted >= WANT_MAX) return;
	wanted[n_wanted++] = p;
}

/* What the screen wants now, the first wanted the first made. */
static void want_commit(void) {
	bool same = n_wanted == n_wanted_last && !memcmp(wanted, wanted_last, sizeof(wanted[0]) * (size_t)n_wanted);
	bool lost = false;
	for (int i = 0; i < n_wanted && !lost; i++) lost = wanted[i]->state == T_NONE;
	if (same && !lost) return;
	memcpy(wanted_last, wanted, sizeof(wanted[0]) * (size_t)n_wanted);
	n_wanted_last = n_wanted;
	jobs_clear();
	workers_start();
	pthread_mutex_lock(&job_lock);
	for (int i = n_wanted - 1; i >= 0; i--) {
		struct photo *p = wanted[i];
		if (p->state != T_NONE) continue;
		p->state = T_QUEUED;
		thumb_jobs[n_thumb_jobs++] = (struct thumb_job){ g_strdup(p->path), p->mtime_ns, p->size, p };
	}
	pthread_cond_broadcast(&job_wake);
	pthread_mutex_unlock(&job_lock);
}

/* ---- one row of a list ---------------------------------------------------------
 * In a picture of the page (a pivot moving, a page turning) the thumbnails are
 * there. Row textures, which the thumbnails' quads then cover, are only squares:
 * nothing to decode or scale, however fast the list flies by. */

static void paint_cell(cairo_t *cr, struct photo *p, double x, double y, double size) {
	cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	if (!in_render || !p || p->slot < 0 || !slots[p->slot].cpu) return;
	if (zmc.style == ZMC_STYLE_DOTMATRIX) {
		zmc_picture(cr, slots[p->slot].cpu, x, y, size);
		return;
	}
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_rectangle(cr, 0, 0, size, size);
	cairo_clip(cr);
	cairo_scale(cr, size / THUMB_PX, size / THUMB_PX);
	cairo_set_source_surface(cr, slots[p->slot].cpu, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
	cairo_paint(cr);
	cairo_restore(cr);
}

static const struct gl_grid *grid_photos(void) {
	static struct gl_grid g;
	g = gl_grid_for(zmc.width, 4, ZMC_MARGIN, 8, 4, 4);
	return &g;
}

static const struct gl_grid *grid_albums(void) {
	static struct gl_grid g;
	g = gl_grid_for(zmc.width, 2, ZMC_MARGIN, 8, 8, 54);
	return &g;
}

static void row_of(const struct gl_grid *g, cairo_t *cr, struct photo **items, int count, int row) {
	for (int c = 0; c < g->cols; c++) {
		int i = row * g->cols + c;
		if (i >= count) break;
		paint_cell(cr, items[i], gl_grid_x(g, c), 0, g->cell);
	}
}

static void photos_row(cairo_t *cr, int row) {
	row_of(grid_photos(), cr, all, n_all, row);
}

static void album_row(cairo_t *cr, int row) {
	if (album_open) row_of(grid_photos(), cr, album_open->items, album_open->n, row);
}

static void albums_row(cairo_t *cr, int row) {
	const struct gl_grid *g = grid_albums();
	for (int c = 0; c < g->cols; c++) {
		int i = row * g->cols + c;
		if (i >= n_albums) break;
		double x = gl_grid_x(g, c);
		paint_cell(cr, albums[i]->items[0], x, 0, g->cell);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, albums[i]->name, FONT_SEMILIGHT, 17, x, g->cell + 4, g->cell);
		char count[32];
		snprintf(count, sizeof(count), "%d", albums[i]->n);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text_line(cr, count, FONT_REGULAR, 12, x, g->cell + 27, g->cell);
	}
}

static bool grid_hit(const struct gl_grid *g, int count, int action, int row, double x, struct zmc_hit *hit) {
	int c = gl_grid_col(g, x), i = row * g->cols + c;
	if (i >= count) return false;
	*hit = (struct zmc_hit){ gl_grid_x(g, c), 0, g->cell, g->cell, action, i, false, false };
	return true;
}

static bool photos_hit(int row, double x, double y, struct zmc_hit *hit) {
	return grid_hit(grid_photos(), n_all, ACT_PHOTO, row, x, hit);
}

static bool album_hit(int row, double x, double y, struct zmc_hit *hit) {
	return album_open && grid_hit(grid_photos(), album_open->n, ACT_PHOTO, row, x, hit);
}

static bool albums_hit(int row, double x, double y, struct zmc_hit *hit) {
	return grid_hit(grid_albums(), n_albums, ACT_ALBUM, row, x, hit);
}

/* ---- the pages ----------------------------------------------------------------- */

static double draw_note(cairo_t *cr, double y, const char *big, const char *small) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, big, FONT_SEMILIGHT, 25.5, ZMC_MARGIN, y + 8, zmc.width - 2 * ZMC_MARGIN);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	return y + 48 + zmc_text(cr, small, FONT_REGULAR, 14, ZMC_MARGIN, y + 48, zmc.width - 2 * ZMC_MARGIN);
}

static const char *const empty_where =
	"Photos you take with Camera, and any you put in Pictures or Screenshots, show up here.";

static double render(cairo_t *cr, int page) {
	in_render = true;
	double y;
	if (page == PAGE_ALBUM) {
		y = zmc_header(cr, album_open ? album_open->name : "album");
		list_top[page] = y;
		if (album_open) {
			const struct gl_grid *g = grid_photos();
			struct zmc_list list = { .count = gl_grid_rows(g, album_open->n), .row_h = g->row_h, .revision = list_rev,
				.draw_row = album_row, .row_hit = album_hit };
			y = zmc_list(cr, y, &list);
		}
	} else if (page == PAGE_ALBUMS) {
		y = zmc_pivot_next(cr, "albums", "photos", ZMC_ACT_PIVOT, PAGE_PHOTOS);
		list_top[page] = y;
		if (n_albums) {
			const struct gl_grid *g = grid_albums();
			struct zmc_list list = { .count = gl_grid_rows(g, n_albums), .row_h = g->row_h, .revision = list_rev,
				.draw_row = albums_row, .row_hit = albums_hit };
			y = zmc_list(cr, y, &list);
		} else if (scanned) {
			y = draw_note(cr, y, "no albums yet", "Each folder of photos is an album.");
		}
	} else {
		y = zmc_pivot_next(cr, "photos", "albums", ZMC_ACT_PIVOT, PAGE_ALBUMS);
		list_top[page] = y;
		if (n_all) {
			const struct gl_grid *g = grid_photos();
			struct zmc_list list = { .count = gl_grid_rows(g, n_all), .row_h = g->row_h, .revision = list_rev,
				.draw_row = photos_row, .row_hit = photos_hit };
			y = zmc_list(cr, y, &list);
		} else if (scanned) {
			y = draw_note(cr, y, "no photos yet", empty_where);
		}
	}
	in_render = false;
	return y + 24;
}

static cairo_surface_t *one_pixel(uint32_t argb) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	*(uint32_t *)cairo_image_surface_get_data(s) = argb;
	cairo_surface_mark_dirty(s);
	return s;
}

static void quad_flat(double c[4][3], double x, double y, double w, double h) {
	for (int i = 0; i < 4; i++) {
		c[i][0] = x + ((i & 1) ? w : 0);
		c[i][1] = y + ((i & 2) ? h : 0);
		c[i][2] = 0;
	}
}

/* a cross in a square: a photo that cannot be read */
static cairo_surface_t *broken_mark(void) {
	static cairo_surface_t *mark;
	if (mark) return mark;
	int side = 40 * zmc.scale;
	mark = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
	cairo_t *cr = cairo_create(mark);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	cairo_set_line_width(cr, 1.5);
	cairo_rectangle(cr, 8, 8, 24, 24);
	cairo_move_to(cr, 8, 8);
	cairo_line_to(cr, 32, 32);
	cairo_move_to(cr, 32, 8);
	cairo_line_to(cr, 8, 32);
	cairo_stroke(cr);
	cairo_destroy(cr);
	cairo_surface_flush(mark);
	return mark;
}

/* ---- the grid, on the GPU ------------------------------------------------------- */

static int uploads_left;
static bool more_frames;

static double press_now(uint32_t now) {
	if (zmc.press_start == 0) return zmc.press_to;
	double t = (double)(int32_t)(now - zmc.press_start) / (zmc.press_to > zmc.press_from ? ZMC_TILT_IN_MS : ZMC_TILT_OUT_MS);
	if (t >= 1) return zmc.press_to;
	if (t <= 0) return zmc.press_from;
	more_frames = true;
	return zmc.press_from + (zmc.press_to - zmc.press_from) * t;
}

/* A thumbnail over its square; `pressed` 0..1 leans it toward the finger as the
 * core leans a row: the square under it, drawn by the core, leans with it. */
static void draw_thumb(struct photo *p, double x, double y, double size, double pressed, uint32_t now) {
	double c[4][3];
	quad_flat(c, x, y, size, size);
	if (pressed > 0.001) {
		double sink = 1 - ZMC_PRESS_SINK * pressed, cx = x + size / 2, cy = y + size / 2;
		double lean = size / 2 * sin(ZMC_TILT_DEG * M_PI / 180) * pressed;
		double fx = 2 * zmc.press_fx - 1, fy = 2 * zmc.press_fy - 1;
		for (int i = 0; i < 4; i++) {
			double sx = (i & 1) ? 1 : -1, sy = (i & 2) ? 1 : -1;
			c[i][0] = cx + (c[i][0] - cx) * sink;
			c[i][1] = cy + (c[i][1] - cy) * sink;
			c[i][2] = -lean * (sx * fx + sy * fy) / 2;
		}
	}
	if (p->state == T_BAD) {
		zmc_gpu_quad(broken_mark(), 1, c, 1);
		return;
	}
	if (p->slot < 0) return;
	struct slot *s = &slots[p->slot];
	s->used = frame_no;
	if (!s->uploaded && s->cpu) {
		if (uploads_left <= 0) {
			more_frames = true;
			return;
		}
		if (!s->tex) s->tex = zmg_row_new(THUMB_PX, THUMB_PX);
		zmg_row_upload(s->tex, s->cpu);
		s->uploaded = true;
		uploads_left--;
	}
	if (!s->uploaded) return;
	double alpha = (double)(int32_t)(now - s->arrive) / THUMB_FADE_MS;
	if (alpha < 1) more_frames = true;
	else alpha = 1;
	struct zmg_vertex q[4];
	for (int i = 0; i < 4; i++) q[i] = (struct zmg_vertex){ (float)c[i][0], (float)c[i][1], (float)c[i][2] };
	zmg_row_draw(s->tex, 0, 0, 1, 1, q, (float)(alpha < 0 ? 0 : alpha));
}

/* One list of cells: `at(i)` gives its photo; those on the screen drawn, those on
 * and near it wanted. */
static void overlay_list(const struct gl_grid *g, int count, double top, struct photo *(*at)(int), int action, uint32_t now) {
	int rows = gl_grid_rows(g, count), first, last;
	double scroll = zmc.scroll;
	bool any = gl_rows_in(g, rows, top, ZMC_HEADER_H + scroll, zmc.height + scroll, &first, &last);
	if (!any) return;
	double press = zmc.pressed.action == action ? press_now(now) : 0;
	/* the rows on the screen, in order, then outward from it */
	for (int r = first; r <= last; r++) {
		for (int c = 0; c < g->cols; c++) {
			int i = r * g->cols + c;
			if (i >= count) break;
			struct photo *p = at(i);
			want(p);
			if (!viewer_covers_grid()) {
				draw_thumb(p, gl_grid_x(g, c), top + r * g->row_h - scroll, g->cell,
					zmc.pressed.arg == i ? press : 0, now);
			}
		}
	}
	for (int k = 1; k <= WANT_AHEAD_ROWS; k++) {
		for (int side = 0; side < 2; side++) {
			int r = side ? first - k : last + k;
			if (r < 0 || r >= rows) continue;
			for (int c = 0; c < g->cols; c++) {
				int i = r * g->cols + c;
				if (i < count) want(at(i));
			}
		}
	}
}

static struct photo *at_all(int i) { return all[i]; }
static struct photo *at_album(int i) { return album_open->items[i]; }
static struct photo *at_cover(int i) { return albums[i]->items[0]; }

/* Stopped, with nothing left to make: the pictures of the page behind a pivot's
 * swipe have what has arrived since they were drawn. */
static bool arrived_since_picture;

static void overlay(int page) {
	frame_no++;
	uint32_t now = zmc_now_ms();
	uploads_left = UPLOADS_PER_FRAME;
	more_frames = false;
	n_wanted = 0;
	if (page == PAGE_PHOTOS) overlay_list(grid_photos(), n_all, list_top[page], at_all, ACT_PHOTO, now);
	else if (page == PAGE_ALBUMS) overlay_list(grid_albums(), n_albums, list_top[page], at_cover, ACT_ALBUM, now);
	else if (album_open) overlay_list(grid_photos(), album_open->n, list_top[page], at_album, ACT_PHOTO, now);
	want_commit();
	if (viewer_is_open()) viewer_overlay();
	/* (inside a frame there is no zmc_present: this is what asks for the next) */
	if (more_frames) zmc.dirty = true;
}

/* ---- results, on the main thread --------------------------------------------------- */

static void results_ready(int fd, void *unused) {
	(void)unused;
	uint64_t count;
	if (read(fd, &count, sizeof(count)) < 0) { /* nothing to read: fine */ }
	struct result *r;
	bool any = false;
	while ((r = g_async_queue_try_pop(results))) {
		switch (r->kind) {
		case R_THUMB:
			thumb_arrived(r->path, r->surface);
			arrived_since_picture = true;
			any = true;
			break;
		case R_IMAGE:
			viewer_got_image(r->path, r->level, r->surface, r->orig_w, r->orig_h);
			g_free(r->path);
			any = true;
			break;
		case R_SCAN:
			scan_running = false;
			apply_scan(r->files);
			if (scan_again) {
				scan_again = false;
				scan_start();
			}
			break;
		}
		g_free(r);
	}
	if (any) zmc_present();
	/* nothing still being made: what the page's pictures show can be brought up
	 * to date (the core does it when idle) */
	if (arrived_since_picture && !n_thumb_jobs) {
		arrived_since_picture = false;
		zmc_list_lock(true);
		zmc_pivot_changed();
		zmc_list_lock(false);
	}
}

/* the folders changed; once they settle, look again */
static void rescan_tick(void) {
	zmc_every(0, NULL);
	scan_start();
}

static void inotify_ready(int fd, void *unused) {
	(void)unused;
	char events[4096];
	if (read(fd, events, sizeof(events)) > 0) zmc_every(600, rescan_tick);
}

/* ---- the viewer ----------------------------------------------------------------------
 * Not a page of its own: it comes up over the grid, from the square that was tapped,
 * and goes back into it. It owns every finger and the whole screen while it is up. */

struct band {
	unsigned tex;
	int first, rows;	/* the rows in its texture: one more each side, or the seams would show */
	int y0, y1;		/* the rows it draws */
};

struct vimg {
	char *path;
	bool loading, failed, ready;
	int w, h, orig_w, orig_h;
	cairo_surface_t *cpu;	/* until the GPU has it */
	struct band *bands;
	int n_bands, n_uploaded;
};

enum view_state { V_CLOSED, V_OPENING, V_OPEN, V_CLOSING };

static struct {
	enum view_state state;
	uint32_t t0;
	struct photo **list;
	int n, index;
	char *album_dir;
	char *current;		/* the path of the photo in front */
	struct vimg near[3];	/* it and the two beside it, in no order */
	struct vimg hires;	/* it, for zooming in */
	/* where it is */
	struct zv st;
	double slide;		/* points across, while swiping */
	/* moving on its own */
	bool zooming;
	struct zv z_from, z_to;
	uint32_t z_t0;
	bool sliding;
	double slide_from, slide_to;
	int slide_dir;
	uint32_t slide_t0;
	double vx, vy;		/* a pan let go */
	uint32_t pan_t;
	bool panning;
	uint32_t tap_at;	/* a tap, waiting to see if it was half of a double one */
	double tap_x, tap_y;
	uint32_t tap_time;
	/* fingers */
	struct { int id; double x, y; } f[2];
	int n_f;
	bool pinching, moved, across;
	double d0, m0x, m0y;
	struct zv from;
	double down_x, down_y;
	uint32_t down_time, last_time;
	double last_x, last_y, vel_x, vel_y;
	/* its furniture */
	bool chrome;
	bool confirm;
	bool notice;		/* failed_note is up, until the next tap */
	double slide_base;
	cairo_surface_t *top, *bar, *dialog, *failed_note;
	unsigned top_rev, bar_rev, dialog_rev, note_rev;
	bool top_stale, dialog_stale;
	double dlg_del_x, dlg_cancel_x, dlg_btn_y, dlg_btn_w, dlg_btn_h;
	cairo_surface_t *black;
} v;

static bool viewer_is_open(void) { return v.state != V_CLOSED; }
static bool viewer_covers_grid(void) { return v.state == V_OPEN; }

static void vimg_free(struct vimg *m) {
	g_free(m->path);
	if (m->cpu) cairo_surface_destroy(m->cpu);
	for (int i = 0; i < m->n_bands; i++) zmg_row_free(m->bands[i].tex);
	g_free(m->bands);
	memset(m, 0, sizeof(*m));
}

/* Its pixels, ready to go up band by band. */
static void vimg_take(struct vimg *m, cairo_surface_t *s, int ow, int oh) {
	m->loading = false;
	if (!s) {
		m->failed = true;
		return;
	}
	m->cpu = s;
	m->w = cairo_image_surface_get_width(s);
	m->h = cairo_image_surface_get_height(s);
	m->orig_w = ow;
	m->orig_h = oh;
	m->n_bands = (m->h + BAND_ROWS - 1) / BAND_ROWS;
	m->bands = g_new0(struct band, m->n_bands);
	for (int i = 0; i < m->n_bands; i++) {
		struct band *b = &m->bands[i];
		b->y0 = i * BAND_ROWS;
		b->y1 = b->y0 + BAND_ROWS < m->h ? b->y0 + BAND_ROWS : m->h;
		b->first = b->y0 > 0 ? b->y0 - 1 : 0;
		b->rows = (b->y1 < m->h ? b->y1 + 1 : m->h) - b->first;
	}
	m->n_uploaded = 0;
	m->ready = false;
}

/* What fits in this frame's share; true when more is waiting. */
static bool vimg_upload(struct vimg *m, int *bytes) {
	while (m->cpu && m->n_uploaded < m->n_bands) {
		if (*bytes <= 0) return true;
		struct band *b = &m->bands[m->n_uploaded];
		int stride = cairo_image_surface_get_stride(m->cpu);
		cairo_surface_t *part = cairo_image_surface_create_for_data(
			cairo_image_surface_get_data(m->cpu) + (size_t)b->first * stride,
			CAIRO_FORMAT_RGB24, m->w, b->rows, stride);
		b->tex = zmg_row_new(m->w, b->rows);
		zmg_row_upload(b->tex, part);
		cairo_surface_destroy(part);
		*bytes -= stride * b->rows;
		m->n_uploaded++;
	}
	if (m->cpu && m->n_uploaded == m->n_bands) {
		cairo_surface_destroy(m->cpu);
		m->cpu = NULL;
		m->ready = true;
	}
	return false;
}

static void vimg_draw(const struct vimg *m, double x, double y, double w, double h, double alpha) {
	if (!m->ready) return;
	for (int i = 0; i < m->n_bands; i++) {
		const struct band *b = &m->bands[i];
		double sy0 = y + h * b->y0 / m->h, sy1 = y + h * b->y1 / m->h;
		if (sy1 < 0 || sy0 > zmc.height) continue;
		struct zmg_vertex q[4] = {
			{ (float)x, (float)sy0, 0 }, { (float)(x + w), (float)sy0, 0 },
			{ (float)x, (float)sy1, 0 }, { (float)(x + w), (float)sy1, 0 },
		};
		zmg_row_draw(b->tex, 0, (double)(b->y0 - b->first) / b->rows, 1, (double)(b->y1 - b->first) / b->rows, q, (float)alpha);
	}
}

static struct vimg *vimg_of(const char *path) {
	for (int i = 0; i < 3; i++) {
		if (v.near[i].path && !strcmp(v.near[i].path, path)) return &v.near[i];
	}
	return NULL;
}

static const char *list_path(int i) {
	return i >= 0 && i < v.n ? v.list[i]->path : NULL;
}

/* What should be held now: this photo and those beside it. What the worker has
 * not started is replaced, the nearest first. */
static void viewer_sync(void) {
	const char *keep[3] = { list_path(v.index), list_path(v.index + 1), list_path(v.index - 1) };
	for (int i = 0; i < 3; i++) {
		struct vimg *m = &v.near[i];
		if (!m->path) continue;
		bool kept = false;
		for (int k = 0; k < 3; k++) kept = kept || (keep[k] && !strcmp(keep[k], m->path));
		if (!kept) vimg_free(m);
	}
	for (int k = 0; k < 3; k++) {
		if (!keep[k] || vimg_of(keep[k])) continue;
		for (int i = 0; i < 3; i++) {
			if (!v.near[i].path) {
				v.near[i].path = g_strdup(keep[k]);
				break;
			}
		}
	}
	if (v.hires.path && (!keep[0] || strcmp(v.hires.path, keep[0]))) vimg_free(&v.hires);
	workers_start();
	pthread_mutex_lock(&job_lock);
	for (int i = 0; i < n_image_jobs; i++) {
		g_free(image_jobs[i].path);
	}
	n_image_jobs = 0;
	for (int k = 0; k < 3; k++) {
		if (!keep[k]) continue;
		struct vimg *m = vimg_of(keep[k]);
		if (!m || m->ready || m->cpu || m->failed) continue;
		m->loading = true;
		if (image_busy && !image_busy_level && !strcmp(image_busy, keep[k])) continue;
		image_jobs[n_image_jobs++] = (struct image_job){ g_strdup(keep[k]), 0, zmc.width * zmc.scale, zmc.height * zmc.scale };
	}
	if (v.hires.path && v.hires.loading && !v.hires.cpu && !v.hires.ready &&
			!(image_busy && image_busy_level && !strcmp(image_busy, v.hires.path))) {
		image_jobs[n_image_jobs++] = (struct image_job){ g_strdup(v.hires.path), 1, 0, 0 };
	}
	pthread_cond_signal(&image_wake);
	pthread_mutex_unlock(&job_lock);
}

static void viewer_busy(void);

static void viewer_got_image(const char *path, int level, cairo_surface_t *s, int ow, int oh) {
	struct vimg *m = level ? (v.hires.path && !strcmp(v.hires.path, path) ? &v.hires : NULL) : vimg_of(path);
	if (v.state == V_CLOSED || !m || m->ready || m->cpu || (m->failed && !level)) {
		if (s) cairo_surface_destroy(s);
		return;
	}
	vimg_take(m, s, ow, oh);
	viewer_busy();
}

/* a photo worth more of its pixels than it has on the screen, and zoomed in */
static void viewer_want_hires(void) {
	struct vimg *m = v.current ? vimg_of(v.current) : NULL;
	if (!m || !m->w || m->failed) return;
	if (v.st.zoom < HIRES_ABOVE_ZOOM) {
		if (v.hires.path) {
			vimg_free(&v.hires);
			viewer_sync();
		}
		return;
	}
	if (v.hires.path) return;
	if (m->orig_w <= m->w * 5 / 4 && m->orig_h <= m->h * 5 / 4) return;	/* it has them all */
	v.hires.path = g_strdup(v.current);
	v.hires.loading = true;
	viewer_sync();
}

/* The photo's upright shape, from whichever of the two pictures there is. */
static void viewer_fit_of(const char *path, double *fw, double *fh) {
	struct vimg *m = path ? vimg_of(path) : NULL;
	if (m && m->w > 0) zv_fit(zmc.width, zmc.height, m->w, m->h, fw, fh);
	else zv_fit(zmc.width, zmc.height, 1, 1, fw, fh);
}

static double viewer_max_zoom(void) {
	struct vimg *m = v.current ? vimg_of(v.current) : NULL;
	double fw, fh;
	viewer_fit_of(v.current, &fw, &fh);
	return zv_max_zoom(fw, zmc.scale, m && m->orig_w > 0 ? m->orig_w : fw * zmc.scale);
}

/* ---- the viewer's furniture: Cairo, once, and only moved ------------------------- */

static void surface_clear(cairo_t *cr) {
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void top_paint(void) {
	if (!v.top) v.top = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * zmc.scale, TOP_H * zmc.scale);
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(v.top);
	surface_clear(cr);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.62);
	cairo_rectangle(cr, 0, 0, zmc.width, TOP_H);
	cairo_fill(cr);
	char where[64];
	snprintf(where, sizeof(where), "%d OF %d", v.index + 1, v.n);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, where, FONT_SEMIBOLD, 11.5, ZMC_MARGIN, 10, zmc.width - 2 * ZMC_MARGIN);
	if (v.index >= 0 && v.index < v.n) {
		struct photo *p = v.list[v.index];
		char *name = display_name(p->name);
		zmc_text_line(cr, name, FONT_REGULAR, 14, ZMC_MARGIN, 28, zmc.width - 2 * ZMC_MARGIN);
		g_free(name);
	}
	cairo_destroy(cr);
	cairo_surface_flush(v.top);
	zmc_list_lock(false);
	v.top_rev++;
	v.top_stale = false;
}

static void bar_paint(void) {
	if (v.bar) return;
	v.bar = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * zmc.scale, BAR_H * zmc.scale);
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(v.bar);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, 0, 0, zmc.width, BAR_H);
	cairo_fill(cr);
	int hits = zmc.n_hits;	/* the viewer answers its own taps */
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, zmc.width / 2.0, 7, "delete", "delete", 0, 0);
	zmc.n_hits = hits;
	cairo_destroy(cr);
	cairo_surface_flush(v.bar);
	zmc_list_lock(false);
	v.bar_rev = 1;
}

/* Windows Phone's message box: a dark panel from the top, two buttons. */
static void dialog_paint(void) {
	if (!v.dialog) v.dialog = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * zmc.scale, 240 * zmc.scale);
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(v.dialog);
	surface_clear(cr);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, 0, 0, zmc.width, 240);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "delete this photo?", FONT_SEMILIGHT, 25.5, ZMC_MARGIN, 28, zmc.width - 2 * ZMC_MARGIN);
	cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
	const char *shown = "";
	char *name = NULL;
	if (v.index >= 0 && v.index < v.n) shown = name = display_name(v.list[v.index]->name);
	double name_h = zmc_text(cr, shown, FONT_REGULAR, 14, ZMC_MARGIN, 72, zmc.width - 2 * ZMC_MARGIN);
	g_free(name);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "It will be gone from the phone. This can't be undone.", FONT_REGULAR, 13, ZMC_MARGIN, 72 + fmin(name_h, 44) + 8, zmc.width - 2 * ZMC_MARGIN);
	int hits = zmc.n_hits;
	v.dlg_btn_w = (zmc.width - 2 * ZMC_MARGIN - 12) / 2.0;
	v.dlg_btn_y = 170;
	v.dlg_btn_h = 48;
	v.dlg_del_x = ZMC_MARGIN;
	v.dlg_cancel_x = ZMC_MARGIN + v.dlg_btn_w + 12;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_button(cr, v.dlg_del_x, v.dlg_btn_y, v.dlg_btn_w, "delete", 0, 0);
	zmc_button(cr, v.dlg_cancel_x, v.dlg_btn_y, v.dlg_btn_w, "cancel", 0, 0);
	zmc.n_hits = hits;
	cairo_destroy(cr);
	cairo_surface_flush(v.dialog);
	zmc_list_lock(false);
	v.dialog_rev++;
	v.dialog_stale = false;
}

/* A line in the middle of the screen, in place of a photo. */
static void note_paint(const char *big, const char *small) {
	if (!v.failed_note) v.failed_note = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * zmc.scale, 120 * zmc.scale);
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(v.failed_note);
	surface_clear(cr);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, big, FONT_SEMILIGHT, 21, ZMC_MARGIN, 10, zmc.width - 2 * ZMC_MARGIN);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, small, FONT_REGULAR, 13, ZMC_MARGIN, 44, zmc.width - 2 * ZMC_MARGIN);
	cairo_destroy(cr);
	cairo_surface_flush(v.failed_note);
	zmc_list_lock(false);
	v.note_rev++;
}

/* ---- the viewer on the GPU --------------------------------------------------------- */

static void draw_box(cairo_surface_t *s, unsigned rev, double x, double y, double w, double h, double alpha) {
	double c[4][3];
	quad_flat(c, x, y, w, h);
	zmc_gpu_quad(s, rev, c, alpha);
}

/* Where the tapped square is on the screen, if it is. */
static bool viewer_cell_rect(double *x, double *y, double *size) {
	if (v.index < 0 || v.index >= v.n) return false;
	const struct gl_grid *g;
	int i = v.index;
	if (zmc.page == PAGE_ALBUMS) return false;
	g = grid_photos();
	double top = list_top[zmc.page];
	int row = i / g->cols, col = i % g->cols;
	*y = top + row * g->row_h - zmc.scroll;
	*x = gl_grid_x(g, col);
	*size = g->cell;
	return *y + g->cell > ZMC_HEADER_H && *y < zmc.height;
}

static const struct vimg *best_of_current(void) {
	if (v.hires.ready && v.st.zoom >= HIRES_ABOVE_ZOOM) return &v.hires;
	return v.current ? vimg_of(v.current) : NULL;
}

/* the photo beside this one, or this one, at zoom 1, shifted across */
static void draw_neighbour(const char *path, double dx) {
	if (!path) return;
	struct vimg *m = vimg_of(path);
	if (!m || !m->ready) return;
	double fw, fh;
	zv_fit(zmc.width, zmc.height, m->w, m->h, &fw, &fh);
	vimg_draw(m, (zmc.width - fw) / 2 + dx, (zmc.height - fh) / 2, fw, fh, 1);
}

static void viewer_overlay(void) {
	uint32_t now = zmc_now_ms();
	double W = zmc.width, H = zmc.height;
	if (!v.black) v.black = one_pixel(0xff000000);
	/* an open anim's progress */
	double t = 1;
	if (v.state == V_OPENING) t = gl_ease_out((double)(int32_t)(now - v.t0) / OPEN_MS);
	else if (v.state == V_CLOSING) t = 1 - gl_ease_out((double)(int32_t)(now - v.t0) / OPEN_MS);
	draw_box(v.black, 1, 0, 0, W, H, fmin(1, t * 1.4));

	/* uploads: the photo in front, then beside it; then its zoomed one */
	int bytes = UPLOAD_BYTES_PER_FRAME;
	const char *order[3] = { list_path(v.index), list_path(v.index + 1), list_path(v.index - 1) };
	for (int k = 0; k < 3; k++) {
		struct vimg *m = order[k] ? vimg_of(order[k]) : NULL;
		if (m && vimg_upload(m, &bytes)) more_frames = true;
	}
	if (v.hires.path && vimg_upload(&v.hires, &bytes)) more_frames = true;

	const struct vimg *m = best_of_current();
	struct photo *cur = v.index >= 0 && v.index < v.n ? v.list[v.index] : NULL;
	double fw, fh;
	viewer_fit_of(v.current, &fw, &fh);
	double w = fw * v.st.zoom, h = fh * v.st.zoom;
	double x = (W - w) / 2 + v.st.x + v.slide, y = (H - h) / 2 + v.st.y;
	if (t < 1) {
		double cx, cy, size;
		if (viewer_cell_rect(&cx, &cy, &size)) {
			/* from the square: photo's shape inside the cell's, covering it */
			double k = fmax(size / fw, size / fh);
			double sw = fw * k, sh = fh * k;
			double sx = cx + (size - sw) / 2, sy = cy + (size - sh) / 2;
			x = sx + (x - sx) * t;
			y = sy + (y - sy) * t;
			w = sw + (w - sw) * t;
			h = sh + (h - sh) * t;
		}
	}
	double alpha = v.state == V_CLOSING ? fmin(1, t * 2) : 1;
	if (m && m->ready) {
		vimg_draw(m, x, y, w, h, alpha);
	} else if (cur && cur->slot >= 0 && slots[cur->slot].uploaded) {
		/* until the photo comes: its thumbnail, as a square in its middle */
		double side = fmin(W, H);
		if (t < 1) side = fmin(w, h);
		struct zmg_vertex q[4] = {
			{ (float)((W - side) / 2 + v.slide), (float)((H - side) / 2), 0 }, { (float)((W + side) / 2 + v.slide), (float)((H - side) / 2), 0 },
			{ (float)((W - side) / 2 + v.slide), (float)((H + side) / 2), 0 }, { (float)((W + side) / 2 + v.slide), (float)((H + side) / 2), 0 },
		};
		zmg_row_draw(slots[cur->slot].tex, 0, 0, 1, 1, q, (float)alpha);
	}
	struct vimg *held = v.current ? vimg_of(v.current) : NULL;
	if (held && held->failed && t >= 1) {
		if (!v.failed_note) note_paint("this photo can't be opened", "The file may be damaged, or of a kind the phone can't read.");
		draw_box(v.failed_note, v.note_rev, v.slide, (H - 120) / 2, W, 120, 1);
	}
	if (v.notice && v.failed_note) draw_box(v.failed_note, v.note_rev, 0, (H - 120) / 2, W, 120, 1);

	/* those beside it, while it moves */
	if (t >= 1 && v.slide != 0) {
		draw_neighbour(list_path(v.index + 1), v.slide + W + PHOTO_GAP);
		draw_neighbour(list_path(v.index - 1), v.slide - W - PHOTO_GAP);
	}

	if (t >= 1) {
		if (v.chrome && !v.confirm) {
			if (v.top_stale || !v.top) top_paint();
			bar_paint();
			draw_box(v.top, v.top_rev, 0, 0, W, TOP_H, 1);
			if (!pick_mode) draw_box(v.bar, v.bar_rev, 0, H - BAR_H, W, BAR_H, 1);
		}
		if (v.confirm) {
			if (v.dialog_stale || !v.dialog) dialog_paint();
			draw_box(v.black, 1, 0, 0, W, H, 0.55);
			draw_box(v.dialog, v.dialog_rev, 0, 0, W, 240, 1);
		}
	}

}

/* the core's loading dots, while the photo in front is on its way */
static void viewer_busy(void) {
	struct vimg *held = v.state != V_CLOSED && v.current ? vimg_of(v.current) : NULL;
	bool wanting = held && !held->ready && !held->cpu && !held->failed;
	if (zmc.busy != wanting && scanned) zmc_busy(wanting);
}

/* ---- moving on its own ------------------------------------------------------------ */

static void viewer_finished_slide(void);

static void viewer_step(void) {
	uint32_t now = zmc_now_ms();
	bool more = false;
	if (v.state == V_OPENING) {
		if ((int32_t)(now - v.t0) >= OPEN_MS) v.state = V_OPEN;
		else more = true;
	} else if (v.state == V_CLOSING) {
		if ((int32_t)(now - v.t0) >= OPEN_MS) {
			v.state = V_CLOSED;
			for (int i = 0; i < 3; i++) vimg_free(&v.near[i]);
			vimg_free(&v.hires);
			pthread_mutex_lock(&job_lock);
			for (int i = 0; i < n_image_jobs; i++) g_free(image_jobs[i].path);
			n_image_jobs = 0;
			pthread_mutex_unlock(&job_lock);
			gallery_app.overlay_above_header = false;	/* the title goes back on top */
			viewer_busy();
			zmc_on_frame(NULL);
			zmc_present();
			return;
		}
		more = true;
	}
	if (v.sliding) {
		double p = gl_ease_out((double)(int32_t)(now - v.slide_t0) / SLIDE_MS);
		v.slide = v.slide_from + (v.slide_to - v.slide_from) * p;
		if ((int32_t)(now - v.slide_t0) >= SLIDE_MS) {
			v.sliding = false;
			viewer_finished_slide();
		} else {
			more = true;
		}
	}
	if (v.zooming) {
		double p = gl_ease_out((double)(int32_t)(now - v.z_t0) / ZOOM_MS);
		/* the zoom by its ratio, or going in looks like it hurries */
		v.st.zoom = v.z_from.zoom * pow(v.z_to.zoom / v.z_from.zoom, p);
		v.st.x = v.z_from.x + (v.z_to.x - v.z_from.x) * p;
		v.st.y = v.z_from.y + (v.z_to.y - v.z_from.y) * p;
		if ((int32_t)(now - v.z_t0) >= ZOOM_MS) {
			v.zooming = false;
			v.st = v.z_to;
			viewer_want_hires();
		} else {
			more = true;
		}
	}
	if (v.panning) {
		double dt = (double)(int32_t)(now - v.pan_t);
		v.pan_t = now;
		double fw, fh;
		viewer_fit_of(v.current, &fw, &fh);
		v.st.x += v.vx * dt;
		v.st.y += v.vy * dt;
		struct zv c = zv_clamped(v.st, zmc.width, zmc.height, fw, fh, viewer_max_zoom());
		if (c.x != v.st.x) v.vx = 0;
		if (c.y != v.st.y) v.vy = 0;
		v.st = c;
		double decay = exp(-dt / 280);
		v.vx *= decay;
		v.vy *= decay;
		if (fabs(v.vx) + fabs(v.vy) < 0.02) v.panning = false;
		else more = true;
	}
	if (v.tap_at) {
		if ((int32_t)(now - v.tap_at) >= 0) {
			v.tap_at = 0;
			v.chrome = !v.chrome;
		} else {
			more = true;
		}
	}
	if (!more) zmc_on_frame(NULL);
}

static void viewer_animate(void) {
	zmc_on_frame(viewer_step);
}

static void viewer_show_index(int index) {
	v.index = index;
	g_free(v.current);
	v.current = v.index >= 0 && v.index < v.n ? g_strdup(v.list[v.index]->path) : NULL;
	v.st = (struct zv){ 1, 0, 0 };
	v.slide = 0;
	v.top_stale = true;
	v.zooming = v.panning = false;
	if (v.failed_note) {
		cairo_surface_destroy(v.failed_note);
		v.failed_note = NULL;
	}
	viewer_sync();
	viewer_busy();
}

static void viewer_finished_slide(void) {
	int dir = v.slide_dir;
	v.slide_dir = 0;
	if (dir) viewer_show_index(v.index + dir);
	else v.slide = 0;
}

static void slide_to(double target, int dir) {
	v.sliding = true;
	v.slide_from = v.slide;
	v.slide_to = target;
	v.slide_dir = dir;
	v.slide_t0 = zmc_now_ms();
	viewer_animate();
}

static void zoom_to(struct zv target) {
	v.zooming = true;
	v.panning = false;
	v.z_from = v.st;
	v.z_to = target;
	v.z_t0 = zmc_now_ms();
	viewer_animate();
}

static void viewer_open(struct photo **list, int n, const char *album_dir, int index) {
	if (!zmg_ready() || index < 0 || index >= n) return;
	v.list = list;
	v.n = n;
	g_free(v.album_dir);
	v.album_dir = g_strdup(album_dir);
	v.state = V_OPENING;
	v.t0 = zmc_now_ms();
	v.chrome = true;
	v.confirm = false;
	v.tap_at = 0;
	v.tap_time = 0;
	v.n_f = 0;
	gallery_app.overlay_above_header = true;
	viewer_show_index(index);
	viewer_animate();
}

/* Back into the square, if it is on the screen; the list comes to it first if not. */
static void viewer_close(void) {
	if (v.state == V_CLOSED || v.state == V_CLOSING) return;
	v.state = V_CLOSING;
	v.t0 = zmc_now_ms();
	v.confirm = false;
	v.st = (struct zv){ 1, 0, 0 };
	v.slide = 0;
	v.sliding = v.zooming = v.panning = false;
	if (zmc.page != PAGE_ALBUMS) {
		const struct gl_grid *g = grid_photos();
		double top = list_top[zmc.page], y = top + v.index / g->cols * g->row_h - zmc.scroll;
		if (y < ZMC_HEADER_H || y + g->cell > zmc.height) zmc_list_show(zmc.page, v.index / g->cols);
	}
	viewer_animate();
}

/* The photos changed under it (a scan, or the delete just done). */
static void viewer_rebuilt(void) {
	if (v.state == V_CLOSED) return;
	if (v.album_dir) {
		struct album *a = NULL;
		for (int i = 0; i < n_albums; i++) {
			if (!strcmp(albums[i]->dir, v.album_dir)) a = albums[i];
		}
		v.list = a ? a->items : NULL;
		v.n = a ? a->n : 0;
	} else {
		v.list = all;
		v.n = n_all;
	}
	if (!v.n) {
		v.index = -1;
		viewer_close();
		return;
	}
	int at = -1;
	for (int i = 0; v.current && i < v.n; i++) {
		if (!strcmp(v.list[i]->path, v.current)) at = i;
	}
	if (at >= 0) {
		v.index = at;
		v.top_stale = true;
		viewer_sync();
	} else {
		viewer_show_index(v.index < v.n ? v.index : v.n - 1);
	}
}

/* ---- deleting -----------------------------------------------------------------------
 * The file itself, by its path, with no shell anywhere near it. */

static void viewer_delete_current(void) {
	if (v.index < 0 || v.index >= v.n) return;
	struct photo *p = v.list[v.index];
	v.confirm = false;
	if (unlink(p->path) != 0) {
		int err = errno;
		char why[160];
		snprintf(why, sizeof(why), "%s.", g_strerror(err));
		note_paint("this photo couldn't be deleted", why);
		v.notice = true;
		zmc_present();
		return;
	}
	thumb_forget(p->path, p->mtime_ns, p->size);
	zmc_haptic("click");
	/* out of the lists now; a scan will find the same when the folder's watch
	 * fires, and change nothing */
	zmc_list_lock(true);
	jobs_clear();
	want_forget();
	int at = -1;
	for (int i = 0; i < n_all; i++) {
		if (all[i] == p) at = i;
	}
	if (at >= 0) {
		memmove(all + at, all + at + 1, sizeof(all[0]) * (size_t)(n_all - at - 1));
		n_all--;
	}
	photo_free(p);
	albums_build();
	list_rev++;
	zmc_list_lock(false);
	if (zmc.page == PAGE_ALBUM && !album_open) zmc_go(PAGE_ALBUMS, -1);
	viewer_rebuilt();
	zmc_redraw();
}

/* ---- fingers in the viewer ------------------------------------------------------------ */

static int finger_of(int id) {
	for (int i = 0; i < v.n_f; i++) {
		if (v.f[i].id == id) return i;
	}
	return -1;
}

static void pinch_begin(void) {
	double dx = v.f[1].x - v.f[0].x, dy = v.f[1].y - v.f[0].y;
	v.pinching = true;
	v.d0 = hypot(dx, dy);
	v.m0x = (v.f[0].x + v.f[1].x) / 2 - zmc.width / 2.0;
	v.m0y = (v.f[0].y + v.f[1].y) / 2 - zmc.height / 2.0;
	v.from = v.st;
}

static void drag_begin(double x, double y, uint32_t time) {
	v.down_x = x;
	v.down_y = y;
	v.last_x = x;
	v.last_y = y;
	v.last_time = time;
	v.vel_x = v.vel_y = 0;
	v.from = v.st;
	v.moved = false;
	v.across = false;
}

/* when zoom, pan or slide went past where they may rest: back to it */
static void settle(void) {
	double fw, fh;
	viewer_fit_of(v.current, &fw, &fh);
	struct zv c = zv_clamped(v.st, zmc.width, zmc.height, fw, fh, viewer_max_zoom());
	if (c.zoom != v.st.zoom || c.x != v.st.x || c.y != v.st.y) zoom_to(c);
	else viewer_want_hires();
}

static void viewer_tap(double x, double y, uint32_t time) {
	if (v.notice) {
		v.notice = false;
		zmc_present();
		return;
	}
	if (v.confirm) {
		if (y >= v.dlg_btn_y && y < v.dlg_btn_y + v.dlg_btn_h) {
			if (x >= v.dlg_del_x && x < v.dlg_del_x + v.dlg_btn_w) {
				viewer_delete_current();
				zmc_present();
				return;
			}
			if (x >= v.dlg_cancel_x && x < v.dlg_cancel_x + v.dlg_btn_w) {
				v.confirm = false;
				zmc_haptic("tap");
				zmc_present();
				return;
			}
		}
		if (y >= 240) {	/* anywhere else: as back */
			v.confirm = false;
			zmc_present();
		}
		return;
	}
	if (v.chrome && !pick_mode && y >= zmc.height - BAR_H && fabs(x - zmc.width / 2.0) < 60) {
		v.confirm = true;
		v.dialog_stale = true;
		zmc_haptic("tap");
		zmc_present();
		return;
	}
	if (v.tap_time && (uint32_t)(time - v.tap_time) < DOUBLE_TAP_MS && hypot(x - v.tap_x, y - v.tap_y) < 40) {
		/* the second of two: in on that point, or back out */
		v.tap_time = 0;
		v.tap_at = 0;
		double fw, fh;
		viewer_fit_of(v.current, &fw, &fh);
		double zoom_in = fmax(DOUBLE_TAP_ZOOM, fmin(zmc.width / fw, zmc.height / fh));
		struct zv target = zv_double_tap(v.st, x - zmc.width / 2.0, y - zmc.height / 2.0,
			zmc.width, zmc.height, fw, fh, zoom_in, viewer_max_zoom());
		if (target.zoom > 1) v.chrome = false;
		zoom_to(target);
		return;
	}
	v.tap_time = time;
	v.tap_x = x;
	v.tap_y = y;
	v.tap_at = zmc_now_ms() + DOUBLE_TAP_MS;
	viewer_animate();
}

static bool app_touch(int phase, int id, double x, double y, uint32_t time) {
	if (v.state == V_CLOSED) {
		v.n_f = 0;
		return false;
	}
	if (v.state != V_OPEN && phase == 0) {
		/* a finger does not wait for the photo to finish arriving */
		if (v.state == V_CLOSING) return true;
		v.state = V_OPEN;
	}
	if (phase == 0) {
		if (v.n_f >= 2) return true;
		v.f[v.n_f].id = id;
		v.f[v.n_f].x = x;
		v.f[v.n_f].y = y;
		v.n_f++;
		if (v.sliding) {	/* a finger takes what is moving where it has got to */
			v.sliding = false;
			if (v.slide_dir) {
				double left = v.slide - v.slide_to;
				int dir = v.slide_dir;
				v.slide_dir = 0;
				viewer_show_index(v.index + dir);
				v.slide = left;
			}
		}
		v.panning = false;
		v.zooming = false;
		v.slide_base = v.slide;
		if (v.n_f == 1) drag_begin(x, y, time);
		else if (!v.confirm) pinch_begin();
		return true;
	}
	int at = finger_of(id);
	if (phase == 1 && at >= 0) {
		v.f[at].x = x;
		v.f[at].y = y;
		if (v.confirm) return true;
		if (v.pinching && v.n_f == 2) {
			double d = hypot(v.f[1].x - v.f[0].x, v.f[1].y - v.f[0].y);
			double mx = (v.f[0].x + v.f[1].x) / 2 - zmc.width / 2.0, my = (v.f[0].y + v.f[1].y) / 2 - zmc.height / 2.0;
			v.st = zv_pinch(v.from, v.d0, d, v.m0x, v.m0y, mx, my, viewer_max_zoom());
			v.slide = 0;
			v.chrome = false;
			v.tap_at = 0;
			zmc_present();
			return true;
		}
		if (v.n_f != 1) return true;
		double dx = x - v.down_x, dy = y - v.down_y;
		if (!v.moved && hypot(dx, dy) > SLOP_PT) {
			v.moved = true;
			v.across = fabs(dx) > fabs(dy);
			v.tap_at = 0;
			v.tap_time = 0;
			/* starting from here: nothing jumps as it begins */
			v.down_x = x;
			v.down_y = y;
			dx = dy = 0;
		}
		if (time != v.last_time) {
			double dt = (double)(time - v.last_time);
			v.vel_x = v.vel_x * 0.4 + (x - v.last_x) / dt * 0.6;
			v.vel_y = v.vel_y * 0.4 + (y - v.last_y) / dt * 0.6;
			v.last_x = x;
			v.last_y = y;
			v.last_time = time;
		}
		if (!v.moved) return true;
		if (v.st.zoom > 1.01) {
			/* a photo larger than the screen: under the finger, and it gives a
			 * little at the edges */
			double fw, fh;
			viewer_fit_of(v.current, &fw, &fh);
			struct zv want_st = { v.st.zoom, v.from.x + dx, v.from.y + dy };
			struct zv c = zv_clamped(want_st, zmc.width, zmc.height, fw, fh, viewer_max_zoom());
			v.st.x = c.x + (want_st.x - c.x) * 0.35;
			v.st.y = c.y + (want_st.y - c.y) * 0.35;
			zmc_present();
		} else if (v.across) {
			bool edge = (dx > 0 && v.index <= 0) || (dx < 0 && v.index >= v.n - 1);
			v.slide = v.slide_base + (edge ? dx * 0.3 : dx);
			zmc_present();
		}
		return true;
	}
	if (phase == 2 && at >= 0) {
		double ux = v.f[at].x, uy = v.f[at].y;
		if (at == 0 && v.n_f == 2) v.f[0] = v.f[1];
		v.n_f--;
		if (v.pinching) {
			if (v.n_f == 1) {
				/* the finger left carries on as a drag */
				v.pinching = false;
				settle();
				drag_begin(v.f[0].x, v.f[0].y, time);
				v.moved = true;
				v.across = false;
			} else {
				v.pinching = false;
				settle();
			}
			return true;
		}
		if (v.n_f != 0) return true;
		if (!v.moved) {
			viewer_tap(ux, uy, time);
			return true;
		}
		if (v.confirm) return true;
		if (v.st.zoom > 1.01) {
			double fw, fh;
			viewer_fit_of(v.current, &fw, &fh);
			struct zv c = zv_clamped(v.st, zmc.width, zmc.height, fw, fh, viewer_max_zoom());
			if (c.x != v.st.x || c.y != v.st.y) {
				zoom_to(c);
			} else if (time - v.last_time < 80) {
				v.vx = v.vel_x;
				v.vy = v.vel_y;
				v.pan_t = zmc_now_ms();
				v.panning = true;
				viewer_animate();
			}
			return true;
		}
		if (v.across || v.slide != 0) {
			double vx = time - v.last_time < 80 ? v.vel_x : 0;
			int to = zv_swipe_result(v.slide, vx, zmc.width, v.index > 0, v.index < v.n - 1);
			if (to) {
				zmc_haptic("tick");
				slide_to(to > 0 ? -(zmc.width + PHOTO_GAP) : zmc.width + PHOTO_GAP, to);
			} else {
				slide_to(0, 0);
			}
		}
		return true;
	}
	if (phase == 3) {
		v.n_f = 0;
		v.pinching = false;
		if (v.slide != 0) slide_to(0, 0);
		settle();
	}
	return true;
}

/* ---- the app ---------------------------------------------------------------------- */

static void finish_pick(struct photo *p) {
	g_free(picked);
	picked = g_strdup(p->path);
	zmc_quit();
}

static void activate(int action, int arg) {
	if (action == ACT_ALBUM && arg >= 0 && arg < n_albums) {
		g_free(album_open_dir);
		album_open_dir = g_strdup(albums[arg]->dir);
		album_open = albums[arg];
		zmc_go(PAGE_ALBUM, 1);
		return;
	}
	if (action != ACT_PHOTO) return;
	struct photo **list = zmc.page == PAGE_ALBUM && album_open ? album_open->items : all;
	int n = zmc.page == PAGE_ALBUM && album_open ? album_open->n : n_all;
	if (arg < 0 || arg >= n) return;
	if (pick_mode) {
		/* a file that cannot be read is no choice */
		if (list[arg]->state != T_BAD) finish_pick(list[arg]);
		return;
	}
	viewer_open(list, n, zmc.page == PAGE_ALBUM && album_open ? album_open->dir : NULL, arg);
}

static bool back(void) {
	if (v.state != V_CLOSED) {
		if (v.confirm) {
			v.confirm = false;
			zmc_present();
			return true;
		}
		viewer_close();
		return true;
	}
	if (zmc.page == PAGE_ALBUM) {
		zmc_go(PAGE_ALBUMS, -1);
		return true;
	}
	return false;
}

struct zmc_app gallery_app = {
	.app_id = "ztgallery", .title = "Photos", .caps = "PHOTOS",
	.render = render, .activate = activate, .back = back, .overlay = overlay,
	.touch = app_touch, .multitouch = true,
};

/* ---- stdout, for whoever asked ------------------------------------------------------- */

static int pick_stdout = -1;

/* From here on, nothing a library writes to stdout can get into the answer. */
static void pick_guard_stdout(void) {
	pick_stdout = dup(STDOUT_FILENO);
	dup2(STDERR_FILENO, STDOUT_FILENO);
}

static int pick_answer(void) {
	if (!picked) return 1;
	if (pick_out) {
		char *tmp = g_strdup_printf("%s.%ld.tmp", pick_out, (long)getpid());
		size_t n = strlen(picked);
		bool ok = false;
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd >= 0) {
			char *line = g_malloc(n + 1);
			size_t size = gl_pick_line(picked, pick_null, line, n + 1);
			ok = write(fd, line, size) == (ssize_t)size;
			g_free(line);
			ok = close(fd) == 0 && ok;
		}
		ok = ok && rename(tmp, pick_out) == 0;
		if (!ok) {
			unlink(tmp);
			fprintf(stderr, "ztgallery: cannot write %s: %s\n", pick_out, g_strerror(errno));
		}
		g_free(tmp);
		return ok ? 0 : 2;
	}
	size_t room = strlen(picked) + 1, at = 0;
	char *line = g_malloc(room);
	size_t size = gl_pick_line(picked, pick_null, line, room);
	while (at < size) {
		ssize_t n = write(pick_stdout, line + at, size - at);
		if (n <= 0) {
			g_free(line);
			return 2;
		}
		at += (size_t)n;
	}
	g_free(line);
	return 0;
}

/* ---- starting ---------------------------------------------------------------------- */

static bool webp_has_loader(void) {
	bool found = false;
	GSList *formats = gdk_pixbuf_get_formats();
	for (GSList *f = formats; f; f = f->next) {
		gchar *name = gdk_pixbuf_format_get_name(f->data);
		if (name && !strcmp(name, "webp")) found = true;
		g_free(name);
	}
	g_slist_free(formats);
	return found;
}

static void usage(FILE *to) {
	fputs("usage: ztgallery [PATH]\n"
		"       ztgallery --pick | --pick-null | --pick-out FILE\n"
		"       ztgallery --render photos|albums OUT.png\n"
		"A pick prints the chosen photo's path and exits 0, or prints nothing and exits 1.\n", to);
}

/* the first screen with its thumbnails, made here and now, no display */
static int render_page(const char *name, const char *file) {
	int page = !strcmp(name, "albums") ? PAGE_ALBUMS : PAGE_PHOTOS;
	webp_loadable = webp_has_loader();
	apply_scan(scan_files());
	int count = page == PAGE_ALBUMS ? (n_albums < 8 ? n_albums : 8) : (n_all < 40 ? n_all : 40);
	for (int i = 0; i < count && i < THUMB_SLOTS; i++) {
		struct photo *p = page == PAGE_ALBUMS ? albums[i]->items[0] : all[i];
		cairo_surface_t *s = thumb_get(p->path, p->mtime_ns, p->size);
		if (!s) continue;
		slots[i].photo = p;
		slots[i].cpu = s;
		p->slot = i;
		p->state = T_READY;
	}
	int height = getenv("ZT_RENDER_HEIGHT") ? atoi(getenv("ZT_RENDER_HEIGHT")) : 664;
	return zmc_render_png(&gallery_app, page, height, file);
}

int main(int argc, char **argv) {
	const char *render_args[2] = { NULL, NULL };
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--pick")) {
			pick_mode = true;
		} else if (!strcmp(a, "--pick-null")) {
			pick_mode = pick_null = true;
		} else if (!strcmp(a, "--pick-out") && i + 1 < argc) {
			pick_mode = true;
			pick_out = argv[++i];
		} else if (!strcmp(a, "--render") && i + 2 < argc) {
			render_args[0] = argv[++i];
			render_args[1] = argv[++i];
		} else if (!strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (a[0] == '-') {
			usage(stderr);
			return 2;
		} else if (!open_path) {
			/* the path given, whatever is in it, or a file:// from a desktop file */
			char *from_uri = !strncmp(a, "file://", 7) ? g_filename_from_uri(a, NULL, NULL) : NULL;
			char *full = realpath(from_uri ? from_uri : a, NULL);
			g_free(from_uri);
			if (full) {
				open_path = g_strdup(full);
				extra_path = g_strdup(full);
				free(full);
			}
		}
	}
	const char *env_home = getenv("HOME");
	home = env_home && env_home[0] ? env_home : "/";
	by_path = g_hash_table_new(g_str_hash, g_str_equal);
	thumb_dir = g_build_filename(g_get_user_cache_dir(), "ztgallery", "thumbs", NULL);
	g_mkdir_with_parents(thumb_dir, 0700);
	if (pick_mode) {
		pick_guard_stdout();
		gallery_app.caps = "CHOOSE A PHOTO";
		g_free(open_path);
		g_free(extra_path);
		open_path = extra_path = NULL;
	}
	if (render_args[0]) return render_page(render_args[0], render_args[1]);

	/* A photo's pixels are large and short-lived, and malloc would otherwise learn
	 * to keep them: after the first few it stops giving such blocks back, and each
	 * worker's arena holds its own. These go straight back to the system. */
	mallopt(M_MMAP_THRESHOLD, 256 * 1024);
	mallopt(M_TRIM_THRESHOLD, 512 * 1024);
	mallopt(M_ARENA_MAX, 2);

	signal(SIGPIPE, SIG_IGN);
	webp_loadable = webp_has_loader();
	results = g_async_queue_new();
	wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	zmc_watch_fd(wake_fd, results_ready, NULL);
	ino_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (ino_fd >= 0) zmc_watch_fd(ino_fd, inotify_ready, NULL);
	zmc_busy(true);
	scan_start();
	int status = zmc_run(&gallery_app, PAGE_PHOTOS);
	if (pick_mode) {
		/* nobody chose: whatever else ended the window, there is no answer */
		int answer = pick_answer();
		_exit(answer);	/* workers may be deep inside a decode: no need to wait for them */
	}
	_exit(status);
}
