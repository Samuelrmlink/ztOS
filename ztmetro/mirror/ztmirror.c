/* ztmirror: the screen and the fingers over a TCP socket, for a viewer on a PC.
 * The same program on the phone (USB-NCM) and in QEMU (a hostfwd port). See
 * README.md for the protocol.
 *
 * Two threads, so that a slow frame never holds up a finger:
 *   main     the socket's input, written straight to the shell's control pipe
 *   capture  wlr-screencopy, then JPEG, then the socket's output
 *
 * The control pipe also takes commands like "launch". Only fixed touch and key
 * lines are ever written to it, built here from numbers; nothing a client sends
 * reaches it as text. Whoever has the token can still use the phone, so this
 * listens only on a wired address and refuses Wi-Fi, and exits when unused.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <wayland-client.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "wlr-screencopy-client.h"

#define DEFAULT_PORT	27183
#define MAX_FINGERS	10	/* the shell's MAX_TOUCH */
#define PIPE_ID_BASE	100	/* pipe IDs of our fingers, clear of scripted ones (scripts/ztctl.py) */
#define HEARTBEAT_MS	5000	/* a client this quiet is gone, even with no FIN: a cable pulled */
#define HANDSHAKE_MS	3000
#define SEND_TIMEOUT_S	3

/* client to server: 8 bytes each */
enum { C_DOWN = 1, C_MOVE, C_UP, C_CANCEL, C_KEY, C_PING };
/* server to client: type, length (u32), payload */
enum { S_FRAME = 1, S_STATUS, S_ERROR, S_PONG };

static const unsigned key_codes[] = { 116, 115, 114, 172, 158 };	/* KEY_POWER, VOLUMEUP, VOLUMEDOWN, HOMEPAGE, BACK */
#define N_KEYS (sizeof(key_codes) / sizeof(key_codes[0]))

static char g_runtime[200], g_token[33];

