// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The real cameras, from /dev/zrn-cam (camera/driver/zrn-cam-live.h): 16-bit
 * Bayer with no ISP. The GPU reads the driver's memory directly as a DMA-BUF and
 * does all of it, once per sensor frame, in two passes at the frame's own size:
 *
 *   develop: black level, lens shading, demosaic (Malvar-He-Cutler), white
 *            balance, colour matrix, saturation, sRGB and tone curve; for stills
 *            it also merges frames (an average for noise, or exposure fusion for
 *            HDR) with the result of the frame before;
 *   finish:  edge-aware noise reduction (luma and chroma) and sharpening, with
 *            strength by the sensor gain.
 *
 * The screen draws the finished texture, and a photo reads it back. The CPU reads
 * only a thin grid of pixels for metering, white balance and focus.
 *
 * Numbers from Samsung's stock firmware (see camera/README.md): black 64 and white
 * 1023 (libexynoscamera3.so), sensor orientation rear 90, front 270 (the same), rear
 * colour from its forward matrices; front colour from a matrix in setfile_3h1.bin.
 * Included by ztcamera.c, inside its GL frame.
 */
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define RAW_DEV		"/dev/zrn-cam"
#define RAW_TORCH	"/sys/class/leds/white:torch/brightness"
#define RAW_BUFS	4
#define RAW_FULL_AREA	(4032u * 2 * 3024)
#define RAW_POOL	(2 * (size_t)RAW_FULL_AREA)
#define RAW_BLACK	64
#define RAW_WHITE	1023
#define AE_TARGET	170.0		/* mean green above black, of 959: about 0.18 linear, mid grey */
#define AE_EVERY	4		/* frames: the sensor takes a new exposure two frames later */
#define AF_SETTLE	3		/* frames at a lens position before it is scored */
#define AF_STEP		40
#define AF_FINE		12
/* preview line times: 25456 and 1266 lines a frame at 30 fps (the vendor's mode tables) */
#define LINE_S_REAR	(1.0 / 30 / 25456)
#define LINE_S_FRONT	(1.0 / 30 / 1266)
/* gain code 32 is 1x. ISO 50 at 1x is inferred: stock's widest analog sensitivity
 * is 640 (rear) at the 16x the driver allows, which puts 1x at 40, and stock's app
 * starts at ISO 50. */
#define ISO_AT_1X	50.0
/* auto aperture, by the scene's EV100 at the lens: F2.4 above, F1.5 below */
#define AP_EV_UP	11.0
#define AP_EV_DOWN	9.0
#define AP_MIN_S	4.0		/* between automatic moves */
#define RAW_DMABUF	_IO('Z', 1)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

struct zrn_cam_frame {
	uint32_t seq, index, w, h, stride, bayer, errors, stopped, area;
};

enum raw_cam { RAW_REAR, RAW_FRONT, RAW_IR, RAW_N };
enum af_state { AF_IDLE, AF_WAIT_AE, AF_SWEEP, AF_REFINE, AF_HOLD };
enum raw_still { STILL_ONE, STILL_NR, STILL_HDR };
enum wb_preset { WB_AUTO, WB_CLOUDY, WB_DAYLIGHT, WB_FLUORESCENT, WB_INCANDESCENT };

static struct {
	int fd, dmabuf;
	void *map;
	enum raw_cam cam;
	uint32_t seq, drawn;
	struct zrn_cam_frame info;
	/* the frame as the GPU reads it */
	void *img[RAW_BUFS];
	unsigned tex[RAW_BUFS];
	float unpack[4];
	bool zero_copy;
	unsigned upload_tex;
	/* what the frame becomes: develop ping-pongs between two, finish writes the third */
	unsigned dev_tex[2], dev_fbo[2], fin_tex, fin_fbo;
	int cur;
	int out_w, out_h;
	int prog, u_size, u_unpack, u_first, u_gain, u_ccm, u_black, u_shade, u_sat, u_acc,
		u_mode, u_start;
	int fin, f_step, f_nr, f_sharp;
	int disp;
	/* the loops */
	float gain[3];
	float ccm[9];
	int coarse, again, coarse_max;
	bool f24;		/* the aperture: F2.4 asked for last, F1.5 otherwise */
	bool wb_set;
	int af_pos, af_best, af_wait, af_lo, af_hi;
	double af_score, af_held;
	enum af_state af;
	double af_low_since, af_asked;
	bool ae_settled;
	double scene_ev;	/* EV100 of the metered scene, at the lens */
	double luma;		/* the metered mean, of AE_TARGET */
	double clipped;		/* the weighted share of the grid at white */
	/* what the app asks for */
	double ev;		/* exposure compensation, stops */
	int man_iso;		/* 0 auto */
	double man_shutter;	/* seconds, 0 auto */
	enum wb_preset man_wb;
	int man_focus;		/* lens position, -1 auto */
	double spot_u, spot_v;	/* the tapped point in the frame, 0-1; spot_until 0 for none */
	double spot_until;
	bool ap_auto;
	double ap_moved;
} raw = { .fd = -1, .dmabuf = -1, .gain = { 1, 1, 1 }, .coarse = 8000, .again = 64,
	.man_focus = -1, .ap_auto = true };

static const char *const raw_stop_why[] = { "running", "stopped", "idle", "time limit",
	"too warm", "receiver errors", "failed" };

/* Forward matrices of libexynoscamera3.so, 2L3, into linear sRGB (D50 adapted):
 * illuminant A's and D65's. The rows sum to 1, as they should. */
static const float rear_fm_a[9] = { 1.602f, -0.598f, -0.007f, -0.145f, 1.331f, -0.184f, 0.040f, -0.723f, 1.684f };
static const float rear_fm_d[9] = { 1.546f, -0.498f, -0.046f, -0.244f, 1.424f, -0.181f, 0.035f, -1.115f, 2.081f };
/* setfile_3h1.bin, at 0x18137 and repeated, over 256: which illuminant is not labelled */
static const float front_ccm[9] = { 330 / 256.f, -80 / 256.f, 6 / 256.f, -72 / 256.f, 346 / 256.f, -18 / 256.f,
	-12 / 256.f, -119 / 256.f, 387 / 256.f };

/*
 * White balance presets as red and blue gains over green. Not from stock: these
 * are starting points for Bayer sensors of this kind (daylight near R 1.9 B 1.6,
 * tungsten near R 1.3 B 2.5), to be tuned against the owner's photos.
 */
static const float wb_preset_gain[5][2] = {
	{ 0, 0 }, { 2.05f, 1.45f }, { 1.90f, 1.60f }, { 1.65f, 2.05f }, { 1.30f, 2.50f },
};

static double raw_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double raw_line_s(void) {
	return raw.cam == RAW_FRONT ? LINE_S_FRONT : LINE_S_REAR;
}

static void raw_images_free(void) {
	static PFNEGLDESTROYIMAGEKHRPROC destroy;
	if (!destroy) {
		destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	}
	for (int i = 0; i < RAW_BUFS; i++) {
		if (raw.img[i] && destroy) {
			destroy(eglGetCurrentDisplay(), raw.img[i]);
		}
		raw.img[i] = NULL;
	}
}

