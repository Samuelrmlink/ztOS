// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztdesk: the desktop (desktop mode, ztmetro/desktop/DESIGN.md) seen and
 * driven from the phone, as a VNC viewer would, but in the same session.
 *
 *  - the view: wlr-screencopy of the desktop's output, into DMA-BUFs (a GPU
 *    copy, drawn as a texture: no pixel passes through the CPU) when the
 *    driver can, shared memory otherwise; only when the desktop has changed
 *    (copy_with_damage), as often as it changes (up to its refresh), and
 *    never while ztdesk is out of sight. The next frame is asked for as soon
 *    as one is ready, so one is always waiting for the next change: a move
 *    of the pointer is in ztdesk's hands one desktop frame later.
 *  - input: wlr-virtual-pointer on the desktop's output, zwp-virtual-keyboard;
 *    the shell routes both to the desktop. The shell's own keyboard types into
 *    the desktop too while ztdesk is in front.
 *  - two ways to point, remembered in ~/.config/ztdesk:
 *      touchpad: the finger moves the pointer, a tap clicks, two fingers
 *                tapped click right, two fingers moved scroll, tap then hold
 *                and move drags
 *      direct:   the desktop under the finger: a tap clicks there, a hold
 *                clicks right, a finger moved drags, two fingers scroll
 *    and in both, two fingers spread or pinched zoom the view, and move it
 *    while they do; in touchpad mode the view follows the pointer.
 */
#define _GNU_SOURCE
#include "zrnmetrocore.h"
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define BAR_H		64	/* the app bar, at the bottom */
#define KEYS_H		44	/* the row of keys the phone's keyboard lacks, over it */
#define TICK_MS		33	/* the clock: the state file, a held finger */
#define HOLD_MS		500	/* direct mode: a finger held this long clicks right */
#define TAP_MS		250
#define MOVE_SLOP	8.0	/* points a finger may wander and still tap */
#define VIEW_X		20	/* the left edge is the shell's (a swipe from it closes the app) */
#define DRM_MOD_INVALID	0x00ffffffffffffffULL

enum mode { MODE_TOUCHPAD, MODE_DIRECT };
enum { ACT_START = 1 };
enum bar_button { BB_MODE, BB_KEYBOARD, BB_KEYS, BB_FIT, BB_MORE, BB_COUNT };

struct out {
	struct wl_output *o;
	uint32_t global;
	char name[32];
};

struct cbuf {
	struct gbm_bo *bo;
	struct wl_buffer *wb;
	void *image;	/* the same buffer, as a texture (zmc_gpu_dmabuf_import) */
	int fd;
};

static struct {
	struct wl_registry *reg;
	struct zwlr_screencopy_manager_v1 *sc;
	struct zwlr_virtual_pointer_manager_v1 *vpm;
	struct zwp_virtual_keyboard_manager_v1 *vkm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct wl_shm *shm;
	struct out outs[8];
	int n_outs;

	/* the desktop, from $XDG_RUNTIME_DIR/ztmetro-desktop.state */
	bool available, session, locked;
	char name[32];
	int px_w, px_h;		/* its pixels, as laid out (turned, if it is) */
	double lw, lh;		/* and points */
	int transform;		/* the output's wl_output transform: the capture is the
				 * panel's own buffer, turned back here (frame_corners) */
	struct wl_output *output;

	struct zwlr_virtual_pointer_v1 *ptr;
	struct zwp_virtual_keyboard_v1 *kbd;
	struct xkb_keymap *keymap;
	uint32_t mod_shift, mod_ctrl, mod_alt, mod_logo;

	/* capture */
	struct zwlr_screencopy_frame_v1 *frame;
	uint32_t asked_ms;
	bool want_frame;
	int fw, fh;
	uint32_t shm_format, shm_stride, dma_format;
	bool dma_offered, dma_failed;
	struct gbm_device *gbm;
	int drm_fd;
	struct cbuf bufs[2];
	int writing, showing;	/* which buffer the copy goes into; which is drawn (-1: none) */
	struct wl_buffer *shm_buf;
	void *shm_data;
	size_t shm_size;
	cairo_surface_t *shm_surface;
	unsigned shm_rev;
	bool shm_ready, y_invert;
	unsigned frames;
	/* ZTDESK_TRACE: a move of ours to the frame that shows it, timed */
	bool trace;
	uint32_t moved_ms;		/* the oldest move not yet in a frame; 0: none */
	uint32_t trace_since, trace_frames, trace_n;
	uint32_t trace_lat[256];

	/* the view: the desktop point at the middle of the view, and how much
	 * nearer than "the whole desktop fits" */
	double cx, cy, zoom;
	double px, py;		/* the pointer, desktop points */
	enum mode mode;
	bool keys_row, keyboard_up, menu;

	/* fingers */
	struct { bool down; int32_t id; double x, y, x0, y0; } f[2];
	int nf;
	uint32_t down_ms, last_tap_ms;
	double last_tap_x, last_tap_y;
	bool moved, dragging, two, two_moved, pinching, held, own;
	double pinch_d0, pinch_zoom0, pinch_cx0, pinch_cy0, mid_x0, mid_y0, scroll_rest;
	double pointer0_x, pointer0_y;
	int bar_pressed;

	cairo_surface_t *chrome;
	unsigned chrome_rev;
	bool chrome_stale;
	cairo_surface_t *arrow;
} d = { .showing = -1, .zoom = 1.0, .drm_fd = -1, .bar_pressed = -1 };

/* ---- the shell and settings --------------------------------------------- */

static void shell_tell(const char *line) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[300];
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) {}
		close(fd);
	}
}

static void config_path(char *out, size_t size) {
	const char *home = getenv("HOME");
	snprintf(out, size, "%s/.config/ztdesk", home ? home : "");
}