static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("ztmirror: ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int64_t now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, v >> 32); put32(p + 4, v); }

/* ---- shared between the threads ------------------------------------------ */

static struct {
	pthread_mutex_t lock;
	int wake_fd;		/* eventfd: the capture thread has something new to look at */
	int client_fd;		/* -1: nobody */
	unsigned generation;	/* a new number for each client */
	int scale, quality, fps;
	/* from the shell's state file, or (an older shell) the output itself */
	double points_w, points_h, scale_factor;
	int pixels_w, pixels_h;
	int screen;		/* 1 on, 0 off, -1 not known */
	bool have_state;
	bool replaced;		/* the old client is told why it is being dropped */
	/* small messages waiting for the capture thread, which does all the sending */
	uint8_t out[1024];
	size_t out_len;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .client_fd = -1, .screen = -1 };

static void wake_capture(void) {
	uint64_t one = 1;
	if (write(g.wake_fd, &one, sizeof(one)) < 0) { /* full: it is awake anyway */ }
}

/* ---- the control pipe ---------------------------------------------------- */

static int g_ctl_fd = -1;

static bool ctl_write(const char *line, size_t len) {
	for (int tries = 0; tries < 2; tries++) {
		if (g_ctl_fd < 0) {
			char path[256];
			snprintf(path, sizeof(path), "%s/ztmetro.ctl", g_runtime);
			g_ctl_fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
			if (g_ctl_fd < 0) {
				return false;	/* the shell is not running */
			}
		}
		/* one write per line, each smaller than PIPE_BUF: never mixed with
		 * another writer's, and in the shell's hands at once */
		ssize_t n = write(g_ctl_fd, line, len);
		if (n == (ssize_t)len) {
			return true;
		}
		if (n < 0 && errno == EAGAIN) {
			/* the shell is behind by a full pipe. Waiting keeps the order;
			 * losing an "up" would leave a finger on the glass. */
			struct pollfd p = { g_ctl_fd, POLLOUT, 0 };
			if (poll(&p, 1, 2000) > 0 && write(g_ctl_fd, line, len) == (ssize_t)len) {
				return true;
			}
			say("the shell is not reading its control pipe");
			return false;
		}
		/* EPIPE: the shell restarted, and this was its old pipe */
		close(g_ctl_fd);
		g_ctl_fd = -1;
	}
	return false;
}

static void ctl_printf(const char *fmt, ...) {
	char line[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n > 0 && n < (int)sizeof(line)) {
		ctl_write(line, n);
	}
}

/* ---- input --------------------------------------------------------------- */

static bool g_finger[MAX_FINGERS], g_key[N_KEYS];

static void lift_everything(void) {
	for (int i = 0; i < MAX_FINGERS; i++) {
		if (g_finger[i]) {
			ctl_printf("cancel %d\n", PIPE_ID_BASE + i);
			g_finger[i] = false;
		}
	}
	for (size_t i = 0; i < N_KEYS; i++) {
		if (g_key[i]) {
			ctl_printf("key %u up\n", key_codes[i]);
			g_key[i] = false;
		}
	}
}

static void queue_out(const uint8_t *data, size_t len) {
	pthread_mutex_lock(&g.lock);
	if (g.out_len + len <= sizeof(g.out)) {
		memcpy(g.out + g.out_len, data, len);
		g.out_len += len;
	}
	pthread_mutex_unlock(&g.lock);
	wake_capture();
}

/* One message from the client, written to the pipe as it comes: the shell times
 * a fling by when a move reaches it, so moves are never merged. */
static void handle_message(const uint8_t *m) {
	unsigned type = m[0], id = m[1];
	unsigned a = m[2] << 8 | m[3], b = m[4] << 8 | m[5];
	switch (type) {
	case C_DOWN:
	case C_MOVE: {
		if (id >= MAX_FINGERS || (type == C_MOVE && !g_finger[id])) {
			return;
		}
		pthread_mutex_lock(&g.lock);
		double w = g.points_w, h = g.points_h;
		pthread_mutex_unlock(&g.lock);
		if (w <= 0 || h <= 0) {
			return;	/* the shell is not up: no screen to touch yet */
		}
		if (type == C_DOWN) {
			if (g_finger[id]) {
				ctl_printf("cancel %d\n", PIPE_ID_BASE + id);
			}
			g_finger[id] = true;
		}
		ctl_printf("%s %d %.2f %.2f\n", type == C_DOWN ? "down" : "move", PIPE_ID_BASE + id,
			a / 65535.0 * w, b / 65535.0 * h);
		break;
	}
	case C_UP:
	case C_CANCEL:
		if (id < MAX_FINGERS && g_finger[id]) {
			g_finger[id] = false;
			ctl_printf("%s %d\n", type == C_UP ? "up" : "cancel", PIPE_ID_BASE + id);
		}
		break;
	case C_KEY:
		if (id < N_KEYS && g_key[id] != (a != 0)) {
			g_key[id] = a != 0;
			ctl_printf("key %u %s\n", key_codes[id], a ? "down" : "up");
		}
		break;
	case C_PING: {
		uint8_t msg[5 + 12];
		msg[0] = S_PONG;
		put32(msg + 1, 12);
		put16(msg + 5, a);
		put16(msg + 7, b);
		put64(msg + 9, (uint64_t)now_us());
		queue_out(msg, sizeof(msg));
		break;
	}
	default:
		break;	/* from a newer client: not for this one */
	}
}

/* ---- the state of the screen --------------------------------------------- */

static void read_state(void) {
	char path[256];
	snprintf(path, sizeof(path), "%s/ztmetro.state", g_runtime);
	FILE *f = fopen(path, "r");
	double pw = 0, ph = 0, s = 0;
	int x = 0, y = 0, screen = -1;
	if (f) {
		char line[64], word[8];
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "points %lf %lf", &pw, &ph) == 2 || sscanf(line, "scale %lf", &s) == 1 ||
					sscanf(line, "pixels %d %d", &x, &y) == 2) {
				continue;
			}
			if (sscanf(line, "screen %7s", word) == 1) {
				screen = strcmp(word, "on") == 0;
			}
		}
		fclose(f);
	}
	pthread_mutex_lock(&g.lock);
	g.have_state = pw > 0;
	if (pw > 0) {
		g.points_w = pw, g.points_h = ph, g.scale_factor = s;
		g.pixels_w = x, g.pixels_h = y, g.screen = screen;
	}
	pthread_mutex_unlock(&g.lock);
}

/* ---- capture ------------------------------------------------------------- */

static struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_output *output;
	struct zwlr_screencopy_manager_v1 *manager;
	int mode_w, mode_h, output_scale;

	struct zwlr_screencopy_frame_v1 *frame;	/* one at a time */
	bool frame_ready, frame_failed, y_invert, offered, with_damage;
	uint32_t fmt_format;
	int fmt_w, fmt_h, fmt_stride;

	/* the buffer the compositor copies into, kept from frame to frame */
	struct wl_buffer *buffer;
	uint8_t *pixels;
	size_t pixels_size;
	uint32_t buf_format;
	int buf_w, buf_h, buf_stride;
} c;

static const char *g_error;	/* why there are no frames, for the status message */

static bool known_format(uint32_t f) {
	return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 ||
		f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888;
}