static void raw_torch(int level) {
	static int was = -1;
	if (level == was) {
		return;
	}
	int fd = open(RAW_TORCH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	char b[8];
	int n = snprintf(b, sizeof(b), "%d", level);
	if (write(fd, b, n) == n) {
		was = level;
	}
	close(fd);
}

static bool raw_say(const char *cmd);

static void raw_close(void) {
	/* the iris holds where it was left: back to F1.5, stock's closing value, so the next open knows */
	if (raw.fd >= 0 && raw.cam == RAW_REAR && raw.f24 && raw_say("ap15")) {
		raw.f24 = false;
	}
	if (raw.fd >= 0) {
		raw_torch(0);	/* a torch never outlives its camera */
	}
	raw_images_free();
	if (raw.map) {
		munmap(raw.map, RAW_POOL);
		raw.map = NULL;
	}
	if (raw.dmabuf >= 0) {
		close(raw.dmabuf);
		raw.dmabuf = -1;
	}
	if (raw.fd >= 0) {
		close(raw.fd);	/* the driver powers the camera down */
		raw.fd = -1;
	}
}

static bool raw_say(const char *cmd) {
	return raw.fd >= 0 && write(raw.fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd);
}

static void raw_set_exposure(int c, int g) {
	char cmd[40];
	snprintf(cmd, sizeof(cmd), "exp %d %d", c, g);
	raw_say(cmd);
}

static void raw_set_focus(int pos) {
	char cmd[24];
	snprintf(cmd, sizeof(cmd), "af %d", pos);
	raw_say(cmd);
}

/* Focus waits for exposure: a dark frame's noise scores as sharp at any lens position. */
static void raw_focus_start(void) {
	if (raw.man_focus >= 0) {
		raw.af = AF_IDLE;
		raw_set_focus(raw.man_focus);
		return;
	}
	raw.af = AF_WAIT_AE;
	raw.af_asked = raw_now();
	raw.ae_settled = false;
}

static void raw_focus_sweep(void) {
	raw.af = AF_SWEEP;
	raw.af_pos = 0;
	raw.af_best = 0;
	raw.af_score = -1;
	raw.af_wait = AF_SETTLE;
	raw.af_lo = 0;
	raw.af_hi = 511;
	raw_set_focus(0);
}

/*
 * Opening a camera blocks: the driver powers the sensor up, asks the APM for
 * dvfs_cam and starts the stream inside the write, the rear taking longest. It
 * runs on a thread so the viewfinder can say "switching" meanwhile; the main
 * loop collects it with raw_open_poll(). Only the thread touches raw_job's
 * fd, map and info until state reads JOB_DONE.
 */
enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };
static struct {
	pthread_t th;
	int state;		/* __atomic */
	enum raw_cam cam;
	int fd, dmabuf, err, tries;
	void *map;
	struct zrn_cam_frame info;
	double started;
} raw_job = { .fd = -1, .dmabuf = -1 };

static void *raw_open_thread(void *arg) {
	(void)arg;
	const char *cmd = raw_job.cam == RAW_FRONT ? "front" : "rear";
	int fd = open(RAW_DEV, O_RDWR | O_CLOEXEC);
	raw_job.err = 0;
	raw_job.map = NULL;
	raw_job.dmabuf = -1;
	if (fd < 0 || write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		raw_job.err = errno ? errno : EIO;
	} else {
		void *m = mmap(NULL, RAW_POOL, PROT_READ, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) {
			raw_job.err = errno ? errno : EIO;
		} else {
			raw_job.map = m;
			if (read(fd, &raw_job.info, sizeof(raw_job.info)) != sizeof(raw_job.info) ||
					!raw_job.info.w || raw_job.info.stopped) {
				raw_job.err = EIO;
			} else {
				raw_job.dmabuf = ioctl(fd, RAW_DMABUF);
			}
		}
	}
	if (raw_job.err) {
		if (raw_job.map) {
			munmap(raw_job.map, RAW_POOL);
			raw_job.map = NULL;
		}
		if (fd >= 0) {
			close(fd);
		}
		fd = -1;
	}
	raw_job.fd = fd;
	__atomic_store_n(&raw_job.state, JOB_DONE, __ATOMIC_RELEASE);
	return NULL;
}

static bool raw_switching(void) {
	return __atomic_load_n(&raw_job.state, __ATOMIC_ACQUIRE) != JOB_IDLE;
}

/* Starts opening cam; false if it cannot even start (IR, no thread). */
static bool raw_open_begin(enum raw_cam cam) {
	if (raw_switching()) {
		return true;	/* one at a time: the one in flight wins */
	}
	raw_close();
	raw.cam = cam;
	raw.seq = 0;
	raw.drawn = 0;
	raw.info.seq = 0;
	if (cam == RAW_IR) {
		return false;
	}
	raw_job.cam = cam;
	raw_job.started = raw_now();
	__atomic_store_n(&raw_job.state, JOB_RUNNING, __ATOMIC_RELEASE);
	if (pthread_create(&raw_job.th, NULL, raw_open_thread, NULL) != 0) {
		__atomic_store_n(&raw_job.state, JOB_IDLE, __ATOMIC_RELEASE);
		return false;
	}
	return true;
}

static void raw_open_ready(enum raw_cam cam);

/*
 * 0 still opening (or nothing in flight), 1 the camera is up, -1 it failed.
 * A failed open is tried once more before the app gives up on it.
 */
static int raw_open_poll(void) {
	if (__atomic_load_n(&raw_job.state, __ATOMIC_ACQUIRE) != JOB_DONE) {
		return 0;
	}
	pthread_join(raw_job.th, NULL);
	__atomic_store_n(&raw_job.state, JOB_IDLE, __ATOMIC_RELEASE);
	const char *name = raw_job.cam == RAW_FRONT ? "front" : "rear";
	if (raw_job.fd < 0) {
		fprintf(stderr, "ztcamera: %s did not start (%s), attempt %d\n", name,
			strerror(raw_job.err), raw_job.tries + 1);
		if (raw_job.tries++ < 1 && raw_open_begin(raw_job.cam)) {
			return 0;
		}
		raw_job.tries = 0;
		return -1;
	}
	raw_job.tries = 0;
	fprintf(stderr, "ztcamera: %s up in %.2f s\n", name, raw_now() - raw_job.started);
	raw.fd = raw_job.fd;
	raw.map = raw_job.map;
	raw.dmabuf = raw_job.dmabuf;
	raw.info = raw_job.info;
	raw_job.fd = raw_job.dmabuf = -1;
	raw_job.map = NULL;
	if (raw.dmabuf < 0) {
		fprintf(stderr, "ztcamera: no DMA-BUF, the frames will be copied\n");
	}
	raw_open_ready(raw.cam);
	return 1;
}

