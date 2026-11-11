// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zt-desklat: how long a pointer move on the desktop takes to reach a viewer
 * the way ztdesk gets it: a wlr-virtual-pointer move, the desktop drawn, a
 * wlr-screencopy copy_with_damage into a DMA-BUF (shared memory if there is
 * none). It measures the time from the move being sent to the copied frame
 * being ready in the client, which is ztdesk's input-to-frame delay before
 * its own drawing.
 *
 *   zt-desklat OUTPUT W H [old|new] [STEPS]
 *
 *   old  ztdesk before: a frame asked for only after the last one is ready,
 *        and not sooner than 33 ms after the last ask (its 30 Hz tick asks)
 *   new  a frame asked for as soon as the last one is ready, so one is always
 *        waiting for the next change (copy_with_damage: a still desktop costs
 *        nothing)
 *
 * It prints the median, the 95th percentile and the frame rate while the
 * pointer moves continuously at 60 Hz.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *sc;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct { struct wl_output *o; char name[64]; } outs[8];
static int n_outs;
static struct wl_output *out;
static struct zwlr_virtual_pointer_v1 *ptr;
static double W, H;
static bool old_policy;

static struct gbm_device *gbm;
static struct { struct gbm_bo *bo; struct wl_buffer *wb; } bufs[2];
static int writing;
static struct wl_buffer *shm_buf;
static uint32_t fmt, dfmt, stride;
static int fw, fh;
static bool dma_offered, dma_failed;

static struct zwlr_screencopy_frame_v1 *frame;
static double asked_at;
static bool want;

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void out_name(void *data, struct wl_output *o, const char *name) { snprintf(data, 64, "%s", name); }
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		int32_t sub, const char *make, const char *model, int32_t tr) {}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {}
static void out_done(void *d, struct wl_output *o) {}
static void out_scale(void *d, struct wl_output *o, int32_t s) {}
static void out_desc(void *d, struct wl_output *o, const char *s) {}
static const struct wl_output_listener out_listener = { out_geometry, out_mode, out_done, out_scale, out_name, out_desc };

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
	if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		sc = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) && version >= 2)
		vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && version >= 3)
		dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
	else if (!strcmp(iface, wl_output_interface.name) && version >= 4 && n_outs < 8) {
		outs[n_outs].o = wl_registry_bind(reg, name, &wl_output_interface, 4);
		wl_output_add_listener(outs[n_outs].o, &out_listener, outs[n_outs].name);
		n_outs++;
	}
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = { global, global_remove };

/* the moves sent, and when */
#define MAX_MOVES 4096
static double sent_at[MAX_MOVES];
static int n_sent;
static double lat[MAX_MOVES];
static int n_lat;
static int last_seen;	/* the newest move already shown */
static int frames;

static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h, uint32_t s) {
	fmt = format, fw = (int)w, fh = (int)h, stride = s;
}
static void f_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h) {
	dma_offered = true, dfmt = format, fw = (int)w, fh = (int)h;
}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}

static bool dma_alloc(int i) {
	if (bufs[i].bo) return true;
	if (!gbm) {
		int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (fd < 0 || !(gbm = gbm_create_device(fd))) return false;
	}
	bufs[i].bo = gbm_bo_create(gbm, fw, fh, dfmt, GBM_BO_USE_RENDERING);
	if (!bufs[i].bo) return false;
	int fd = gbm_bo_get_fd(bufs[i].bo);
	uint64_t mod = gbm_bo_get_modifier(bufs[i].bo);
	struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
	zwp_linux_buffer_params_v1_add(p, fd, 0, 0, gbm_bo_get_stride(bufs[i].bo), mod >> 32, mod & 0xffffffff);
	bufs[i].wb = zwp_linux_buffer_params_v1_create_immed(p, fw, fh, dfmt, 0);
	zwp_linux_buffer_params_v1_destroy(p);
	close(fd);
	return bufs[i].wb != NULL;
}

static void f_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
	if (dma_offered && !dma_failed && dmabuf && dma_alloc(writing)) {
		zwlr_screencopy_frame_v1_copy_with_damage(f, bufs[writing].wb);
		return;
	}
	dma_failed = true;
	if (!shm_buf) {
		int fd = memfd_create("desklat", MFD_CLOEXEC);
		if (ftruncate(fd, (off_t)stride * fh) < 0) {}
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(stride * fh));
		shm_buf = wl_shm_pool_create_buffer(pool, 0, fw, fh, (int32_t)stride, fmt);
		wl_shm_pool_destroy(pool);
		close(fd);
	}
	zwlr_screencopy_frame_v1_copy_with_damage(f, shm_buf);
}