static void free_buffer(void) {
	if (c.buffer) {
		wl_buffer_destroy(c.buffer);
		c.buffer = NULL;
	}
	if (c.pixels) {
		munmap(c.pixels, c.pixels_size);
		c.pixels = NULL;
	}
}

static bool ensure_buffer(void) {
	if (c.buffer && c.buf_format == c.fmt_format && c.buf_w == c.fmt_w && c.buf_h == c.fmt_h &&
			c.buf_stride == c.fmt_stride) {
		return true;
	}
	free_buffer();
	size_t size = (size_t)c.fmt_stride * c.fmt_h;
	int fd = memfd_create("ztmirror", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0) close(fd);
		return false;
	}
	c.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (c.pixels == MAP_FAILED) {
		c.pixels = NULL;
		close(fd);
		return false;
	}
	c.pixels_size = size;
	struct wl_shm_pool *pool = wl_shm_create_pool(c.shm, fd, size);
	c.buffer = wl_shm_pool_create_buffer(pool, 0, c.fmt_w, c.fmt_h, c.fmt_stride, c.fmt_format);
	wl_shm_pool_destroy(pool);
	close(fd);
	c.buf_format = c.fmt_format, c.buf_w = c.fmt_w, c.buf_h = c.fmt_h, c.buf_stride = c.fmt_stride;
	return true;
}

static void frame_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
	if (!c.offered && known_format(format)) {
		c.offered = true;
		c.fmt_format = format, c.fmt_w = w, c.fmt_h = h, c.fmt_stride = stride;
	}
}
static void frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
	c.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}
static void frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
	c.frame_ready = true;
}
static void frame_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
	c.frame_failed = true;
}
static void frame_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}
static void frame_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h) {}
static void frame_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
	if (!c.offered) {
		g_error = "the screen's pixel format is not one ztmirror knows";
		c.frame_failed = true;
		return;
	}
	if (!ensure_buffer()) {
		g_error = "out of memory for the capture buffer";
		c.frame_failed = true;
		return;
	}
	if (c.with_damage) {
		zwlr_screencopy_frame_v1_copy_with_damage(f, c.buffer);
	} else {
		zwlr_screencopy_frame_v1_copy(f, c.buffer);
	}
}
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer, .flags = frame_flags, .ready = frame_ready, .failed = frame_failed,
	.damage = frame_damage, .linux_dmabuf = frame_dmabuf, .buffer_done = frame_buffer_done,
};

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
		const char *make, const char *model, int32_t transform) {}
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		c.mode_w = w, c.mode_h = h;
	}
}
static void output_done(void *d, struct wl_output *o) {}
static void output_scale(void *d, struct wl_output *o, int32_t s) {
	c.output_scale = s;
}
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale,
};

static void registry_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
	if (strcmp(iface, wl_shm_interface.name) == 0) {
		c.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0 && !c.output) {
		c.output = wl_registry_bind(r, name, &wl_output_interface, version < 2 ? version : 2);
		wl_output_add_listener(c.output, &output_listener, NULL);
	} else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0 && version >= 3) {
		c.manager = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 3);
	}
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void wayland_close(void) {
	free_buffer();
	if (c.display) {
		wl_display_disconnect(c.display);
	}
	memset(&c, 0, sizeof(c));
}

static bool wayland_open(void) {
	c.display = wl_display_connect(NULL);
	if (!c.display) {
		return false;
	}
	c.registry = wl_display_get_registry(c.display);
	wl_registry_add_listener(c.registry, &registry_listener, NULL);
	wl_display_roundtrip(c.display);
	wl_display_roundtrip(c.display);
	if (!c.shm || !c.output || !c.manager) {
		say("this compositor offers no wlr-screencopy version 3");
		wayland_close();
		return false;
	}
	return true;
}

static void frame_drop(void) {
	if (c.frame) {
		zwlr_screencopy_frame_v1_destroy(c.frame);
		c.frame = NULL;
	}
	c.frame_ready = c.frame_failed = false;
}

static void frame_request(bool with_damage) {
	c.frame = zwlr_screencopy_manager_v1_capture_output(c.manager, 0, c.output);
	c.with_damage = with_damage;
	c.offered = false;
	c.y_invert = false;
	zwlr_screencopy_frame_v1_add_listener(c.frame, &frame_listener, NULL);
}

/* Half the size, averaging each 2x2 block: four bytes a pixel, in any order.
 * dst may be src. On the phone the first halving reads 11.8 MB a frame, so it
 * is the one loop here worth writing for the CPU: NEON takes 8 pixels of each
 * row at a time, the plain version 2. */