/* What every open sets up once the stream runs. */
static void raw_open_ready(enum raw_cam cam) {
	/* preview's own frame length: 25456 and 1266 lines (the vendor's mode tables) */
	raw.coarse_max = cam == RAW_FRONT ? 1250 : 25000;
	raw.coarse = cam == RAW_FRONT ? 1000 : 8000;
	raw.again = 64;
	raw.wb_set = false;
	raw.gain[0] = raw.gain[1] = raw.gain[2] = 1;
	raw.spot_until = 0;
	memcpy(raw.ccm, cam == RAW_FRONT ? front_ccm : rear_fm_d, sizeof(raw.ccm));
	raw_set_exposure(raw.coarse, raw.again);
	raw_focus_start();
}

/* ---- exposure ------------------------------------------------------------- */

static int raw_iso(void) {
	return (int)(raw.again / 32.0 * ISO_AT_1X + 0.5);
}

static double raw_shutter_s(void) {
	return raw.coarse * raw_line_s();
}

/*
 * Exposure towards the target (with the compensation the app asks for), gain only
 * once exposure is at its limit. A manual ISO or shutter holds that half and
 * leaves the other automatic; both manual is fully manual.
 */
static void raw_expose(double green, double clipped) {
	double target = AE_TARGET * pow(2, raw.ev);
	if (clipped > 0.03 && raw.ev <= 0) {
		target *= 0.75;		/* hold the highlights */
	}
	target = fmin(target, 700);
	raw.luma = green / AE_TARGET;
	raw.clipped = clipped;
	double r = green > 1 ? target / green : 2;
	/* the scene's EV100: what the exposure was, and how far from mid grey it came out */
	double n = raw.cam == RAW_REAR ? (raw.f24 ? 2.4 : 1.5) : 1.7;
	double t = raw_shutter_s(), iso = raw.again / 32.0 * ISO_AT_1X;
	raw.scene_ev = log2(n * n / t) - log2(iso / 100) + log2(fmax(green, 1) / AE_TARGET);

	int g_man = raw.man_iso ? (int)fmin(512, fmax(32, raw.man_iso / ISO_AT_1X * 32)) : 0;
	int c_man = raw.man_shutter > 0 ? (int)fmin(raw.coarse_max, fmax(4, raw.man_shutter / raw_line_s())) : 0;
	if (g_man && c_man) {
		raw.ae_settled = true;
		if (g_man != raw.again || c_man != raw.coarse) {
			raw.again = g_man;
			raw.coarse = c_man;
			raw_set_exposure(c_man, g_man);
		}
		return;
	}
	if (r > 0.92 && r < 1.08 && (!g_man || g_man == raw.again) && (!c_man || c_man == raw.coarse)) {
		raw.ae_settled = true;
		return;
	}
	raw.ae_settled = false;
	r = fmin(1.5, fmax(0.6, r));
	double want = (double)raw.coarse * raw.again / 32 * r;
	int c, g;
	if (g_man) {
		g = g_man;
		c = (int)fmin(raw.coarse_max, fmax(4, want * 32 / g));
	} else if (c_man) {
		c = c_man;
		g = (int)fmin(512, fmax(32, want / c * 32));
	} else {
		c = (int)fmin(raw.coarse_max, fmax(4, want));
		g = (int)fmin(512, fmax(32, want / c * 32));
	}
	if (r > 1 && c == raw.coarse && g == raw.again) {
		raw.ae_settled = true;	/* as bright as it goes */
		return;
	}
	if (c != raw.coarse || g != raw.again) {
		raw.coarse = c;
		raw.again = g;
		raw_set_exposure(c, g);
	}
}

/* The aperture, rear only. The exposure follows at once: 1.36 stops between them. */
static bool raw_aperture(bool f24) {
	if (raw.cam != RAW_REAR || raw.f24 == f24) {
		return raw.cam == RAW_REAR;
	}
	if (!raw_say(f24 ? "ap24" : "ap15")) {
		return false;
	}
	raw.f24 = f24;
	raw.ap_moved = raw_now();
	double k = f24 ? 2.56 : 1 / 2.56;
	double want = (double)raw.coarse * raw.again / 32 * k;
	int c = (int)fmin(raw.coarse_max, fmax(4, want));
	int g = (int)fmin(512, fmax(32, want / c * 32));
	raw.coarse = c;
	raw.again = g;
	raw_set_exposure(c, g);
	return true;
}

/* Samsung's rule of thumb: bright scenes at F2.4 for depth and sharpness, dim ones at F1.5. */
static void raw_aperture_auto(void) {
	if (!raw.ap_auto || raw.cam != RAW_REAR || !raw.ae_settled || raw.man_iso || raw.man_shutter > 0) {
		return;
	}
	if (raw_now() - raw.ap_moved < AP_MIN_S) {
		return;
	}
	bool want = raw.f24 ? raw.scene_ev > AP_EV_DOWN : raw.scene_ev > AP_EV_UP;
	if (want != raw.f24) {
		if (!raw_aperture(want)) {
			/* the driver said no (its move limit): ask again in a minute */
			raw.ap_moved = raw_now() + 60 - AP_MIN_S;
			fprintf(stderr, "ztcamera: aperture move refused (%s)\n", strerror(errno));
		} else {
			note("aperture F%s (EV %.1f)", want ? "2.4" : "1.5", raw.scene_ev);
		}
	}
}

/* ---- statistics: metering, white balance ---------------------------------- */

/* How much a point of the frame counts: centre-weighted, or round a tapped spot. */
static double raw_meter_weight(double u, double v) {
	if (raw.spot_until > raw_now()) {
		double du = u - raw.spot_u, dv = v - raw.spot_v;
		return 0.4 + 8 * exp(-(du * du + dv * dv) / (2 * 0.09 * 0.09));
	}
	double du = u - 0.5, dv = v - 0.5;
	return 1 + 2 * exp(-(du * du + dv * dv) / (2 * 0.22 * 0.22));
}

static void raw_wb_from(double kr, double kb, double m) {
	kr = fmin(3.4, fmax(0.9, kr));
	kb = fmin(3.4, fmax(0.9, kb));
	raw.gain[0] += (kr - raw.gain[0]) * m;
	raw.gain[2] += (kb - raw.gain[2]) * m;
	raw.gain[1] = 1;
	raw.wb_set = true;
	if (raw.cam == RAW_REAR) {
		/* warm light has the larger red-to-blue ratio low: A's matrix there, D65's in daylight */
		double ratio = raw.gain[2] / raw.gain[0];	/* about 0.85 in daylight, 1.9 under tungsten */
		double t = fmin(1, fmax(0, (ratio - 0.9) / 1.0));
		for (int i = 0; i < 9; i++) {
			raw.ccm[i] = (float)(rear_fm_d[i] + (rear_fm_a[i] - rear_fm_d[i]) * t);
		}
	}
}

/*
 * A grid across the frame. Exposure from the weighted green mean. White balance
 * from three estimates: grey world over the whole frame; the grey pixels (those
 * near neutral under the current gains, which steadies on the real neutrals and
 * ignores a frame full of one colour); and the brightest unclipped patches (white
 * patch). Moved gently, and sane in the dark.
 */