static void ask(void);

static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
	double t = now();
	zwlr_screencopy_frame_v1_destroy(f);
	frame = NULL;
	writing ^= 1;
	frames++;
	/* the newest move sent before this frame's render shows in it: the
	 * frame's own time (the output's commit) says when that was */
	double rendered = ((double)((uint64_t)hi << 32 | lo)) * 1e3 + ns / 1e6;
	int newest = -1;
	for (int i = n_sent - 1; i >= 0; i--) {
		if (sent_at[i] <= rendered) { newest = i; break; }
	}
	for (int i = last_seen + 1; i <= newest && n_lat < MAX_MOVES; i++) {
		lat[n_lat++] = t - sent_at[i];	/* each move, until it is seen */
	}
	if (newest > last_seen) last_seen = newest;
	if (old_policy) {
		want = true;	/* the 30 Hz tick asks, not sooner than 33 ms after the last ask */
	} else {
		ask();
	}
}

static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
	zwlr_screencopy_frame_v1_destroy(f);
	frame = NULL;
	dma_failed = true;
	want = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = f_buffer, .flags = f_flags, .ready = f_ready, .failed = f_failed, .damage = f_damage,
	.linux_dmabuf = f_dmabuf, .buffer_done = f_done,
};

static void ask(void) {
	if (frame) return;
	want = false;
	asked_at = now();
	dma_offered = false;
	frame = zwlr_screencopy_manager_v1_capture_output(sc, 1, out);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
}

static int cmp(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: zt-desklat OUTPUT W H [old|new] [MOVES]\n");
		return 2;
	}
	W = atof(argv[2]), H = atof(argv[3]);
	old_policy = argc > 4 && !strcmp(argv[4], "old");
	int moves = argc > 5 ? atoi(argv[5]) : 300;
	if (moves > MAX_MOVES) moves = MAX_MOVES;
	dpy = wl_display_connect(NULL);
	if (!dpy) return 1;
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	for (int i = 0; i < n_outs; i++) if (!strcmp(outs[i].name, argv[1])) out = outs[i].o;
	if (!out || !sc || !vpm || !seat) {
		fprintf(stderr, "zt-desklat: missing output '%s' or a protocol\n", argv[1]);
		return 1;
	}
	ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(vpm, seat, out);
	wl_display_roundtrip(dpy);
	ask();
	double next_move = now() + 300, next_tick = now(), t0 = 0;
	int frames0 = 0;
	while (n_sent < moves || now() < next_move + 200) {
		double t = now();
		if (n_sent < moves && t >= next_move) {
			/* round and round the middle of the desktop, a finger's pace */
			double a = n_sent * 0.15;
			zwlr_virtual_pointer_v1_motion_absolute(ptr, (uint32_t)t, (uint32_t)(W / 2 + 200 * cos(a)),
				(uint32_t)(H / 2 + 150 * sin(a)), (uint32_t)W, (uint32_t)H);
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_flush(dpy);
			sent_at[n_sent++] = now();
			if (n_sent == 1) t0 = now(), frames0 = frames;
			next_move += 1000.0 / 60;
		}
		if (old_policy && t >= next_tick) {
			next_tick += 33;
			if (want && !frame && t - asked_at >= 33) ask();
		}
		wl_display_flush(dpy);
		double wait = fmin(next_move, old_policy ? next_tick : next_move + 1000) - now();
		struct pollfd pfd = { wl_display_get_fd(dpy), POLLIN, 0 };
		if (wl_display_prepare_read(dpy) == 0) {
			if (poll(&pfd, 1, wait > 0 ? (int)ceil(wait) : 0) > 0) wl_display_read_events(dpy);
			else wl_display_cancel_read(dpy);
		}
		wl_display_dispatch_pending(dpy);
	}
	double secs = (now() - t0) / 1e3;
	qsort(lat, (size_t)n_lat, sizeof(lat[0]), cmp);
	printf("%s: %d moves, move to frame ready: median %.1f ms, p95 %.1f ms, max %.1f ms; %.1f frames/s (%s)\n",
		old_policy ? "old" : "new", n_lat, n_lat ? lat[n_lat / 2] : 0, n_lat ? lat[n_lat * 95 / 100] : 0,
		n_lat ? lat[n_lat - 1] : 0, (frames - frames0) / secs, dma_failed ? "shared memory" : "dma-buf");
	return 0;
}