static void halve(const uint8_t *src, int w, int h, int stride, uint8_t *dst, int dst_stride) {
	const uint64_t mask = 0x00FF00FF00FF00FFULL;
	for (int y = 0; y + 1 < h; y += 2) {
		const uint8_t *r0 = src + (size_t)y * stride, *r1 = r0 + stride;
		uint8_t *out = dst + (size_t)(y / 2) * dst_stride;
		int x = 0;
#ifdef __aarch64__
		for (; x + 8 <= w; x += 8) {
			uint8x16_t a0 = vrhaddq_u8(vld1q_u8(r0 + (size_t)x * 4), vld1q_u8(r1 + (size_t)x * 4));
			uint8x16_t a1 = vrhaddq_u8(vld1q_u8(r0 + (size_t)x * 4 + 16), vld1q_u8(r1 + (size_t)x * 4 + 16));
			uint32x4_t p0 = vreinterpretq_u32_u8(a0), p1 = vreinterpretq_u32_u8(a1);
			vst1q_u8(out, vrhaddq_u8(vreinterpretq_u8_u32(vuzp1q_u32(p0, p1)), vreinterpretq_u8_u32(vuzp2q_u32(p0, p1))));
			out += 16;
		}
#endif
		for (; x + 1 < w; x += 2) {
			uint64_t a, b;
			memcpy(&a, r0 + (size_t)x * 4, 8);
			memcpy(&b, r1 + (size_t)x * 4, 8);
			uint64_t even = (a & mask) + (b & mask), odd = ((a >> 8) & mask) + ((b >> 8) & mask);
			uint32_t e = (uint32_t)(even + (even >> 32)), o = (uint32_t)(odd + (odd >> 32));
			uint32_t px = (((e + 0x00020002) >> 2) & 0x00FF00FF) | ((((o + 0x00020002) >> 2) & 0x00FF00FF) << 8);
			memcpy(out, &px, 4);
			out += 4;
		}
	}
}