static void raw_stats(const uint8_t *base) {
	int w = raw.info.w, h = raw.info.h, st = raw.info.stride;
	double sum[3] = { 0, 0, 0 }, n = 0, seen = 0, clip = 0;
	double grey[3] = { 0, 0, 0 }, ng = 0, top[3] = { 0, 0, 0 }, nt = 0, gmax = 0;
	int sx = (w / 48) & ~1, sy = (h / 40) & ~1;
	/* first pass: brightest unclipped green, for the white patch threshold */
	for (int y = sy / 2 & ~1; y < h - 1; y += sy) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)y * st);
		for (int x = sx / 2 & ~1; x < w - 1; x += sx) {
			if (a[x] < 980 && a[x] > gmax) {
				gmax = a[x];
			}
		}
	}
	gmax -= RAW_BLACK;
	for (int y = sy / 2 & ~1; y < h - 1; y += sy) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)y * st);
		const uint16_t *b = (const uint16_t *)(base + (size_t)(y + 1) * st);
		for (int x = sx / 2 & ~1; x < w - 1; x += sx) {
			int g1 = a[x], one = a[x + 1], other = b[x], g2 = b[x + 1];
			int ri = raw.info.bayer ? one : other, bi = raw.info.bayer ? other : one;
			double wt = raw_meter_weight((double)x / w, (double)y / h);
			seen += wt;
			if (g1 >= 980 || g2 >= 980 || ri >= 980 || bi >= 980) {
				clip += wt;
				continue;
			}
			double r = ri - RAW_BLACK, g = (g1 + g2) / 2.0 - RAW_BLACK, bl = bi - RAW_BLACK;
			sum[0] += wt * r;
			sum[1] += wt * g;
			sum[2] += wt * bl;
			n += wt;
			if (g < 8 || r < 2 || bl < 2) {
				continue;	/* noise, not colour */
			}
			double lr = log(r * raw.gain[0] / g), lb = log(bl * raw.gain[2] / g);
			if (lr * lr + lb * lb < 0.30 * 0.30) {
				grey[0] += r;
				grey[1] += g;
				grey[2] += bl;
				ng++;
			}
			if (g > gmax * 0.7) {
				top[0] += r;
				top[1] += g;
				top[2] += bl;
				nt++;
			}
		}
	}
	if (!n) {
		raw_expose(1e6, 1);
		return;
	}
	double r = sum[0] / n, g = sum[1] / n, bl = sum[2] / n;
	if (raw.af != AF_SWEEP && raw.af != AF_REFINE) {	/* a steady picture to score */
		raw_expose(g, clip / seen);
		raw_aperture_auto();
	}
	if (raw.man_wb != WB_AUTO) {
		raw_wb_from(wb_preset_gain[raw.man_wb][0], wb_preset_gain[raw.man_wb][1], 1);
		return;
	}
	if (g <= 6 || r <= 2 || bl <= 2) {
		return;
	}
	double kr = g / r, kb = g / bl;	/* grey world */
	int samples = (h / sy) * (w / sx);
	if (ng > samples * 0.04 && raw.wb_set) {
		/* grey pixels lead, the rest of the frame keeps them honest */
		kr = 0.75 * grey[1] / grey[0] + 0.25 * kr;
		kb = 0.75 * grey[1] / grey[2] + 0.25 * kb;
	}
	if (nt > 4 && top[0] > 0 && top[2] > 0) {
		kr = 0.7 * kr + 0.3 * top[1] / top[0];
		kb = 0.7 * kb + 0.3 * top[1] / top[2];
	}
	/* Samsung's look keeps a touch of the light's warmth rather than neutralising it */
	kr *= 0.98;
	kb *= 0.96;
	raw_wb_from(kr, kb, raw.wb_set ? 0.15 : 1);
}

/* Green differences round the focus point (the tapped spot or the middle), over its brightness. */
static double raw_sharpness(const uint8_t *base) {
	int w = raw.info.w, h = raw.info.h, st = raw.info.stride;
	double cu = 0.5, cv = 0.5;
	if (raw.spot_until > raw_now()) {
		cu = fmin(0.85, fmax(0.15, raw.spot_u));
		cv = fmin(0.85, fmax(0.15, raw.spot_v));
	}
	int x0 = (int)((cu - 0.125) * w) & ~1, x1 = (int)((cu + 0.125) * w);
	int y0 = (int)((cv - 0.125) * h) & ~1, y1 = (int)((cv + 0.125) * h);
	double d = 0, s = 1;
	for (int y = y0; y < y1; y += 8) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)y * st);
		for (int x = x0; x < x1 && x + 10 < w; x += 8) {
			/* pairs of green pixels against the next pair: less noise than one against one */
			int v = (int)a[x] + a[x + 2] - 2 * RAW_BLACK, u = (int)a[x + 4] + a[x + 6] - 2 * RAW_BLACK;
			int m = (int)a[x + 8] + a[x + 10] - 2 * RAW_BLACK;
			d += (v - u) * (v - u) + (u - m) * (u - m);
			s += v > 0 ? v : 0;
		}
	}
	return d / (s * s);
}

/* Sweep, then fine steps round the best, then stay until the picture softens. */
static void raw_focus(const uint8_t *base) {
	if (raw.af == AF_IDLE) {
		return;
	}
	if (raw.af == AF_WAIT_AE) {
		if (raw.ae_settled || raw_now() - raw.af_asked > 2) {
			raw_focus_sweep();
		}
		return;
	}
	if (raw.af == AF_HOLD) {
		if (raw.seq % 15 == 0) {
			double s = raw_sharpness(base), now = raw_now();
			if (s > raw.af_held) {
				raw.af_held = s;
			}
			if (s < raw.af_held * 0.6) {
				if (raw.af_low_since == 0) {
					raw.af_low_since = now;
				} else if (now - raw.af_low_since > 2) {
					raw.af_low_since = 0;
					raw_focus_start();
				}
			} else {
				raw.af_low_since = 0;
			}
		}
		return;
	}
	if (--raw.af_wait > 0) {
		return;
	}
	double s = raw_sharpness(base);
	if (s > raw.af_score) {
		raw.af_score = s;
		raw.af_best = raw.af_pos;
	}
	int step = raw.af == AF_SWEEP ? AF_STEP : AF_FINE;
	raw.af_pos += step;
	if (raw.af_pos > raw.af_hi) {
		if (raw.af == AF_SWEEP) {
			raw.af = AF_REFINE;
			raw.af_lo = raw.af_best - AF_STEP > 0 ? raw.af_best - AF_STEP : 0;
			raw.af_hi = raw.af_best + AF_STEP < 511 ? raw.af_best + AF_STEP : 511;
			raw.af_pos = raw.af_lo;
		} else {
			raw.af = AF_HOLD;
			raw.af_held = raw.af_score;
			raw.af_low_since = 0;
			raw_set_focus(raw.af_best);
			return;
		}
	}
	raw_set_focus(raw.af_pos);
	raw.af_wait = AF_SETTLE;
}

/* Tap to focus and expose: u, v in the frame, 0-1. Held for 8 seconds of metering. */
static void raw_focus_at(double u, double v) {
	raw.spot_u = fmin(1, fmax(0, u));
	raw.spot_v = fmin(1, fmax(0, v));
	raw.spot_until = raw_now() + 8;
	if (raw.man_focus < 0) {
		raw_focus_start();
	}
}