static void config_load(void) {
	char path[512], line[128];
	config_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		char key[32], value[64];
		if (sscanf(line, " %31[^= ] = %63s", key, value) != 2) continue;
		if (!strcmp(key, "mode")) d.mode = !strcmp(value, "direct") ? MODE_DIRECT : MODE_TOUCHPAD;
		else if (!strcmp(key, "keys")) d.keys_row = !strcmp(value, "on");
		else if (!strcmp(key, "zoom")) {
			double z = strtod(value, NULL);
			d.zoom = z >= 1.0 && z <= 8.0 ? z : 1.0;
		}
	}
	fclose(f);
}

static void config_save(void) {
	char path[512];
	config_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "mode = %s\nkeys = %s\nzoom = %.2f\n", d.mode == MODE_DIRECT ? "direct" : "touchpad",
		d.keys_row ? "on" : "off", d.zoom);
	fclose(f);
}

/* The shell's word on its desktop: "output NAME virtual|monitor W H SCALE X Y". */
static void state_read(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[300], line[512];
	snprintf(path, sizeof(path), "%s/ztmetro-desktop.state", runtime ? runtime : "/tmp");
	bool available = false, session = false, locked = false;
	char name[32] = "";
	int w = 0, h = 0, transform = 0;
	double scale = 1;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char kind[16];
		if (sscanf(line, "output %31s %15s %d %d %lf", name, kind, &w, &h, &scale) == 5) {
			available = w > 0 && h > 0;
		} else if (!strncmp(line, "session running", 15)) {
			session = true;
		} else if (!strncmp(line, "locked yes", 10)) {
			locked = true;
		} else if (sscanf(line, "transform %d", &transform) == 1) {
			transform &= 7;
		}
	}
	if (f) fclose(f);
	/* a locked desktop is not shown (the shell covers it, and refuses our clicks) */
	available = available && !locked;
	bool changed = available != d.available || session != d.session || locked != d.locked || strcmp(name, d.name) ||
		w != d.px_w || h != d.px_h || transform != d.transform;
	d.transform = transform;
	d.available = available;
	d.session = session;
	d.locked = locked;
	if (!changed) return;
	snprintf(d.name, sizeof(d.name), "%s", name);
	d.px_w = w, d.px_h = h;
	d.lw = scale > 0 ? w / scale : w;
	d.lh = scale > 0 ? h / scale : h;
	if (d.px == 0 && d.py == 0) {
		d.px = d.lw / 2, d.py = d.lh / 2;
	}
	d.chrome_stale = true;
	zmc_redraw();
}

/* ---- the view ----------------------------------------------------------- */

static double view_h(void) {
	double h = zmc.height - BAR_H - (d.keys_row ? KEYS_H : 0);
	return h > 40 ? h : 40;
}

static double view_w(void) {
	return zmc.width - VIEW_X;
}

static double view_scale(void) {
	if (d.lw <= 0 || d.lh <= 0) return 1;
	double fit = fmin(view_w() / d.lw, view_h() / d.lh);
	return fit * d.zoom;
}

/* keep the view on the desktop: centred when it is all in sight */
static void view_clamp(void) {
	double s = view_scale();
	double half_w = view_w() / 2.0 / s, half_h = view_h() / 2.0 / s;
	if (half_w * 2 >= d.lw) d.cx = d.lw / 2;
	else d.cx = fmin(fmax(d.cx, half_w), d.lw - half_w);
	if (half_h * 2 >= d.lh) d.cy = d.lh / 2;
	else d.cy = fmin(fmax(d.cy, half_h), d.lh - half_h);
}

static void to_screen(double x, double y, double *sx, double *sy) {
	double s = view_scale();
	*sx = VIEW_X + view_w() / 2.0 + (x - d.cx) * s;
	*sy = view_h() / 2.0 + (y - d.cy) * s;
}

static void to_desktop(double sx, double sy, double *x, double *y) {
	double s = view_scale();
	*x = d.cx + (sx - VIEW_X - view_w() / 2.0) / s;
	*y = d.cy + (sy - view_h() / 2.0) / s;
}

/* touchpad mode: the view goes after the pointer when it leaves it */
static void view_follow(void) {
	if (d.mode != MODE_TOUCHPAD || d.zoom <= 1.0) return;
	double s = view_scale(), margin = 24 / s;
	double half_w = view_w() / 2.0 / s, half_h = view_h() / 2.0 / s;
	if (d.px < d.cx - half_w + margin) d.cx = d.px + half_w - margin;
	if (d.px > d.cx + half_w - margin) d.cx = d.px - half_w + margin;
	if (d.py < d.cy - half_h + margin) d.cy = d.py + half_h - margin;
	if (d.py > d.cy + half_h - margin) d.cy = d.py - half_h + margin;
	view_clamp();
}

/* ---- the virtual pointer and keyboard ---------------------------------- */

/* (every event carries our own clock: the touch's times are the shell's, and
 * a release stamped before its press is a click GTK does not see) */
static void pointer_to(double x, double y, uint32_t time) {
	if (!d.ptr || d.lw <= 0) return;
	time = zmc_now_ms();
	d.px = fmin(fmax(x, 0), d.lw - 1);
	d.py = fmin(fmax(y, 0), d.lh - 1);
	zwlr_virtual_pointer_v1_motion_absolute(d.ptr, time, (uint32_t)lround(d.px), (uint32_t)lround(d.py),
		(uint32_t)lround(d.lw), (uint32_t)lround(d.lh));
	zwlr_virtual_pointer_v1_frame(d.ptr);
	if (d.trace && !d.moved_ms) d.moved_ms = time ? time : 1;
	view_follow();
	zmc_present();
}