static bool send_all(int fd, struct iovec *iov, int n) {
	while (n > 0) {
		ssize_t sent = writev(fd, iov, n);
		if (sent < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		while (n > 0 && (size_t)sent >= iov->iov_len) {
			sent -= iov->iov_len;
			iov++, n--;
		}
		if (n > 0) {
			iov->iov_base = (char *)iov->iov_base + sent;
			iov->iov_len -= sent;
		}
	}
	return true;
}

static bool send_msg(int fd, int type, const void *head, size_t head_len, const void *body, size_t body_len) {
	uint8_t top[5];
	top[0] = type;
	put32(top + 1, head_len + body_len);
	struct iovec iov[3] = { { top, 5 }, { (void *)head, head_len }, { (void *)body, body_len } };
	return send_all(fd, iov, 3);
}

/* Sending failed: the main thread sees this as the client leaving. */
static void send_failed(int *fd) {
	shutdown(*fd, SHUT_RDWR);
	close(*fd);
	*fd = -1;
}

static void *capture_main(void *arg) {
	/* behind the shell if the CPU is short (this thread only: not the fingers) */
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), 5);

	tjhandle jpeg = tjInitCompress();
	unsigned char *jpeg_buf = NULL;
	unsigned long jpeg_size = 0;
	uint8_t *small = NULL;
	size_t small_size = 0;

	unsigned generation = 0;
	int fd = -1, scale = 2, quality = 70, fps = 20;
	int64_t next_frame = 0, next_status = 0, next_connect = 0, fps_start = now_us();
	int fps_count = 0, stream_w = 0, stream_h = 0;
	double fps_now = 0;
	uint32_t seq = 0;
	bool first = true, was_off = false, failed_last = false;

	for (;;) {
		int64_t now = now_us();

		/* who is watching */
		pthread_mutex_lock(&g.lock);
		bool changed = generation != g.generation;
		int old_fd = -1;
		bool replaced = false;
		if (changed) {
			generation = g.generation;
			old_fd = fd;
			replaced = g.replaced;
			g.replaced = false;
			fd = g.client_fd >= 0 ? dup(g.client_fd) : -1;
			scale = g.scale, quality = g.quality, fps = g.fps;
		}
		size_t out_len = g.out_len;
		uint8_t out[sizeof(g.out)];
		memcpy(out, g.out, out_len);
		g.out_len = 0;
		int screen = g.screen;
		bool have_state = g.have_state;
		pthread_mutex_unlock(&g.lock);
		if (changed) {
			if (old_fd >= 0) {
				if (replaced) {
					/* between frames here, so the message stands on its own */
					static const char why[] = "another client connected";
					send_msg(old_fd, S_ERROR, NULL, 0, why, sizeof(why) - 1);
				}
				shutdown(old_fd, SHUT_RDWR);
				close(old_fd);
			}
			frame_drop();
			first = true;
			next_frame = next_status = 0;
			g_error = NULL;
			stream_w = stream_h = 0;
			fps_now = 0, fps_count = 0, fps_start = now;
			seq = 0;
			if (fd < 0) {
				free_buffer();	/* nobody watching: give the memory back */
			}
		}

		/* the compositor */
		if (!c.display && fd >= 0 && now >= next_connect) {
			if (!wayland_open()) {
				next_connect = now + 1000000;
			}
			first = true;
			next_status = 0;
		}
		if (!have_state && c.display && c.mode_w > 0) {
			/* a shell without the state file: what the output says */
			int s = c.output_scale > 0 ? c.output_scale : 1;
			pthread_mutex_lock(&g.lock);
			g.points_w = (double)c.mode_w / s, g.points_h = (double)c.mode_h / s;
			g.scale_factor = s, g.pixels_w = c.mode_w, g.pixels_h = c.mode_h;
			g.screen = -1;
			pthread_mutex_unlock(&g.lock);
		}

		/* small messages first, between frames */
		if (fd >= 0 && out_len) {
			struct iovec iov = { out, out_len };
			if (!send_all(fd, &iov, 1)) {
				send_failed(&fd);
			}
		}

		/* the screen went dark or came back */
		if (screen == 0 && !was_off) {
			frame_drop();
			next_status = 0;
		} else if (screen != 0 && was_off) {
			first = true;
			next_status = 0;
		}
		was_off = screen == 0;

		/* status: when the screen changes, and once a second to show the link
		 * is alive, since a still screen sends no frames */
		if (fd >= 0 && now >= next_status) {
			if (now - fps_start >= 1000000) {
				fps_now = fps_count * 1e6 / (now - fps_start);
				fps_count = 0, fps_start = now;
			}
			pthread_mutex_lock(&g.lock);
			double pw = g.points_w, ph = g.points_h, sf = g.scale_factor;
			int xw = g.pixels_w, xh = g.pixels_h;
			pthread_mutex_unlock(&g.lock);
			char status[300];
			int len = snprintf(status, sizeof(status),
				"{\"points\":[%.2f,%.2f],\"scale\":%.2f,\"pixels\":[%d,%d],\"stream\":[%d,%d],"
				"\"screen\":\"%s\",\"compositor\":%s,\"fps\":%.1f,\"error\":\"%s\"}",
				pw, ph, sf, xw, xh, stream_w, stream_h,
				screen == 0 || (screen < 0 && failed_last) ? "off" : "on",
				c.display ? "true" : "false", fps_now, g_error ? g_error : "");
			if (!send_msg(fd, S_STATUS, NULL, 0, status, len)) {
				send_failed(&fd);
			}
			next_status = now + 1000000;
		}

		/* ask for the next frame: the first of a client, or the first after the
		 * screen comes on, takes what is there; the rest wait for damage, so a
		 * still screen costs nothing here or in the shell */
		if (fd >= 0 && c.display && !c.frame && screen != 0 && now >= next_frame && !g_error) {
			frame_request(!first);
		}

		/* wait */
		struct pollfd p[2] = { { g.wake_fd, POLLIN, 0 }, { -1, POLLIN, 0 } };
		int timeout = 500;
		if (fd >= 0) {
			int64_t until = next_status - now;
			if (!c.frame && next_frame - now < until) {
				until = next_frame - now;
			}
			if (until < 0) until = 0;
			timeout = (until + 999) / 1000;
			if (timeout > 500) timeout = 500;
		}
		if (c.display) {
			while (wl_display_prepare_read(c.display) != 0) {
				wl_display_dispatch_pending(c.display);
			}
			wl_display_flush(c.display);
			p[1].fd = wl_display_get_fd(c.display);
			if (c.frame_ready || c.frame_failed) {
				timeout = 0;
			}
		}
		poll(p, 2, timeout);
		if (p[0].revents & POLLIN) {
			uint64_t v;
			if (read(g.wake_fd, &v, sizeof(v)) < 0) { /* nothing to do */ }
		}
		if (c.display) {
			if (p[1].revents) {
				wl_display_read_events(c.display);
			} else {
				wl_display_cancel_read(c.display);
			}
			wl_display_dispatch_pending(c.display);
			int err = wl_display_get_error(c.display);
			if (err) {
				say("the compositor's connection ended (%s)", strerror(err));
				wayland_close();
				next_connect = now_us() + 1000000;
				next_status = 0;
				continue;
			}
		}

		if (c.frame_failed) {
			frame_drop();
			failed_last = true;
			next_frame = now_us() + 1000000;
			continue;
		}
		if (!c.frame_ready) {
			continue;
		}

		/* A frame. Done with the compositor before any of the slow work. */
		int64_t t_ready = now_us();
		failed_last = false;
		bool y_invert = c.y_invert;
		frame_drop();
		/* A frame only comes with a commit, so ask a little before the one
		 * wanted: asking on the dot would miss it, and 30 fps would be 20. */
		next_frame = t_ready + 1000000 / fps - 12000;
		if (fd < 0) {
			continue;
		}
		first = false;

		const uint8_t *src = c.pixels;
		int w = c.buf_w, h = c.buf_h, stride = c.buf_stride;
		if (scale > 1) {
			size_t need = (size_t)(w / 2) * (h / 2) * 4;
			if (need > small_size) {
				free(small);
				small = malloc(need);
				small_size = small ? need : 0;
			}
			if (!small) {
				g_error = "out of memory for the capture buffer";
				continue;
			}
			for (int s = scale; s > 1; s /= 2) {
				halve(src, w, h, stride, small, (w / 2) * 4);
				src = small, w /= 2, h /= 2, stride = w * 4;
			}
		}
		int pf = c.buf_format == WL_SHM_FORMAT_XRGB8888 || c.buf_format == WL_SHM_FORMAT_ARGB8888 ?
			TJPF_BGRX : TJPF_RGBX;
		if (tjCompress2(jpeg, src, w, stride, h, pf, &jpeg_buf, &jpeg_size, TJSAMP_420, quality,
				TJFLAG_FASTDCT | (y_invert ? TJFLAG_BOTTOMUP : 0)) != 0) {
			say("jpeg: %s", tjGetErrorStr2(jpeg));
			continue;
		}
		int64_t t_done = now_us();

		uint8_t head[4 + 8 + 4 + 2 + 2];
		put32(head, seq++);
		put64(head + 4, (uint64_t)t_ready);
		put32(head + 12, (uint32_t)(t_done - t_ready));
		put16(head + 16, w);
		put16(head + 18, h);
		if (!send_msg(fd, S_FRAME, head, sizeof(head), jpeg_buf, jpeg_size)) {
			send_failed(&fd);
			continue;
		}
		fps_count++;
		stream_w = w, stream_h = h;
	}
	return NULL;
}