/* Newest frame's statistics; a stopped camera returns its reason. */
static const char *raw_poll(void) {
	if (raw.fd < 0 || read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info)) {
		return raw.fd < 0 ? NULL : "gone";
	}
	if (raw.info.stopped) {
		return raw_stop_why[raw.info.stopped < 7 ? raw.info.stopped : 6];
	}
	if (raw.info.seq == raw.seq || !raw.info.seq) {
		return NULL;
	}
	uint32_t was = raw.seq;
	raw.seq = raw.info.seq;
	const uint8_t *base = (const uint8_t *)raw.map + raw.info.index * (size_t)raw.info.area;
	if (raw.seq / AE_EVERY != was / AE_EVERY) {
		raw_stats(base);
	}
	raw_focus(base);
	return NULL;
}

/* ---- QR codes, through zbar if the phone has it ---------------------------- */

static struct {
	void *lib, *scanner;
	void *(*scanner_create)(void);
	int (*set_config)(void *, int, int, int);
	void *(*image_create)(void);
	void (*image_set_format)(void *, unsigned long);
	void (*image_set_size)(void *, unsigned, unsigned);
	void (*image_set_data)(void *, const void *, unsigned long, void (*)(void *));
	int (*scan)(void *, void *);
	const void *(*first)(const void *);
	const char *(*data)(const void *);
	void (*image_destroy)(void *);
	bool tried, busy, done;
	uint8_t *grey;
	int gw, gh;
	char found[256];
	double last;
} qr;

static bool qr_load(void) {
	if (qr.tried) {
		return qr.lib != NULL;
	}
	qr.tried = true;
	qr.lib = dlopen("libzbar.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!qr.lib) {
		return false;
	}
	qr.scanner_create = (void *(*)(void))dlsym(qr.lib, "zbar_image_scanner_create");
	qr.set_config = (int (*)(void *, int, int, int))dlsym(qr.lib, "zbar_image_scanner_set_config");
	qr.image_create = (void *(*)(void))dlsym(qr.lib, "zbar_image_create");
	qr.image_set_format = (void (*)(void *, unsigned long))dlsym(qr.lib, "zbar_image_set_format");
	qr.image_set_size = (void (*)(void *, unsigned, unsigned))dlsym(qr.lib, "zbar_image_set_size");
	qr.image_set_data = (void (*)(void *, const void *, unsigned long, void (*)(void *)))
		dlsym(qr.lib, "zbar_image_set_data");
	qr.scan = (int (*)(void *, void *))dlsym(qr.lib, "zbar_scan_image");
	qr.first = (const void *(*)(const void *))dlsym(qr.lib, "zbar_image_first_symbol");
	qr.data = (const char *(*)(const void *))dlsym(qr.lib, "zbar_symbol_get_data");
	qr.image_destroy = (void (*)(void *))dlsym(qr.lib, "zbar_image_destroy");
	if (!qr.scanner_create || !qr.set_config || !qr.image_create || !qr.image_set_format ||
			!qr.image_set_size || !qr.image_set_data || !qr.scan || !qr.first || !qr.data ||
			!qr.image_destroy) {
		dlclose(qr.lib);
		qr.lib = NULL;
		return false;
	}
	qr.scanner = qr.scanner_create();
	/* ZBAR_NONE, ZBAR_CFG_ENABLE: off for all, then ZBAR_QRCODE (64) on */
	qr.set_config(qr.scanner, 0, 0, 0);
	qr.set_config(qr.scanner, 64, 0, 1);
	return qr.scanner != NULL;
}

static void *qr_thread(void *arg) {
	(void)arg;
	void *im = qr.image_create();
	qr.image_set_format(im, FOURCC('Y', '8', '0', '0'));
	qr.image_set_size(im, qr.gw, qr.gh);
	qr.image_set_data(im, qr.grey, (unsigned long)qr.gw * qr.gh, NULL);
	qr.found[0] = 0;
	if (qr.scan(qr.scanner, im) > 0) {
		const void *s = qr.first(im);
		if (s && qr.data(s)) {
			snprintf(qr.found, sizeof(qr.found), "%s", qr.data(s));
		}
	}
	qr.image_destroy(im);
	__atomic_store_n(&qr.done, true, __ATOMIC_RELEASE);
	return NULL;
}

/* Green at half size, from the newest frame, to a thread; its answer next time. NULL until one is read. */
static const char *qr_step(void) {
	static char said[256];
	if (qr.busy) {
		if (!__atomic_load_n(&qr.done, __ATOMIC_ACQUIRE)) {
			return NULL;
		}
		qr.busy = false;
		if (qr.found[0] && strcmp(qr.found, said)) {
			snprintf(said, sizeof(said), "%s", qr.found);
			return said;
		}
	}
	if (raw.fd < 0 || !raw.seq || raw_now() - qr.last < 0.4 || !qr_load()) {
		return NULL;
	}
	qr.last = raw_now();
	int w = raw.info.w / 2, h = raw.info.h / 2;
	if (w != qr.gw || h != qr.gh) {
		free(qr.grey);
		qr.grey = malloc((size_t)w * h);
		qr.gw = w;
		qr.gh = h;
	}
	if (!qr.grey) {
		return NULL;
	}
	const uint8_t *base = (const uint8_t *)raw.map + raw.info.index * (size_t)raw.info.area;
	for (int y = 0; y < h; y++) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)(2 * y) * raw.info.stride);
		uint8_t *o = qr.grey + (size_t)y * w;
		/* green sits at x 0 of even rows in both orders (GB/RG, GR/BG) */
		for (int x = 0; x < w; x++) {
			int v = (a[2 * x] - RAW_BLACK) * 255 / 400;
			o[x] = v < 0 ? 0 : v > 255 ? 255 : v;
		}
	}
	qr.done = false;
	qr.busy = true;
	pthread_t t;
	if (pthread_create(&t, NULL, qr_thread, NULL) != 0) {
		qr.busy = false;
	} else {
		pthread_detach(t);
	}
	return NULL;
}

/* ---- the GPU pipeline ------------------------------------------------------- */

static GLuint raw_shader(GLenum type, const char *text) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[800] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "ztcamera: shader: %s\n", log);
	}
	return s;
}

static GLuint raw_program(const char *vs, const char *fs) {
	GLuint p = glCreateProgram();
	glAttachShader(p, raw_shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, raw_shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "position");
	glBindAttribLocation(p, 1, "texcoord");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	return ok ? p : 0;
}