static void button(uint32_t code, bool down, uint32_t time) {
	if (!d.ptr) return;
	time = zmc_now_ms();
	zwlr_virtual_pointer_v1_button(d.ptr, time, code,
		down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(d.ptr);
}

/* A click: pressed now, let go on the next tick. A press and release in the
 * same instant is lost on GTK's menus (the release lands before the menu is
 * up, and takes it down again). */
static uint32_t release_button;
static void click(uint32_t code, uint32_t time) {
	if (release_button) button(release_button, false, time);
	button(code, true, time);
	release_button = code;
	zmc_haptic("tap");
}

static void scroll(double dy, double dx, uint32_t time, bool stop) {
	if (!d.ptr) return;
	time = zmc_now_ms();
	zwlr_virtual_pointer_v1_axis_source(d.ptr, WL_POINTER_AXIS_SOURCE_FINGER);
	if (stop) {
		zwlr_virtual_pointer_v1_axis_stop(d.ptr, time, WL_POINTER_AXIS_VERTICAL_SCROLL);
	} else {
		if (dy != 0) zwlr_virtual_pointer_v1_axis(d.ptr, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
			wl_fixed_from_double(dy));
		if (dx != 0) zwlr_virtual_pointer_v1_axis(d.ptr, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			wl_fixed_from_double(dx));
	}
	zwlr_virtual_pointer_v1_frame(d.ptr);
}

static void keyboard_setup(void) {
	if (!d.vkm || !zmc.seat || d.kbd) return;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	d.keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	if (!d.keymap) return;
	char *text = xkb_keymap_get_as_string(d.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t size = strlen(text) + 1;
	int fd = memfd_create("ztdesk-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
		free(text);
		if (fd >= 0) close(fd);
		return;
	}
	free(text);
	d.kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(d.vkm, zmc.seat);
	zwp_virtual_keyboard_v1_keymap(d.kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	d.mod_shift = 1u << xkb_keymap_mod_get_index(d.keymap, XKB_MOD_NAME_SHIFT);
	d.mod_ctrl = 1u << xkb_keymap_mod_get_index(d.keymap, XKB_MOD_NAME_CTRL);
	d.mod_alt = 1u << xkb_keymap_mod_get_index(d.keymap, XKB_MOD_NAME_ALT);
	d.mod_logo = 1u << xkb_keymap_mod_get_index(d.keymap, XKB_MOD_NAME_LOGO);
}

/* a key, with modifiers held round it */
static void key_combo(const uint32_t *mods, int n_mods, uint32_t key) {
	if (!d.kbd) return;
	uint32_t t = zmc_now_ms(), mask = 0;
	for (int i = 0; i < n_mods; i++) {
		zwp_virtual_keyboard_v1_key(d.kbd, t, mods[i], WL_KEYBOARD_KEY_STATE_PRESSED);
		mask |= mods[i] == KEY_LEFTALT ? d.mod_alt : mods[i] == KEY_LEFTCTRL ? d.mod_ctrl :
			mods[i] == KEY_LEFTMETA ? d.mod_logo : d.mod_shift;
	}
	if (n_mods) zwp_virtual_keyboard_v1_modifiers(d.kbd, mask, 0, 0, 0);
	zwp_virtual_keyboard_v1_key(d.kbd, t, key, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(d.kbd, t + 1, key, WL_KEYBOARD_KEY_STATE_RELEASED);
	for (int i = n_mods - 1; i >= 0; i--) {
		zwp_virtual_keyboard_v1_key(d.kbd, t + 2, mods[i], WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	if (n_mods) zwp_virtual_keyboard_v1_modifiers(d.kbd, 0, 0, 0, 0);
	zmc_haptic("tap");
}

/* the keys row: what the phone's keyboard has no key for */
static const struct { const char *label; uint32_t mods[2]; int n; uint32_t key; } keys[] = {
	{ "Esc", {0}, 0, KEY_ESC }, { "Tab", {0}, 0, KEY_TAB }, { "Win", {0}, 0, KEY_LEFTMETA },
	{ "Alt+Tab", { KEY_LEFTALT }, 1, KEY_TAB }, { "Alt+F4", { KEY_LEFTALT }, 1, KEY_F4 },
	{ "←", {0}, 0, KEY_LEFT }, { "↑", {0}, 0, KEY_UP }, { "↓", {0}, 0, KEY_DOWN },
	{ "→", {0}, 0, KEY_RIGHT },
};
#define N_KEYS (int)(sizeof(keys) / sizeof(keys[0]))

/* ---- capture -------------------------------------------------------------- */

static void capture_request(void);

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
		uint32_t w, uint32_t h, uint32_t stride) {
	d.shm_format = format, d.shm_stride = stride;
	d.fw = (int)w, d.fh = (int)h;
}

static void frame_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
		uint32_t w, uint32_t h) {
	d.dma_offered = true;
	d.dma_format = format;
	d.fw = (int)w, d.fh = (int)h;
}

static void cbuf_free(struct cbuf *b) {
	if (b->image) zmc_gpu_dmabuf_free(b->image);
	if (b->wb) wl_buffer_destroy(b->wb);
	if (b->bo) gbm_bo_destroy(b->bo);
	if (b->fd >= 0) close(b->fd);
	*b = (struct cbuf){ .fd = -1 };
}

/* two buffers on the GPU, which the shell copies into and we draw from */
static bool dma_alloc(struct cbuf *b) {
	if (b->bo && (int)gbm_bo_get_width(b->bo) == d.fw && (int)gbm_bo_get_height(b->bo) == d.fh) {
		return true;
	}
	cbuf_free(b);
	if (!d.gbm) {
		static const char *const nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", NULL };
		for (int i = 0; nodes[i] && d.drm_fd < 0; i++) d.drm_fd = open(nodes[i], O_RDWR | O_CLOEXEC);
		if (d.drm_fd < 0) return false;
		d.gbm = gbm_create_device(d.drm_fd);
		if (!d.gbm) return false;
	}
	b->bo = gbm_bo_create(d.gbm, d.fw, d.fh, d.dma_format, GBM_BO_USE_RENDERING);
	if (!b->bo) return false;
	b->fd = gbm_bo_get_fd(b->bo);
	uint32_t stride = gbm_bo_get_stride(b->bo), offset = 0;
	uint64_t mod = gbm_bo_get_modifier(b->bo);
	struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(d.dmabuf);
	zwp_linux_buffer_params_v1_add(params, b->fd, 0, offset, stride, mod >> 32, mod & 0xffffffff);
	b->wb = zwp_linux_buffer_params_v1_create_immed(params, d.fw, d.fh, d.dma_format, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	b->image = zmc_gpu_dmabuf_import(d.fw, d.fh, d.dma_format, mod, 1, &b->fd, &offset, &stride);
	if (!b->wb || !b->image) {
		cbuf_free(b);
		return false;
	}
	return true;
}

static bool shm_alloc(void) {
	size_t size = (size_t)d.shm_stride * d.fh;
	if (d.shm_buf && size == d.shm_size) return true;
	if (d.shm_buf) {
		wl_buffer_destroy(d.shm_buf);
		cairo_surface_destroy(d.shm_surface);
		munmap(d.shm_data, d.shm_size);
		d.shm_buf = NULL;
	}
	int fd = memfd_create("ztdesk-frame", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0) close(fd);
		return false;
	}
	d.shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(d.shm, fd, (int32_t)size);
	d.shm_buf = wl_shm_pool_create_buffer(pool, 0, d.fw, d.fh, (int32_t)d.shm_stride, d.shm_format);
	wl_shm_pool_destroy(pool);
	close(fd);
	d.shm_size = size;
	/* XRGB8888 and ARGB8888 are cairo's RGB24 and ARGB32 */
	d.shm_surface = cairo_image_surface_create_for_data(d.shm_data, CAIRO_FORMAT_RGB24, d.fw, d.fh,
		(int)d.shm_stride);
	return true;
}

static void frame_done(void *data, struct zwlr_screencopy_frame_v1 *f) {
	/* buffer_done: every buffer kind on offer is known; copy into one */
	if (d.dma_offered && !d.dma_failed && d.dmabuf) {
		struct cbuf *b = &d.bufs[d.writing];
		if (dma_alloc(b)) {
			zwlr_screencopy_frame_v1_copy_with_damage(f, b->wb);
			return;
		}
		d.dma_failed = true;
		fprintf(stderr, "ztdesk: no DMA-BUF capture here, shared memory instead\n");
	}
	if (d.shm && shm_alloc()) {
		zwlr_screencopy_frame_v1_copy_with_damage(f, d.shm_buf);
		return;
	}
	zwlr_screencopy_frame_v1_destroy(f);
	d.frame = NULL;
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
	d.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static int trace_cmp(const void *a, const void *b) {
	return (int)*(const uint32_t *)a - (int)*(const uint32_t *)b;
}

/* every 2 s while the finger moves: frames a second, move to frame (median, p95) */
static void trace_frame(void) {
	uint32_t now = zmc_now_ms();
	if (!d.trace_since) d.trace_since = now;
	d.trace_frames++;
	if (d.moved_ms && d.trace_n < 256) {
		d.trace_lat[d.trace_n++] = now - d.moved_ms;
	}
	d.moved_ms = 0;
	if (now - d.trace_since >= 2000) {
		if (d.trace_n) {
			qsort(d.trace_lat, d.trace_n, sizeof(d.trace_lat[0]), trace_cmp);
			fprintf(stderr, "ztdesk: %.1f frames/s, move to frame: median %u ms, p95 %u ms (%u moves)\n",
				d.trace_frames * 1000.0 / (now - d.trace_since), d.trace_lat[d.trace_n / 2],
				d.trace_lat[d.trace_n * 95 / 100], d.trace_n);
		}
		d.trace_since = now, d.trace_frames = 0, d.trace_n = 0;
	}
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
	zwlr_screencopy_frame_v1_destroy(f);
	d.frame = NULL;
	d.frames++;
	if (d.trace) trace_frame();
	if (d.dma_offered && !d.dma_failed && d.bufs[d.writing].image) {
		d.showing = d.writing;
		d.writing ^= 1;
	} else {
		d.shm_ready = true;
		d.shm_rev++;
		cairo_surface_mark_dirty(d.shm_surface);
	}
	zmc_present();
	capture_request();
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *f) {
	zwlr_screencopy_frame_v1_destroy(f);
	d.frame = NULL;
	if (d.dma_offered && !d.dma_failed) {
		d.dma_failed = true;	/* the next one in shared memory */
		cbuf_free(&d.bufs[0]);
		cbuf_free(&d.bufs[1]);
	}
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h) {
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer, .flags = frame_flags, .ready = frame_ready, .failed = frame_failed,
	.damage = frame_damage, .linux_dmabuf = frame_dmabuf, .buffer_done = frame_done,
};

/* The next frame, when there is a desktop to see and we are in front. Asked
 * for at once (it was at most every 33 ms, after the last was ready, on the
 * tick: 18 frames a second and 28 ms from a move to its frame, median;
 * zt-desklat measures both ways). copy_with_damage holds it until the desktop
 * changes, so a still desktop still costs nothing. */
static void capture_request(void) {
	if (d.frame || !d.sc || !d.output || !d.available || !zmc.active) {
		d.want_frame = !d.frame;
		return;
	}
	uint32_t now = zmc_now_ms();
	d.want_frame = false;
	d.asked_ms = now;
	d.dma_offered = false;
	d.frame = zwlr_screencopy_manager_v1_capture_output(d.sc, 1, d.output);
	zwlr_screencopy_frame_v1_add_listener(d.frame, &frame_listener, NULL);
}

/* ---- the globals ---------------------------------------------------------- */

static void output_name(void *data, struct wl_output *o, const char *name) {
	struct out *out = data;
	snprintf(out->name, sizeof(out->name), "%s", name);
}
static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		int32_t sub, const char *make, const char *model, int32_t transform) {}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {}
static void output_done(void *data, struct wl_output *o) {}
static void output_scale(void *data, struct wl_output *o, int32_t factor) {}
static void output_description(void *data, struct wl_output *o, const char *desc) {}
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale,
	.name = output_name, .description = output_description,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
	if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
		d.sc = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
	} else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) && version >= 2) {
		d.vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
	} else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		d.vkm = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && version >= 3) {
		d.dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		d.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name) && version >= 4 && d.n_outs < 8) {
		struct out *o = &d.outs[d.n_outs++];
		o->global = name;
		o->name[0] = 0;
		o->o = wl_registry_bind(reg, name, &wl_output_interface, 4);
		wl_output_add_listener(o->o, &output_listener, o);
	}
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
	for (int i = 0; i < d.n_outs; i++) {
		if (d.outs[i].global != name) continue;
		if (d.output == d.outs[i].o) {
			if (d.ptr) zwlr_virtual_pointer_v1_destroy(d.ptr);
			d.ptr = NULL;
			d.output = NULL;
			d.showing = -1;
			d.shm_ready = false;
		}
		wl_output_release(d.outs[i].o);
		d.outs[i] = d.outs[--d.n_outs];
		return;
	}
}