/* ---- network ------------------------------------------------------------- */

static struct {
	const char *bind_addr;
	int port;
	char device[IFNAMSIZ];
	unsigned device_index;
	int fd;
} l = { .fd = -1 };

static const char *const default_addrs[] = { "172.16.42.1" /* the phone's USB */, "10.0.2.15" /* QEMU's guest */ };

/* The interface that has this address; "" if none here. */
static void device_of(const char *addr, char *out) {
	out[0] = 0;
	struct ifaddrs *list, *i;
	struct in_addr want;
	if (inet_pton(AF_INET, addr, &want) != 1 || getifaddrs(&list) != 0) {
		return;
	}
	for (i = list; i; i = i->ifa_next) {
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
				((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == want.s_addr) {
			snprintf(out, IFNAMSIZ, "%s", i->ifa_name);
			break;
		}
	}
	freeifaddrs(list);
}

static bool is_wireless(const char *device) {
	char path[300];
	snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", device);
	if (access(path, F_OK) == 0) return true;
	snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", device);
	return access(path, F_OK) == 0;
}

/* 1: listening; 0: not yet (the address is not up); -1: never (Wi-Fi) */
static int listen_open(void) {
	const char *addr = l.bind_addr;
	char device[IFNAMSIZ] = "";
	if (addr) {
		device_of(addr, device);
	} else {
		for (size_t i = 0; i < sizeof(default_addrs) / sizeof(*default_addrs) && !device[0]; i++) {
			addr = default_addrs[i];
			device_of(addr, device);
		}
	}
	if (!device[0]) {
		return 0;
	}
	if (is_wireless(device)) {
		say("%s is on Wi-Fi (%s): refusing to listen there", addr, device);
		return -1;
	}
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	/* A socket bound to an address still takes packets for it from any interface:
	 * this one is tied to the wire, so Wi-Fi can't reach it by routing there. */
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device, strlen(device)) != 0) {
		say("could not tie the socket to %s: %s", device, strerror(errno));
		close(fd);
		return -1;
	}
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(l.port) };
	inet_pton(AF_INET, addr, &sa.sin_addr);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 2) != 0) {
		say("could not listen on %s:%d: %s", addr, l.port, strerror(errno));
		close(fd);
		return 0;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	l.fd = fd;
	snprintf(l.device, sizeof(l.device), "%s", device);
	l.device_index = if_nametoindex(device);
	say("listening on %s:%d (%s)", addr, l.port, device);
	return 1;
}