static bool raw_gl(void) {
	if (raw.prog) {
		return raw.prog > 0;
	}
	static const char vs[] =
		"attribute vec2 position; attribute vec2 texcoord; varying vec2 uv;"
		"void main(){ uv=texcoord; gl_Position=vec4(position,0.0,1.0); }";
	/*
	 * Malvar, He and Cutler's 5x5 filters. first: where in the 2x2 cell red is.
	 * mode 0: the developed frame. 1: averaged with acc (per-pixel count in alpha,
	 * over 8), a pixel that moved left out. 2: exposure fusion with acc (weight sum
	 * in alpha, over 4), each pixel weighted by how well exposed it is.
	 * start: 1 for the first frame of a merge, which ignores acc.
	 */
	static const char fs[] =
		"precision highp float; varying vec2 uv; uniform sampler2D bayer; uniform sampler2D acc;"
		"uniform vec2 size; uniform vec4 unpack; uniform vec2 first; uniform vec3 gain;"
		"uniform mat3 ccm; uniform float black; uniform vec3 shade; uniform float sat;"
		"uniform float mode; uniform float start;"
		"float px(float x,float y){ return dot(texture2D(bayer,uv+vec2(x,y)/size),unpack); }"
		"void main(){"
		" float c=px(0.,0.);"
		" float n1=px(0.,-1.),s1=px(0.,1.),w1=px(-1.,0.),e1=px(1.,0.);"
		" float n2=px(0.,-2.),s2=px(0.,2.),w2=px(-2.,0.),e2=px(2.,0.);"
		" float d=px(-1.,-1.)+px(1.,-1.)+px(-1.,1.)+px(1.,1.);"
		" vec2 p=mod(floor(uv*size)+first,2.);"	/* 0,0 is red; 1,1 is blue */
		" float gc=(4.*c+2.*(n1+s1+w1+e1)-(n2+s2+w2+e2))/8.;"
		" float opp=(6.*c+2.*d-1.5*(n2+s2+w2+e2))/8.;"
		" float rowh=(5.*c+4.*(w1+e1)-(w2+e2)-d+.5*(n2+s2))/8.;"	/* same colour left and right */
		" float rowv=(5.*c+4.*(n1+s1)-(n2+s2)-d+.5*(w2+e2))/8.;"
		" vec3 v;"
		" if(p.x<.5&&p.y<.5) v=vec3(c,gc,opp);"
		" else if(p.x>.5&&p.y>.5) v=vec3(opp,gc,c);"
		" else if(p.y<.5) v=vec3(rowh,c,rowv);"		/* green on red's row */
		" else v=vec3(rowv,c,rowh);"			/* green on blue's row */
		" v=(v-black)/(1.-black);"
		/* lens shading: radial, a little more for red, whose falloff is widest */
		" vec2 q=uv-.5; float r2=dot(q,q);"
		" v*=1.+r2*shade*(1.+r2*shade.g);"
		" v=clamp(v*gain,0.,1.);"
		" v=clamp(ccm*v,0.,1.);"
		" float l=dot(v,vec3(.2126,.7152,.0722));"
		" v=clamp(mix(vec3(l),v,sat),0.,1.);"
		" v=mix(v*12.92,1.055*pow(v,vec3(.41667))-.055,step(.0031308,v));"
		/* tone: lifted shadows, an S through the middle, highlights rolled off */
		" v=v+.10*v*(1.-v)*(1.-v);"
		" v=mix(v,v*v*(3.-2.*v),.30);"
		" if(mode<.5){ gl_FragColor=vec4(v,1.); return; }"
		" vec4 a=texture2D(acc,uv);"
		" if(mode<1.5){"
		"  float n=start>.5?0.:a.a*8.;"
		"  float keep=n>0.&&abs(dot(v-a.rgb,vec3(.3,.5,.2)))>.10?0.:1.;"
		"  gl_FragColor=vec4((a.rgb*n+v*keep)/max(n+keep,1.),(n+keep)/8.);"
		" } else {"
		"  float y=dot(v,vec3(.299,.587,.114));"
		"  float wt=exp(-(y-.5)*(y-.5)*12.5)*(.2+length(v-vec3(y)))+.002;"
		"  float W=start>.5?0.:a.a*4.;"
		"  gl_FragColor=vec4((a.rgb*W+v*wt)/(W+wt),min(1.,(W+wt)/4.));"
		" } }";
	/*
	 * Finish: a 3x3 bilateral on luma and chroma (chroma smoothed harder), then an
	 * unsharp mask on luma with a coring threshold so noise is not sharpened.
	 * nr: the bilateral's luma sigma, larger with gain; sharp: the mask's amount.
	 */
	static const char ff[] =
		"precision highp float; varying vec2 uv; uniform sampler2D frame;"
		"uniform vec2 step1; uniform float nr; uniform float sharp;"
		"void main(){"
		"  vec3 c=texture2D(frame,uv).rgb;"
		"  float yc=dot(c,vec3(.299,.587,.114));"
		"  vec3 cs=vec3(0.); float ys=0.,wsum=0.,yb=0.;"
		"  for(int j=-1;j<=1;j++) for(int i=-1;i<=1;i++){"
		"    vec3 s=texture2D(frame,uv+vec2(float(i),float(j))*step1).rgb;"
		"    float y=dot(s,vec3(.299,.587,.114));"
		"    float w=exp(-(y-yc)*(y-yc)/(2.*nr*nr));"
		"    cs+=(s-y)*w; ys+=y*w; wsum+=w; yb+=y; }"
		"  vec3 chroma=cs/wsum; float yn=mix(yc,ys/wsum,.6);"
		"  float dt=yc-yb/9.; dt=sign(dt)*max(abs(dt)-nr*.35,0.);"
		"  float y=yn+sharp*dt;"
		"  gl_FragColor=vec4(clamp(vec3(y)+chroma,0.,1.),1.); }";
	static const char fd[] =
		"precision mediump float; varying vec2 uv; uniform sampler2D frame;"
		"void main(){ gl_FragColor=vec4(texture2D(frame,uv).rgb,1.); }";
	GLuint p = raw_program(vs, fs), f = raw_program(vs, ff), d = raw_program(vs, fd);
	if (!p || !f || !d) {
		raw.prog = -1;
		return false;
	}
	raw.prog = p;
	raw.u_size = glGetUniformLocation(p, "size");
	raw.u_unpack = glGetUniformLocation(p, "unpack");
	raw.u_first = glGetUniformLocation(p, "first");
	raw.u_gain = glGetUniformLocation(p, "gain");
	raw.u_ccm = glGetUniformLocation(p, "ccm");
	raw.u_black = glGetUniformLocation(p, "black");
	raw.u_shade = glGetUniformLocation(p, "shade");
	raw.u_sat = glGetUniformLocation(p, "sat");
	raw.u_acc = glGetUniformLocation(p, "acc");
	raw.u_mode = glGetUniformLocation(p, "mode");
	raw.u_start = glGetUniformLocation(p, "start");
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "bayer"), 0);
	glUniform1i(raw.u_acc, 1);
	raw.fin = f;
	raw.f_step = glGetUniformLocation(f, "step1");
	raw.f_nr = glGetUniformLocation(f, "nr");
	raw.f_sharp = glGetUniformLocation(f, "sharp");
	raw.disp = d;
	return true;
}