static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

/* the desktop's wl_output, by the name the shell gave, and a pointer on it */
static void output_pick(void) {
	if (!d.available) return;
	struct wl_output *want = NULL;
	for (int i = 0; i < d.n_outs; i++) {
		if (!strcmp(d.outs[i].name, d.name)) want = d.outs[i].o;
	}
	if (want == d.output) return;
	if (d.ptr) zwlr_virtual_pointer_v1_destroy(d.ptr);
	d.ptr = NULL;
	d.output = want;
	d.showing = -1;
	d.shm_ready = false;
	cbuf_free(&d.bufs[0]);
	cbuf_free(&d.bufs[1]);
	if (want && d.vpm && zmc.seat) {
		/* absolute positions are on the desktop's output, never the phone's */
		d.ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(d.vpm, zmc.seat, want);
	}
	view_clamp();
	capture_request();
}

/* ---- drawing -------------------------------------------------------------- */

static void glyph(cairo_t *cr, int which, double cx, double cy) {
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (which) {
	case BB_MODE:
		if (d.mode == MODE_TOUCHPAD) {	/* a touchpad: a rounded box, two buttons */
			cairo_rectangle(cr, cx - 9, cy - 8, 18, 16);
			cairo_move_to(cr, cx - 9, cy + 3);
			cairo_line_to(cr, cx + 9, cy + 3);
			cairo_move_to(cr, cx, cy + 3);
			cairo_line_to(cr, cx, cy + 8);
		} else {			/* a finger on a point */
			cairo_arc(cr, cx, cy - 2, 5, 0, 2 * M_PI);
			cairo_move_to(cr, cx - 9, cy + 9);
			cairo_line_to(cr, cx + 9, cy + 9);
		}
		cairo_stroke(cr);
		break;
	case BB_KEYBOARD:
		cairo_rectangle(cr, cx - 10, cy - 6, 20, 12);
		cairo_stroke(cr);
		for (int i = 0; i < 4; i++) cairo_rectangle(cr, cx - 7 + i * 4, cy - 3, 2, 2);
		cairo_rectangle(cr, cx - 5, cy + 2, 10, 1.5);
		cairo_fill(cr);
		break;
	case BB_KEYS:	/* "esc" in a key */
		cairo_rectangle(cr, cx - 9, cy - 7, 18, 14);
		cairo_stroke(cr);
		zmc_text_centred(cr, "esc", FONT_REGULAR, 8.5, cx, cy - 6);
		break;
	case BB_FIT:	/* four corners */
		for (int i = 0; i < 4; i++) {
			double sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1;
			cairo_move_to(cr, cx + sx * 9, cy + sy * 3);
			cairo_line_to(cr, cx + sx * 9, cy + sy * 8);
			cairo_line_to(cr, cx + sx * 4, cy + sy * 8);
		}
		cairo_stroke(cr);
		break;
	case BB_MORE:
		for (int i = -1; i <= 1; i++) cairo_arc(cr, cx + i * 7, cy, 1.6, 0, 2 * M_PI), cairo_fill(cr);
		break;
	}
}