/* A USB gadget can come back as a new interface when the cable returns. The
 * socket is tied to the old one, so it has to be made again. */
static void listen_check(void) {
	if (l.fd >= 0 && if_nametoindex(l.device) != l.device_index) {
		say("%s changed: listening again", l.device);
		close(l.fd);
		l.fd = -1;
	}
}

static void write_token(void) {
	uint8_t bytes[16];
	if (getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes)) {
		say("no random numbers");
		exit(1);
	}
	for (int i = 0; i < 16; i++) {
		sprintf(g_token + i * 2, "%02x", bytes[i]);
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/ztmirror.token", g_runtime);
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, g_token, 32) != 32) {
		say("could not write %s: %s", path, strerror(errno));
		exit(1);
	}
	close(fd);
}

static bool token_matches(const char *given) {
	unsigned diff = strlen(given) != 32;
	for (int i = 0; i < 32 && given[i]; i++) {
		diff |= (unsigned char)given[i] ^ (unsigned char)g_token[i];
	}
	return diff == 0;
}

static void client_drop(int *client, const char *why, bool replaced) {
	lift_everything();
	pthread_mutex_lock(&g.lock);
	g.client_fd = -1;
	g.generation++;
	g.out_len = 0;
	g.replaced = replaced;
	pthread_mutex_unlock(&g.lock);
	if (!replaced) {
		shutdown(*client, SHUT_RDWR);	/* wakes a send in the capture thread */
	}
	close(*client);
	*client = -1;
	wake_capture();
	say("client gone (%s)", why);
}

static void refuse(int fd, const char *why) {
	uint8_t top[5] = { S_ERROR };
	put32(top + 1, strlen(why));
	struct iovec iov[2] = { { top, 5 }, { (void *)why, strlen(why) } };
	(void)!writev(fd, iov, 2);
}