static void raw_tex_params(GLenum filter) {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* This frame buffer as a texture with no copy, once; 16-bit red, or two 8-bit channels. */
static bool raw_import(unsigned i) {
	static PFNEGLCREATEIMAGEKHRPROC create;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target;
	if (raw.img[i]) {
		return true;
	}
	if (!create) {
		create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	}
	if (raw.dmabuf < 0 || !create || !target) {
		return false;
	}
	static const struct { uint32_t fourcc; float unpack[4]; } formats[] = {
		{ FOURCC('R', '1', '6', ' '), { 65535.f / 1023, 0, 0, 0 } },
		{ FOURCC('G', 'R', '8', '8'), { 255.f / 1023, 65280.f / 1023, 0, 0 } },
	};
	for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		EGLint at[] = {
			EGL_WIDTH, (EGLint)raw.info.w, EGL_HEIGHT, (EGLint)raw.info.h,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)formats[f].fourcc,
			EGL_DMA_BUF_PLANE0_FD_EXT, raw.dmabuf,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)(i * raw.info.area),
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)raw.info.stride,
			EGL_NONE,
		};
		EGLImageKHR im = create(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
		if (im == EGL_NO_IMAGE_KHR) {
			continue;
		}
		if (!raw.tex[i]) {
			GLuint t;
			glGenTextures(1, &t);
			raw.tex[i] = t;
		}
		glBindTexture(GL_TEXTURE_2D, raw.tex[i]);
		raw_tex_params(GL_NEAREST);
		while (glGetError() != GL_NO_ERROR) {
		}
		target(GL_TEXTURE_2D, im);
		if (glGetError() != GL_NO_ERROR) {
			continue;
		}
		raw.img[i] = im;
		fprintf(stderr, "ztcamera: frame %u read directly as %.4s\n", i, (const char *)&formats[f].fourcc);
		memcpy(raw.unpack, formats[f].unpack, sizeof(raw.unpack));
		raw.zero_copy = true;
		return true;
	}
	return false;
}

/* The fallback if the driver cannot import it: a copy, low byte and high byte. */
static void raw_upload(const uint8_t *base) {
	static int tw, th;
	if (!raw.upload_tex) {
		GLuint t;
		glGenTextures(1, &t);
		raw.upload_tex = t;
		glBindTexture(GL_TEXTURE_2D, t);
		raw_tex_params(GL_NEAREST);
	}
	glBindTexture(GL_TEXTURE_2D, raw.upload_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
	if (tw != (int)raw.info.w || th != (int)raw.info.h) {
		tw = raw.info.w;
		th = raw.info.h;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, tw, th, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, base);
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, base);
	}
	raw.unpack[0] = 255.f / 1023;
	raw.unpack[1] = raw.unpack[2] = 0;
	raw.unpack[3] = 65280.f / 1023;
	raw.zero_copy = false;
}

static void raw_target(unsigned *tex, unsigned *fbo, int w, int h) {
	if (!*tex) {
		GLuint t;
		glGenTextures(1, &t);
		*tex = t;
	}
	if (!*fbo) {
		GLuint f;
		glGenFramebuffers(1, &f);
		*fbo = f;
	}
	glBindTexture(GL_TEXTURE_2D, *tex);
	raw_tex_params(GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
}

static void raw_quad(void) {
	static const float q[16] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/*
 * The newest frame through develop, into the other of the two develop targets
 * (merging with the one before when mode is not 0), then finish. Restores the
 * caller's frame buffer, viewport and blend state.
 */
static void raw_develop_ex(int mode, bool start, bool finish) {
	unsigned i = raw.info.index;
	const uint8_t *base = (const uint8_t *)raw.map + i * (size_t)raw.info.area;
	int w = raw.info.w, h = raw.info.h;
	GLint old_fbo = 0, vp[4], held = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &held);
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	if (raw.out_w != w || raw.out_h != h) {
		raw_target(&raw.dev_tex[0], &raw.dev_fbo[0], w, h);
		raw_target(&raw.dev_tex[1], &raw.dev_fbo[1], w, h);
		raw_target(&raw.fin_tex, &raw.fin_fbo, w, h);
		raw.out_w = w;
		raw.out_h = h;
	}
	if (mode < 0) {
		goto finish;	/* only the finishing pass, over what is merged so far */
	}
	int dst = mode ? 1 - raw.cur : raw.cur;
	/* the accumulation only while merging: never the texture being drawn into */
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, mode ? raw.dev_tex[raw.cur] : 0);
	glActiveTexture(GL_TEXTURE0);
	if (i < RAW_BUFS && raw_import(i)) {
		glBindTexture(GL_TEXTURE_2D, raw.tex[i]);
	} else {
		raw_upload(base);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, raw.dev_fbo[dst]);
	glViewport(0, 0, w, h);
	glUseProgram(raw.prog);
	glUniform2f(raw.u_size, (float)w, (float)h);
	glUniform4fv(raw.u_unpack, 1, raw.unpack);
	/* rear GB/RG: red at x 0, y 1. Front GR/BG: red at x 1, y 0. */
	if (raw.info.bayer) {
		glUniform2f(raw.u_first, 1, 0);
	} else {
		glUniform2f(raw.u_first, 0, 1);
	}
	/* no gain below 1, or a clipped white would come out tinted */
	float low = fmin(fmin(raw.gain[0], raw.gain[1]), raw.gain[2]);
	glUniform3f(raw.u_gain, raw.gain[0] / low, raw.gain[1] / low, raw.gain[2] / low);
	const float m[9] = { raw.ccm[0], raw.ccm[3], raw.ccm[6], raw.ccm[1], raw.ccm[4], raw.ccm[7],
		raw.ccm[2], raw.ccm[5], raw.ccm[8] };	/* GL is column first */
	glUniformMatrix3fv(raw.u_ccm, 1, GL_FALSE, m);
	glUniform1f(raw.u_black, RAW_BLACK / (float)RAW_WHITE);
	/* no calibration read from the module's memory: a fall-off for this lens size, red widest.
	 * x: red, y: green (also the quadratic term), z: blue */
	if (raw.cam == RAW_FRONT) {
		glUniform3f(raw.u_shade, 1.35f, 1.2f, 1.2f);
	} else {
		glUniform3f(raw.u_shade, 1.6f, 1.4f, 1.45f);
	}
	/* Samsung's colour is a little richer than the matrices alone; less in the dark, where it is noise */
	glUniform1f(raw.u_sat, raw.again > 256 ? 1.05f : 1.18f);
	glUniform1f(raw.u_mode, (float)mode);
	glUniform1f(raw.u_start, start ? 1.f : 0.f);
	raw_quad();
	raw.cur = dst;
finish:
	if (finish) {
		glBindFramebuffer(GL_FRAMEBUFFER, raw.fin_fbo);
		glUseProgram(raw.fin);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, raw.dev_tex[raw.cur]);
		glUniform2f(raw.f_step, 1.0f / w, 1.0f / h);
		/* noise grows with gain: smooth more, sharpen less; merged stills need less */
		double gx = raw.again / 32.0;
		float nr = (float)fmin(0.12, 0.018 + 0.010 * gx);
		if (mode < 0) {
			nr *= 0.6f;	/* a merged still: the average took most of it already */
		}
		glUniform1f(raw.f_nr, nr);
		glUniform1f(raw.f_sharp, (float)fmax(0.35, 1.1 - 0.06 * gx));
		raw_quad();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, held);
	if (blend) {
		glEnable(GL_BLEND);
	}
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
}