static const char *const bar_labels[BB_COUNT] = { "touchpad", "keyboard", "keys", "fit", "" };

static double bar_x(int i) {
	return zmc.width * (i + 0.5) / BB_COUNT;
}

static const char *menu_item(int i) {
	switch (i) {
	case 0: return d.session ? "stop the desktop" : "start the desktop";
	case 1: return "remove the virtual screen";
	}
	return "";
}
#define MENU_ITEMS 2
#define MENU_ROW 46

/* the bar, the keys row and the menu: drawn with cairo when they change,
 * shown every frame as one texture over the view */
static void chrome_draw(void) {
	int s = zmc.scale > 0 ? zmc.scale : 1;
	int w = zmc.width, h = zmc.height;
	if (!d.chrome || cairo_image_surface_get_width(d.chrome) != w * s ||
			cairo_image_surface_get_height(d.chrome) != h * s) {
		if (d.chrome) cairo_surface_destroy(d.chrome);
		d.chrome = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * s, h * s);
	}
	cairo_t *cr = cairo_create(d.chrome);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, s, s);
	double bar_y = h - BAR_H - (d.menu ? MENU_ITEMS * MENU_ROW + 8 : 0);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);	/* WP's app bar: dark, the buttons in white rings */
	cairo_rectangle(cr, 0, bar_y, w, h - bar_y);
	cairo_fill(cr);
	for (int i = 0; i < BB_COUNT; i++) {
		double cx = bar_x(i), cy = bar_y + 22;
		bool on = (i == BB_KEYS && d.keys_row) || (i == BB_KEYBOARD && d.keyboard_up) || d.bar_pressed == i;
		if (i != BB_MORE) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2);
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, 17, 0, 2 * M_PI);
			if (on) cairo_fill(cr); else cairo_stroke(cr);
		}
		if (on && i != BB_MORE) cairo_set_source_rgb(cr, 0, 0, 0);
		else cairo_set_source_rgb(cr, 1, 1, 1);
		glyph(cr, i, cx, cy);
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		const char *label = i == BB_MODE && d.mode == MODE_DIRECT ? "direct" : bar_labels[i];
		zmc_text_centred(cr, label, FONT_REGULAR, 10, cx, cy + 22);
	}
	if (d.menu) {
		for (int i = 0; i < MENU_ITEMS; i++) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, menu_item(i), FONT_SEMILIGHT, 19, 16, h - MENU_ITEMS * MENU_ROW - 4 + i * MENU_ROW + 10, 0);
		}
	}
	if (d.keys_row) {
		double y = bar_y - KEYS_H, kw = (double)w / N_KEYS;
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		cairo_rectangle(cr, 0, y, w, KEYS_H);
		cairo_fill(cr);
		for (int i = 0; i < N_KEYS; i++) {
			cairo_set_source_rgb(cr, 0.32, 0.32, 0.32);
			cairo_rectangle(cr, i * kw + 2, y + 4, kw - 4, KEYS_H - 8);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_centred(cr, keys[i].label, FONT_REGULAR, strlen(keys[i].label) > 4 ? 10 : 13,
				i * kw + kw / 2, y + KEYS_H / 2 - 9);
		}
	}
	cairo_destroy(cr);
	cairo_surface_flush(d.chrome);
	d.chrome_rev++;
	d.chrome_stale = false;
}