/* "ZTMIRROR 1 TOKEN scale=N quality=N fps=N\n" */
static bool handshake(int fd, const char *line, int *client) {
	char token[64];
	int version = 0, scale = 2, quality = 70, fps = 20;
	if (sscanf(line, "ZTMIRROR %d %63s scale=%d quality=%d fps=%d", &version, token, &scale, &quality, &fps) < 2 ||
			version != 1) {
		refuse(fd, "not a ztmirror client of this version");
		return false;
	}
	if (!token_matches(token)) {
		refuse(fd, "wrong token");
		say("a connection with the wrong token");
		return false;
	}
	if (scale != 1 && scale != 2 && scale != 4) scale = 2;
	if (quality < 10 || quality > 100) quality = 70;
	if (fps < 1 || fps > 60) fps = 20;

	/* one viewer at a time. The newest wins: an old one may be a cable pulled
	 * a moment ago, with nobody left to say so. */
	if (*client >= 0) {
		client_drop(client, "replaced by a new one", true);
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	/* a small queue on the way out: the next frame waits for the wire, and
	 * what the viewer sees stays recent */
	int sndbuf = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	struct timeval tv = { .tv_sec = SEND_TIMEOUT_S };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	/* the capture thread's sends wait; reads here use MSG_DONTWAIT */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

	*client = fd;
	pthread_mutex_lock(&g.lock);
	g.client_fd = fd;
	g.scale = scale, g.quality = quality, g.fps = fps;
	g.generation++;
	g.out_len = 0;
	pthread_mutex_unlock(&g.lock);
	wake_capture();
	say("client connected (1/%d size, quality %d, up to %d fps)", scale, quality, fps);
	return true;
}

static void usage(void) {
	fprintf(stderr,
		"usage: ztmirror [--bind ADDR] [--port N] [--idle-exit SECONDS]\n"
		"  --bind ADDR    an address of a wired interface (default: 172.16.42.1 if it is here,\n"
		"                 else QEMU's 10.0.2.15). Wi-Fi is refused.\n"
		"  --port N       default %d\n"
		"  --idle-exit S  exit after S seconds with no client (default 600; 0: never)\n"
		"The token a client needs is written to $XDG_RUNTIME_DIR/ztmirror.token.\n", DEFAULT_PORT);
	exit(2);
}

int main(int argc, char **argv) {
	int idle_exit = 600;
	l.port = DEFAULT_PORT;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) l.bind_addr = argv[++i];
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) l.port = atoi(argv[++i]);
		else if (strcmp(argv[i], "--idle-exit") == 0 && i + 1 < argc) idle_exit = atoi(argv[++i]);
		else usage();
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime) {
		say("XDG_RUNTIME_DIR is not set");
		return 1;
	}
	snprintf(g_runtime, sizeof(g_runtime), "%s", runtime);

	int ready = listen_open();
	if (ready < 0) {
		return 1;
	}
	if (ready == 0) {
		say("%s is not up here yet: waiting", l.bind_addr ? l.bind_addr : "the USB address");
	}
	write_token();

	signal(SIGPIPE, SIG_IGN);
	sigset_t quit;
	sigemptyset(&quit);
	sigaddset(&quit, SIGTERM);
	sigaddset(&quit, SIGINT);
	sigaddset(&quit, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &quit, NULL);	/* the capture thread inherits this */
	int signal_fd = signalfd(-1, &quit, SFD_CLOEXEC);

	g.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	read_state();
	pthread_t capture;
	pthread_create(&capture, NULL, capture_main, NULL);

	int client = -1, pending = -1;
	char pending_buf[256];
	size_t pending_len = 0;
	int64_t pending_since = 0, last_rx = 0, last_seen = now_us(), next_tick = 0;
	uint8_t rx[4096];
	size_t rx_len = 0;

	for (;;) {
		struct pollfd p[4] = {
			{ signal_fd, POLLIN, 0 }, { l.fd, POLLIN, 0 },
			{ client, POLLIN, 0 }, { pending, POLLIN, 0 },
		};
		poll(p, 4, 500);
		int64_t now = now_us();
		if (p[0].revents) {
			break;
		}

		if (client >= 0 && p[2].revents) {
			ssize_t n = recv(client, rx + rx_len, sizeof(rx) - rx_len, MSG_DONTWAIT);
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				client_drop(&client, n == 0 ? "disconnected" : strerror(errno), false);
				rx_len = 0;
				last_seen = now;
			} else if (n > 0) {
				rx_len += n;
				last_rx = now;
				size_t used = 0;
				for (; rx_len - used >= 8; used += 8) {
					handle_message(rx + used);
				}
				memmove(rx, rx + used, rx_len - used);
				rx_len -= used;
			}
		}
		if (client >= 0 && now - last_rx > HEARTBEAT_MS * 1000LL) {
			client_drop(&client, "nothing heard: the link is down", false);
			rx_len = 0;
			last_seen = now;
		}

		if (l.fd >= 0 && pending < 0 && (p[1].revents & POLLIN)) {
			pending = accept4(l.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			pending_len = 0;
			pending_since = now;
		} else if (pending >= 0 && p[3].revents) {
			ssize_t n = recv(pending, pending_buf + pending_len, sizeof(pending_buf) - 1 - pending_len, 0);
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close(pending);
				pending = -1;
			} else if (n > 0) {
				pending_len += n;
				pending_buf[pending_len] = 0;
				if (strchr(pending_buf, '\n')) {
					if (handshake(pending, pending_buf, &client)) {
						last_rx = now;
						/* what came straight behind the line is already input */
						const char *rest = strchr(pending_buf, '\n') + 1;
						rx_len = pending_buf + pending_len - rest;
						memcpy(rx, rest, rx_len);
						size_t used = 0;
						for (; rx_len - used >= 8; used += 8) {
							handle_message(rx + used);
						}
						memmove(rx, rx + used, rx_len - used);
						rx_len -= used;
					} else {
						close(pending);
					}
					pending = -1;
				} else if (pending_len == sizeof(pending_buf) - 1) {
					refuse(pending, "not a ztmirror client");
					close(pending);
					pending = -1;
				}
			}
		}
		if (pending >= 0 && now - pending_since > HANDSHAKE_MS * 1000LL) {
			close(pending);
			pending = -1;
		}

		if (now >= next_tick) {
			next_tick = now + 500000;
			read_state();
			wake_capture();
			if (client < 0) {
				listen_check();
				if (l.fd < 0 && listen_open() < 0) {
					break;
				}
				if (idle_exit > 0 && now - last_seen > idle_exit * 1000000LL) {
					say("nobody for %d s: exiting", idle_exit);
					break;
				}
			}
		}
	}

	/* no finger stays on the glass when this ends */
	lift_everything();
	char path[256];
	snprintf(path, sizeof(path), "%s/ztmirror.token", g_runtime);
	unlink(path);
	_exit(0);
}