static void raw_develop(void) {
	raw_develop_ex(0, true, true);
}

/* Screen point (0-1 across and down) to the frame's u, v: raw_draw's mapping backwards. */
static void raw_screen_to_frame(double sx, double sy, double *u, double *v) {
	double span = fmin(1, ((double)zmc.width / zmc.height) * ((double)raw.info.w / raw.info.h));
	double v0 = 0.5 - span / 2, v1 = 0.5 + span / 2;
	bool front = raw.cam == RAW_FRONT;
	double top = front ? 1 : 0, bottom = front ? 0 : 1;
	*u = top + sy * (bottom - top);
	*v = v1 - sx * (v1 - v0);
}

/*
 * Over the whole window, cropped to fill. The sensors lie on their sides, by
 * stock's orientation: rear 90, front 270, so the front's picture is the rear's
 * turned half round. The front is then mirrored like a mirror, as stock previews it;
 * that leaves its v the same as the rear's and its u reversed.
 * The core's own program and buffers come back afterwards; every core draw sets
 * its texture and vertices again.
 */
static void raw_draw(void) {
	if (!raw.seq || !raw_gl()) {
		return;
	}
	GLint old = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &old);
	if (raw.drawn != raw.seq || raw.out_w != (int)raw.info.w) {
		raw_develop();
		raw.drawn = raw.seq;
	}
	glUseProgram(raw.disp);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, raw.fin_tex);
	double span = fmin(1, ((double)zmc.width / zmc.height) * ((double)raw.info.w / raw.info.h));
	float v0 = 0.5f - span / 2, v1 = 0.5f + span / 2;
	bool front = raw.cam == RAW_FRONT;
	float top = front ? 1 : 0, bottom = front ? 0 : 1;
	/* position x,y, then the frame's u,v */
	const float q[16] = {
		-1,  1, top, v1,
		 1,  1, top, v0,
		-1, -1, bottom, v1,
		 1, -1, bottom, v0,
	};
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(old);
}

/* ---- stills ------------------------------------------------------------------ */

/* Waits for a frame at least `after` past seq `from`; false if the camera stopped or 3 s passed. */
static bool raw_wait_frame(uint32_t from, uint32_t after) {
	double until = raw_now() + 3;
	while (raw_now() < until) {
		if (read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info) || raw.info.stopped) {
			return false;
		}
		if (raw.info.seq >= from + after) {
			return true;
		}
		struct timespec nap = { 0, 8000000 };
		nanosleep(&nap, NULL);
	}
	return false;
}

/*
 * A photo: the sensor at full size for a moment, developed by the same shaders,
 * upright, the front mirrored as in the viewfinder. Falls back to the preview's
 * size if the full mode gives no frame. Returns a new surface or NULL.
 *
 * STILL_NR averages four frames on the GPU (a pixel that moved between them is
 * left out), which takes most of the noise out of a dim room. STILL_HDR brackets
 * three exposures (as metered, 2 stops under, 1.5 over) and fuses them, each
 * pixel weighted by how well exposed and how colourful it is in each.
 */
static cairo_surface_t *raw_capture(enum raw_still kind) {
	if (!raw_gl() || raw.fd < 0) {
		return NULL;
	}
	bool front = raw.cam == RAW_FRONT;
	int c = raw.coarse, g = raw.again, focus = raw.man_focus >= 0 ? raw.man_focus : raw.af_best;
	bool full = raw_say(front ? "frontfull" : "rearfull");
	raw_images_free();
	/* the same time in lines: preview's line length over the full mode's */
	double k = full ? (front ? (double)0x2970 / 0x13D0 : (double)0x0528 / 0x0A50) : 1;
	int cf = (int)(c * k);
	if (cf < 2) {
		cf = 2;
	}
	if (full) {
		(void)read(raw.fd, &raw.info, sizeof(raw.info));
		raw_set_exposure(cf, g);
		raw_set_focus(focus);
		full = raw_wait_frame(raw.info.seq, 5);
		if (!full) {
			fprintf(stderr, "ztcamera: no full-size frame (stopped %u, errors %u)\n",
				raw.info.stopped, raw.info.errors);
		}
	}
	if (!full) {	/* back to preview, then its next frame */
		raw_say(front ? "front" : "rear");
		raw_images_free();
		cf = c;
		(void)read(raw.fd, &raw.info, sizeof(raw.info));
		raw_set_exposure(c, g);
		raw_set_focus(focus);
		if (!raw_wait_frame(raw.info.seq, 3)) {
			return NULL;
		}
	}
	if (kind == STILL_ONE) {
		raw_develop_ex(0, true, true);
	} else if (kind == STILL_NR) {
		for (int f = 0; f < 4; f++) {
			if (f && !raw_wait_frame(raw.info.seq, 1)) {
				break;
			}
			raw_develop_ex(1, f == 0, false);
		}
		raw_develop_ex(-1, false, true);
	} else {
		/* metered, under, over: shorter lines for under, more gain (then lines) for over */
		static const double stops[3] = { 0, -2, 1.5 };
		for (int f = 0; f < 3; f++) {
			double mul = pow(2, stops[f]);
			int ce = cf, ge = g;
			if (mul < 1) {
				ce = (int)fmax(2, cf * mul);
			} else if (mul > 1) {
				ge = (int)fmin(512, g * mul);
				ce = (int)(cf * mul * g / ge);
			}
			if (f) {
				raw_set_exposure(ce, ge);
				if (!raw_wait_frame(raw.info.seq, 3)) {
					break;
				}
			}
			raw_develop_ex(2, f == 0, false);
		}
		raw_develop_ex(-1, false, true);
	}
	int w = raw.info.w, h = raw.info.h;
	uint8_t *px = malloc((size_t)w * h * 4);
	cairo_surface_t *sf = px ? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, h, w) : NULL;
	if (px && cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS) {
		GLint old_fbo = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, raw.fin_fbo);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
		uint8_t *d = cairo_image_surface_get_data(sf);
		int st = cairo_image_surface_get_stride(sf);
		cairo_surface_flush(sf);
		/* the same mapping as raw_draw: screen row y is the frame's u, screen column x its v */
		for (int y = 0; y < w; y++) {
			uint32_t *row = (uint32_t *)(d + (size_t)y * st);
			int u = front ? w - 1 - y : y;
			for (int x = 0; x < h; x++) {
				const uint8_t *s = px + ((size_t)(h - 1 - x) * w + u) * 4;
				row[x] = 0xff000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
			}
		}
		cairo_surface_mark_dirty(sf);
	} else if (sf) {
		cairo_surface_destroy(sf);
		sf = NULL;
	}
	free(px);
	/* back to the viewfinder */
	if (full) {
		raw_say(front ? "front" : "rear");
		raw_images_free();
	}
	raw.seq = 0;
	raw.drawn = 0;
	if (read(raw.fd, &raw.info, sizeof(raw.info)) == sizeof(raw.info)) {
		raw_set_exposure(c, g);
		raw_set_focus(focus);
		raw.coarse = c;
		raw.again = g;
	}
	return sf;
}