static void arrow_make(void) {
	int s = zmc.scale > 0 ? zmc.scale : 1;
	d.arrow = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16 * s, 22 * s);
	cairo_t *cr = cairo_create(d.arrow);
	cairo_scale(cr, s, s);
	cairo_move_to(cr, 1, 1);
	cairo_line_to(cr, 1, 18);
	cairo_line_to(cr, 5.5, 14);
	cairo_line_to(cr, 9, 21);
	cairo_line_to(cr, 12, 19.5);
	cairo_line_to(cr, 8.5, 13);
	cairo_line_to(cr, 14, 13);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1.2);
	cairo_stroke(cr);
	cairo_destroy(cr);
}

/* Where the capture's corners go (top left, top right, bottom left, bottom
 * right of the buffer, as copied): on the desktop's corners as laid out,
 * turned back by the output's transform (the capture is not turned for us),
 * and flipped if the copy came upside down. */
static void frame_corners(double x0, double y0, double x1, double y1, double c[4][3]) {
	for (int i = 0; i < 4; i++) {
		double bx = i & 1, by = i & 2 ? 1 : 0, ux = bx, uy = by;
		if (d.y_invert) by = 1 - by;
		/* the buffer's corner (bx, by), 0..1, on the laid-out desktop (ux, uy):
		 * wl_output transform 90 is the content turned a quarter
		 * anticlockwise into the buffer, and so on */
		switch (d.transform & 3) {
		case 1: ux = 1 - by; uy = bx; break;
		case 2: ux = 1 - bx; uy = 1 - by; break;
		case 3: ux = by; uy = 1 - bx; break;
		default: ux = bx; uy = by; break;
		}
		if (d.transform & 4) ux = 1 - ux;	/* the flipped ones: mirrored across */
		c[i][0] = x0 + ux * (x1 - x0);
		c[i][1] = y0 + uy * (y1 - y0);
		c[i][2] = 0;
	}
}

static void overlay(int page) {
	if (!d.available) return;
	/* a new size (the phone turned: auto-rotation): the bar drawn again for it */
	static double chrome_w, chrome_h;
	if (zmc.width != chrome_w || zmc.height != chrome_h) {
		chrome_w = zmc.width, chrome_h = zmc.height;
		d.chrome_stale = true;
	}
	if (d.chrome_stale || !d.chrome) chrome_draw();
	view_clamp();
	double x0, y0, x1, y1;
	to_screen(0, 0, &x0, &y0);
	to_screen(d.lw, d.lh, &x1, &y1);
	double c[4][3];
	frame_corners(x0, y0, x1, y1, c);
	if (d.showing >= 0 && d.bufs[d.showing].image) {
		zmc_gpu_dmabuf(d.bufs[d.showing].image, c, 1);
	} else if (d.shm_ready && d.shm_surface) {
		zmc_gpu_quad(d.shm_surface, d.shm_rev, c, 1);
	} else {
		zmc_gpu_rect(x0, y0, x1 - x0, y1 - y0, 0.1, 0.1, 0.12);
	}
	if (d.mode == MODE_TOUCHPAD) {
		/* our own arrow, where we put the pointer: the capture may not show it */
		if (!d.arrow) arrow_make();
		double sx, sy;
		to_screen(d.px, d.py, &sx, &sy);
		double q[4][3] = { { sx, sy, 0 }, { sx + 16, sy, 0 }, { sx, sy + 22, 0 }, { sx + 16, sy + 22, 0 } };
		zmc_gpu_quad(d.arrow, 1, q, 1);
	}
	double full[4][3] = { { 0, 0, 0 }, { zmc.width, 0, 0 }, { 0, zmc.height, 0 }, { zmc.width, zmc.height, 0 } };
	zmc_gpu_quad(d.chrome, d.chrome_rev, full, 1);
}

static double render(cairo_t *cr, int page) {
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	if (d.available) {
		return zmc.height;	/* the rest is drawn on the GPU, every frame (overlay) */
	}
	double y = zmc_header(cr, "desktop");
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (d.locked) {
		y += zmc_text(cr, "The desktop is locked.", FONT_SEMILIGHT, 20, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += 12 + zmc_text(cr, "It locked with the phone when the dock was unplugged. Its apps are still "
			"running; unlocking the phone unlocks it.", FONT_REGULAR, 15, ZMC_MARGIN, y + 12,
			zmc.width - 2 * ZMC_MARGIN);
		return y;
	}
	y += zmc_text(cr, "No desktop is running.", FONT_SEMILIGHT, 20, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += 12 + zmc_text(cr, "Plug in a monitor, or start one here on a virtual screen and use it from this app.",
		FONT_REGULAR, 15, ZMC_MARGIN, y + 12, zmc.width - 2 * ZMC_MARGIN);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y = zmc_button(cr, ZMC_MARGIN, y + 24, zmc.width - 2 * ZMC_MARGIN, "Start a virtual desktop", ACT_START, 0);
	return y;
}

static void activate(int action, int arg) {
	if (action == ACT_START) {
		shell_tell("desktop add\ndesktop start\n");
	}
}

/* ---- fingers ---------------------------------------------------------------- */

static int bar_hit(double x, double y) {
	double bar_y = zmc.height - BAR_H - (d.menu ? MENU_ITEMS * MENU_ROW + 8 : 0);
	if (y < bar_y - (d.keys_row ? KEYS_H : 0)) return -1;
	if (d.keys_row && y < bar_y) return 100 + (int)(x / ((double)zmc.width / N_KEYS));
	if (d.menu && y >= zmc.height - MENU_ITEMS * MENU_ROW - 4) {
		return 200 + (int)((y - (zmc.height - MENU_ITEMS * MENU_ROW - 4)) / MENU_ROW);
	}
	if (y < bar_y + BAR_H) return (int)(x / ((double)zmc.width / BB_COUNT));
	return -2;	/* the bar, nothing on it */
}

static void bar_press(int hit) {
	if (hit >= 100 && hit < 100 + N_KEYS) {
		const __typeof__(keys[0]) *k = &keys[hit - 100];
		key_combo(k->mods, k->n, k->key);
		return;
	}
	if (hit >= 200) {
		d.menu = false;
		if (hit == 200) shell_tell(d.session ? "desktop stop\n" : "desktop start\n");
		if (hit == 201) shell_tell("desktop remove\n");
		d.chrome_stale = true;
		return;
	}
	switch (hit) {
	case BB_MODE:
		d.mode = d.mode == MODE_TOUCHPAD ? MODE_DIRECT : MODE_TOUCHPAD;
		config_save();
		break;
	case BB_KEYBOARD:
		d.keyboard_up = !d.keyboard_up;
		zmc_keyboard(d.keyboard_up);	/* the shell's: typing goes to the desktop */
		break;
	case BB_KEYS:
		d.keys_row = !d.keys_row;
		config_save();
		break;
	case BB_FIT:
		d.zoom = 1.0;
		config_save();
		break;
	case BB_MORE:
		d.menu = !d.menu;
		break;
	}
	zmc_haptic("tap");
	d.chrome_stale = true;
}

static int finger(int32_t id) {
	for (int i = 0; i < 2; i++) if (d.f[i].down && d.f[i].id == id) return i;
	return -1;
}

static double fdist(void) {
	return hypot(d.f[0].x - d.f[1].x, d.f[0].y - d.f[1].y);
}

static void two_begin(uint32_t time) {
	d.two = true;
	d.two_moved = d.pinching = false;
	if (d.dragging) {
		button(BTN_LEFT, false, time);
		d.dragging = false;
	}
	d.pinch_d0 = fdist();
	d.pinch_zoom0 = d.zoom;
	d.mid_x0 = (d.f[0].x + d.f[1].x) / 2, d.mid_y0 = (d.f[0].y + d.f[1].y) / 2;
	to_desktop(d.mid_x0, d.mid_y0, &d.pinch_cx0, &d.pinch_cy0);	/* the point between them stays put */
	d.scroll_rest = 0;
}

static void two_move(uint32_t time) {
	double mx = (d.f[0].x + d.f[1].x) / 2, my = (d.f[0].y + d.f[1].y) / 2;
	double ratio = d.pinch_d0 > 1 ? fdist() / d.pinch_d0 : 1;
	if (!d.pinching && (ratio > 1.12 || ratio < 0.89)) {
		d.pinching = true;	/* from now on, this pair zooms and moves the view */
	}
	if (hypot(mx - d.mid_x0, my - d.mid_y0) > MOVE_SLOP) d.two_moved = true;
	if (d.pinching) {
		d.zoom = fmin(fmax(d.pinch_zoom0 * ratio, 1.0), 8.0);
		double s = view_scale();
		d.cx = d.pinch_cx0 - (mx - VIEW_X - view_w() / 2.0) / s;
		d.cy = d.pinch_cy0 - (my - view_h() / 2.0) / s;
		view_clamp();
		zmc_present();
		return;
	}
	if (d.two_moved) {
		/* scrolling, the content following the fingers (as on a touchpad) */
		double dy = my - d.mid_y0, dx = mx - d.mid_x0;
		double step = 1.0 / fmax(view_scale(), 0.25);
		scroll(-(dy - d.scroll_rest) * step, 0, time, false);
		(void)dx;
		d.scroll_rest = dy;
	}
}

static bool touch(int phase, int32_t id, double x, double y, uint32_t time) {
	if (getenv("ZTDESK_DEBUG")) {
		fprintf(stderr, "ztdesk: touch %d id %d at %.0f,%.0f (%u)\n", phase, id, x, y, time);
	}
	if (!d.available) return false;
	if (phase == 0) {
		if (d.nf == 0) {
			int hit = bar_hit(x, y);
			if (hit != -1) {
				d.bar_pressed = hit;
				if (hit < BB_COUNT && hit >= 0) d.chrome_stale = true;
				zmc_present();
				d.own = false;
				return true;
			}
			if (d.menu) {
				d.menu = false;
				d.chrome_stale = true;
				zmc_present();
				return true;
			}
		}
		if (d.bar_pressed != -1) return true;
		int i = d.f[0].down ? (d.f[1].down ? -1 : 1) : 0;
		if (i < 0) return true;	/* a third finger: nothing */
		d.f[i] = (__typeof__(d.f[0])){ true, id, x, y, x, y };
		d.nf++;
		d.own = true;
		if (d.nf == 1) {
			d.down_ms = time;
			d.moved = d.held = d.two = false;
			d.pointer0_x = d.px, d.pointer0_y = d.py;
			if (d.mode == MODE_DIRECT) {
				double dx, dy;
				to_desktop(x, y, &dx, &dy);
				pointer_to(dx, dy, time);
			}
		} else {
			two_begin(time);
		}
		return true;
	}
	if (d.bar_pressed != -1) {
		if (phase == 2) {
			int hit = d.bar_pressed;
			d.bar_pressed = -1;
			if (hit >= 0) bar_press(hit);
			d.chrome_stale = true;
			zmc_present();
		} else if (phase == 3) {
			d.bar_pressed = -1;
			d.chrome_stale = true;
		}
		return true;
	}
	int i = finger(id);
	if (phase == 1 && i >= 0) {
		double fx = x - d.f[i].x, fy = y - d.f[i].y;
		d.f[i].x = x, d.f[i].y = y;
		if (d.two) {
			two_move(time);
			return true;
		}
		if (!d.moved && hypot(x - d.f[i].x0, y - d.f[i].y0) > MOVE_SLOP) {
			d.moved = true;
			if (d.mode == MODE_DIRECT ||
					(time - d.last_tap_ms < 300 && hypot(x - d.last_tap_x, y - d.last_tap_y) < 40)) {
				/* direct: a finger that moves drags; touchpad: tap, then hold and move */
				if (release_button) {
					button(release_button, false, time);	/* the tap's own click, done */
					release_button = 0;
				}
				button(BTN_LEFT, true, time);
				d.dragging = true;
			}
		}
		if (!d.moved || d.held) return true;
		if (d.mode == MODE_DIRECT) {
			double dx, dy;
			to_desktop(x, y, &dx, &dy);
			pointer_to(dx, dy, time);
		} else {
			/* a touchpad: the pointer goes as far on the desktop as the finger
			 * on the view, a little further when the whole desktop is shown */
			double k = 1.6 / view_scale();
			pointer_to(d.px + fx * k, d.py + fy * k, time);
		}
		return true;
	}
	if ((phase == 2 || phase == 3) && i >= 0) {
		bool cancel = phase == 3;
		d.f[i].down = false;
		d.nf--;
		if (d.two) {
			if (d.nf == 0) {
				if (!cancel && !d.two_moved && !d.pinching && time - d.down_ms < 400) {
					click(BTN_RIGHT, time);	/* two fingers tapped */
				} else if (d.two_moved && !d.pinching) {
					scroll(0, 0, time, true);
				}
				if (d.pinching) config_save();
				d.two = false;
			}
			return true;
		}
		if (d.dragging) {
			button(BTN_LEFT, false, time);
			d.dragging = false;
		} else if (!cancel && !d.moved && !d.held && time - d.down_ms < (d.mode == MODE_DIRECT ? HOLD_MS : TAP_MS)) {
			click(BTN_LEFT, time);
			d.last_tap_ms = time;
			d.last_tap_x = x, d.last_tap_y = y;
		}
		return true;
	}
	return true;
}

/* ---- the clock: the state, a held finger, a frame due ------------------------ */

static void tick(void) {
	static unsigned n;
	if (release_button) {
		button(release_button, false, zmc_now_ms());
		release_button = 0;
	}
	if (++n % 15 == 0) {	/* every half second */
		state_read();
		output_pick();
		keyboard_setup();
	}
	if (d.mode == MODE_DIRECT && d.nf == 1 && !d.two && !d.moved && !d.held &&
			zmc_now_ms() - d.down_ms >= HOLD_MS) {
		d.held = true;
		click(BTN_RIGHT, zmc_now_ms());	/* held: the right button */
	}
	if (d.want_frame || (!d.frame && d.showing < 0 && !d.shm_ready)) {
		capture_request();
	}
	if (d.chrome_stale) zmc_present();
}

static void arrived(int page) {
	static bool once;
	if (once) return;
	once = true;
	d.reg = wl_display_get_registry(zmc.display);
	wl_registry_add_listener(d.reg, &reg_listener, NULL);
	state_read();
	zmc_every(TICK_MS, tick);
}

static bool back(void) {
	if (d.menu) {
		d.menu = false;
		d.chrome_stale = true;
		return true;
	}
	if (d.keyboard_up) {
		d.keyboard_up = false;
		zmc_keyboard(false);
		d.chrome_stale = true;
		return true;
	}
	return false;
}

static const struct zmc_app app = {
	.app_id = "ztdesk", .title = "Desktop", .caps = "DESKTOP",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .overlay = overlay,
	.touch = touch, .multitouch = true, .fixed = true, .overlay_above_header = true,
};

int main(int argc, char **argv) {
	for (int i = 0; i < 2; i++) d.bufs[i].fd = -1;
	d.trace = getenv("ZTDESK_TRACE") != NULL;
	config_load();
	d.chrome_stale = true;
	if (argc == 3 && !strcmp(argv[1], "--render")) {
		return zmc_render_png(&app, 0, 740, argv[2]);
	}
	return zmc_run(&app, 0);
}
