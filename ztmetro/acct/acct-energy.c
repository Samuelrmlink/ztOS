// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd's energy model, Android's power_profile done on this phone's own
 * counters, and checked against the battery gauge.
 *
 * Every scan, in joules:
 *
 *   CPU	per cluster (policy0: 4 x Cortex-A55, policy4: 4 x Exynos M3),
 *		the cores' busy time from /proc/stat, at the frequencies cpufreq's
 *		time_in_state says the cluster ran at in that time (weighted by
 *		frequency: schedutil runs fast when busy), times a busy core's power
 *		at that frequency. Each process gets its CPU seconds times the joules
 *		per busy second of the cluster it was last seen on; interrupt time
 *		goes to the kernel; processes gone before the scan get the mean.
 *   screen	while it is on: a fixed part plus a part in proportion to the
 *		backlight, to the app in front (ztMETRO at Start).
 *   GPU	devfreq's time above its lowest frequency, to the app in front.
 *   network	Wi-Fi: per packet and per byte, to the socket's process;
 *		associated and idle, "Wi-Fi idle". Mobile: the radio's active time
 *		(packets, then a tail), shared by bytes; idle, "Mobile radio idle".
 *   base	everything else awake ("Idle (awake)": this kernel has no cpuidle
 *		states, the SoC idles hot) and asleep ("Deep sleep", CLOCK_BOOTTIME
 *		minus CLOCK_MONOTONIC).
 *
 * Every 5 minutes (and before a save) the modelled joules are checked
 * against the gauge: the coulomb counter (charge_counter, uAh) times the
 * voltage. On battery, if the model says more than was used, every app's
 * share is scaled down to fit; if less, what is left is "Unaccounted",
 * shown as such. On the charger the gauge cannot see the phone's own use:
 * the model stands as it is. The windows the gauge saw are kept, with the
 * model's inputs, in /var/lib/ztacct/calib.csv for fitting the coefficients.
 *
 *   accessories  what the phone powers beyond itself, by state read from
 *		sysfs on the scan (no wakes of their own):
 *		"USB-C accessories": the MAX77705's boost giving VBUS to a sink
 *		partner (its regulator max77705-otg enabled), and while it does,
 *		the USB host bus with devices on it (xHCI, the PHY, the dock's
 *		hubs and devices: /sys/bus/usb/devices, per device).
 *		"Desktop (external display)": the DisplayPort link powered (the
 *		driver's debugfs state, read at most every 5 minutes) and video
 *		out (the DRM connector card*-DP-* enabled).
 *		"Bluetooth": the controller up (HCIGETDEVINFO) and its links
 *		(/sys/class/bluetooth/hci0:N). "Mobile radio (airplane)": the
 *		modem's CP online (mif modem_state) with the radio off. Both
 *		are inside the measured base (they were on when it was taken):
 *		their share is moved out of "Idle (awake)", not added.
 *   floor	"Always-on (unseparated)": what the gauge shows, window after
 *		window, beyond the measured base with nothing we read changing
 *		(fitted; see base_extra_mw).
 *
 * Coefficients: /etc/ztacct/power_profile (KEY VALUE lines) overrides the
 * defaults below. Measured on this phone: the base (325 mA, screen off, dark,
 * radio off) and Wi-Fi idle (17 mA tuned, screen off; 82 mA untuned, which
 * is what zrnperfd leaves on with the screen on), power/README.md. The rest
 * fitted to the gauge (fit-calib.py on calib.csv, 2026-11-11: 109 windows,
 * the old ones' accessory states rebuilt from the kernel's log), or
 * estimates where marked.
 */
#include "acctd.h"
#include "acct-common.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define SETTLE_S	300
#define MAX_CLUSTERS	4
#define MAX_FREQS	32
#define MAX_CPUS	16
#define CALIB_KEEP	96

/* ------------------------------------------------------------ profile */

struct ptab { int n; double khz[MAX_FREQS], mw[MAX_FREQS]; };

/* mW per busy core, with what a busy core pulls along (L3, memory, the bus).
 * The little cores: fitted to the gauge on 2026-11-08 by fit-calib.py, two
 * cores busy compiling with the screen off at 1.15 GHz: 430 mW each (4.5 x
 * the core-only estimate); the other clocks scaled alike until bench-cpu.sh
 * (overnight) measures them. The big cores: estimates (AnandTech's Exynos
 * 9810 figures), not yet measured. */
static const struct ptab little_default = { 5,
	{ 349916, 650000, 1150500, 1499333, 1794000 },
	{ 110, 200, 430, 680, 1000 } };
static const struct ptab big_default = { 6,
	{ 400000, 1066000, 1469000, 1794000, 2327000, 2704000 },
	{ 150, 420, 700, 1050, 1900, 2900 } };
static const struct ptab gpu_default = { 3,
	{ 260000, 455000, 572000 },	/* kHz */
	{ 0, 600, 1000 } };

static struct {
	double base_awake_mw;		/* awake, screen off, idle, radio off */
	double base_suspend_mw;
	double screen_mw, screen_full_mw;	/* screen on: fixed, plus at full backlight */
	double wifi_idle_on_mw, wifi_idle_off_mw;
	double wifi_pkt_mj, wifi_byte_uj;
	double mobile_idle_mw, mobile_active_mw, mobile_tail_s, mobile_pkt_s;
	double wake_mj;			/* each resume from deep sleep */
	double cpu_scale;		/* the CPU tables times this */
	double gpu_scale;		/* the GPU table times this */
	double base_extra_mw;		/* the awake floor beyond the measured base */
	double otg_mw, usb_host_mw, usb_dev_mw;	/* VBUS out; the bus with devices; each device */
	double dp_link_mw, dp_video_mw;
	double bt_on_mw, bt_conn_mw;	/* bt_on: inside the base */
	double cp_air_mw;		/* the modem's CP, radio off: inside the base */
	double wake_ctxt_uj, wake_irq_uj;	/* each context switch, each interrupt (fitted; 0: not yet) */
	struct ptab cl[MAX_CLUSTERS], gpu;
	bool cl_set[MAX_CLUSTERS];
} prof = {
	.base_awake_mw = 1190, .base_suspend_mw = 100,	/* 325 mA x 3.67 V; suspend: an estimate */
	/* screen: fitted 2026-11-11 (was 250 + 1100, estimates): ztMETRO is
	 * black on an AMOLED panel, and the floor below now takes what the
	 * screen-on windows used to blame on it */
	.screen_mw = 80, .screen_full_mw = 400,
	/* Wi-Fi associated, screen off: 65 mW on the 11-08 bench (5 GHz, quiet
	 * LAN), 300 mW in daily use on 11-11 (2.4 GHz at -70 dBm, 3-9 frames/s
	 * each keeping the radio up 200 ms): radio off 76 mA below the baselines
	 * either side, power/always-on/README.md. The daily figure is used */
	.wifi_idle_on_mw = 315, .wifi_idle_off_mw = 300,
	.wifi_pkt_mj = 0.07, .wifi_byte_uj = 0.1,	/* per byte: a 1.2 GB download, 2026-11-08; per packet: 2026-11-11 */
	.mobile_idle_mw = 40, .mobile_active_mw = 900, .mobile_tail_s = 5, .mobile_pkt_s = 0.002,
	.wake_mj = 300,
	/* the CPU tables x 0.78, the GPU table x 2.5: fitted 2026-11-11 */
	.cpu_scale = 0.78, .gpu_scale = 2.5,
	/* fit-calib.py, 2026-11-11, 109 windows (calib.csv since 11-08, states
	 * of the old rows from the kernel's log). The floor: the base fitted to
	 * 1673 mW against the 1190 measured (Wi-Fi off, panel asleep, 150 s on
	 * the bench); the gauge agrees with current_now to 1 % (checked
	 * 2026-11-11), so the difference is real draw that no state we read
	 * tells apart (bench vs daily use: see README.md). Refitted 2026-11-11
	 * (138 windows) with Wi-Fi idle at its measured 300 mW and the
	 * accessories held: 365 (was 480; the rest was Wi-Fi) */
	.base_extra_mw = 365,
	/* the Belkin 6-in-1 dock on the phone's VBUS (hub, hub, RTL8153
	 * Ethernet without a cable, card reader, billboard, HDMI converter,
	 * keyboard/mouse dongle): 1.5-1.6 W in all, the gauge's step when it was
	 * pulled (11-11 00:48: 3.55 W -> 1.95 W, screen off). The boost alone, a
	 * dock that never enumerated (11-09): 90 mW. Bus vs per device rests on
	 * one device more on 11-11 than on 11-10: the sum is solid, the split is
	 * not */
	.otg_mw = 90, .usb_host_mw = 470, .usb_dev_mw = 140,
	/* the DP link left up, PMA/PHY/controller (the driver keeps it so after
	 * unplugging, until a reboot): fitted, 11-09 and 11-11 windows with and
	 * without it. Video out (DECON2 scanout + the link's video at 1080p60):
	 * an estimate, never on long enough on battery to fit */
	.dp_link_mw = 220, .dp_video_mw = 300,
	/* estimates: the BCM4361's Bluetooth up and idle (inside the base: on
	 * when it was measured), each link in sniff mode; the CP online in
	 * airplane mode (CFUN=4, mif traffic every ~20 s; inside the base) */
	.bt_on_mw = 10, .bt_conn_mw = 20,
	.cp_air_mw = 25,
};

static void parse_list(const char *s, double *out, int *n) {
	*n = 0;
	char *end;
	while (*n < MAX_FREQS) {
		double v = strtod(s, &end);
		if (end == s) break;
		out[(*n)++] = v;
		s = end;
		while (*s == ',' || *s == ' ') s++;
	}
}

static void profile_load(void) {
	FILE *f = fopen(ACCT_CONF, "re");
	char line[512], key[64], val[400];
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || sscanf(line, "%63s %399[^\n]", key, val) != 2) continue;
		double v = atof(val);
		struct { const char *k; double *p; } keys[] = {
			{ "base_awake_mw", &prof.base_awake_mw }, { "base_suspend_mw", &prof.base_suspend_mw },
			{ "screen_mw", &prof.screen_mw }, { "screen_full_mw", &prof.screen_full_mw },
			{ "wifi_idle_on_mw", &prof.wifi_idle_on_mw }, { "wifi_idle_off_mw", &prof.wifi_idle_off_mw },
			{ "wifi_pkt_mj", &prof.wifi_pkt_mj }, { "wifi_byte_uj", &prof.wifi_byte_uj },
			{ "mobile_idle_mw", &prof.mobile_idle_mw }, { "mobile_active_mw", &prof.mobile_active_mw },
			{ "mobile_tail_s", &prof.mobile_tail_s }, { "mobile_pkt_s", &prof.mobile_pkt_s },
			{ "wake_mj", &prof.wake_mj }, { "cpu_scale", &prof.cpu_scale },
			{ "base_extra_mw", &prof.base_extra_mw }, { "otg_mw", &prof.otg_mw },
			{ "usb_host_mw", &prof.usb_host_mw }, { "usb_dev_mw", &prof.usb_dev_mw },
			{ "dp_link_mw", &prof.dp_link_mw }, { "dp_video_mw", &prof.dp_video_mw },
			{ "bt_on_mw", &prof.bt_on_mw }, { "bt_conn_mw", &prof.bt_conn_mw },
			{ "cp_air_mw", &prof.cp_air_mw },
			{ "wake_ctxt_uj", &prof.wake_ctxt_uj }, { "wake_irq_uj", &prof.wake_irq_uj },
		};
		bool done = false;
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			if (!strcmp(key, keys[i].k)) {
				*keys[i].p = v;
				done = true;
			}
		}
		int c;
		if (!done && sscanf(key, "cluster%d.khz", &c) == 1 && c >= 0 && c < MAX_CLUSTERS) {
			parse_list(val, prof.cl[c].khz, &prof.cl[c].n);
			prof.cl_set[c] = true;
		} else if (!done && sscanf(key, "cluster%d.mw", &c) == 1 && c >= 0 && c < MAX_CLUSTERS) {
			int n;
			parse_list(val, prof.cl[c].mw, &n);
		} else if (!done && !strcmp(key, "gpu.khz")) {
			parse_list(val, prof.gpu.khz, &prof.gpu.n);
		} else if (!done && !strcmp(key, "gpu.mw")) {
			int n;
			parse_list(val, prof.gpu.mw, &n);
		}
	}
	fclose(f);
}

static double ptab_mw(const struct ptab *t, double khz) {
	if (t->n == 0) return 0;
	if (khz <= t->khz[0]) return t->mw[0] * khz / t->khz[0];
	for (int i = 1; i < t->n; i++) {
		if (khz <= t->khz[i]) {
			double a = (khz - t->khz[i - 1]) / (t->khz[i] - t->khz[i - 1]);
			return t->mw[i - 1] + a * (t->mw[i] - t->mw[i - 1]);
		}
	}
	return t->mw[t->n - 1] * khz / t->khz[t->n - 1];
}

/* ------------------------------------------------------- the hardware */

static struct cluster {
	char path[128];		/* .../cpufreq/policyN */
	int cpus[MAX_CPUS], n_cpus;
	int nf;
	double khz[MAX_FREQS];
	unsigned long long tis[MAX_FREQS];	/* time_in_state, 10 ms */
	struct ptab *tab;
	double busy_s, e_j, eps;	/* this scan: busy core seconds, joules, joules per busy second */
	double busy_total, e_total;
} cl[MAX_CLUSTERS];
static int n_cl;
static int cpu_cluster[MAX_CPUS];
static unsigned long long cpu_busy[MAX_CPUS], cpu_irq[MAX_CPUS];
static unsigned long long gpu_ms[MAX_FREQS];
static double gpu_khz[MAX_FREQS];
static int gpu_nf;
static char gpu_path[160];
static long long charge_prev = -1;
static double volt_prev;
static unsigned long resumes_prev;
static char resume_path[96];

static int g_idle = -1, g_sleep = -1, g_wifi = -1, g_mobile = -1, g_unacc = -1, g_kernel = -1, g_shell = -1, g_gauge = -1;
static int g_usbc = -1, g_display = -1, g_bt = -1, g_cp = -1, g_floor = -1, g_wake = -1;

static long long read_ll(const char *path) {
	char buf[64];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	return atoll(buf);
}

static void read_tis(struct cluster *c, unsigned long long *out, bool init) {
	char path[160];
	snprintf(path, sizeof(path), "%s/stats/time_in_state", c->path);
	FILE *f = fopen(path, "re");
	if (!f) return;
	double khz;
	unsigned long long t;
	int i = 0;
	while (i < MAX_FREQS && fscanf(f, "%lf %llu", &khz, &t) == 2) {
		if (init) c->khz[i] = khz;
		out[i++] = t;
	}
	if (init) c->nf = i;
	fclose(f);
}

static void read_gpu(unsigned long long *out, bool init) {
	if (!gpu_path[0]) return;
	FILE *f = fopen(gpu_path, "re");
	char line[512];
	int i = 0;
	if (!f) return;
	while (fgets(line, sizeof(line), f) && i < MAX_FREQS) {
		char *c = strchr(line, ':');
		if (!c || !strstr(line, "0:")) continue;
		char *s = line;
		while (*s == ' ' || *s == '*') s++;
		double hz = atof(s);
		if (hz <= 0) continue;
		/* the last number on the line is the time, ms */
		char *last = line + strlen(line);
		while (last > c && (last[-1] == '\n' || last[-1] == ' ')) last--;
		while (last > c && last[-1] != ' ') last--;
		if (init) gpu_khz[i] = hz / 1000;
		out[i++] = strtoull(last, NULL, 10);
	}
	if (init) gpu_nf = i;
	fclose(f);
}

/* the system's wakeups, from the same read: context switches and interrupts
 * since boot (/proc/stat "ctxt", and the total that opens "intr") */
static unsigned long long stat_ctxt, stat_intr;

static void read_cpus(unsigned long long *busy, unsigned long long *irq) {
	FILE *f = fopen("/proc/stat", "re");
	char line[512];
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		int c;
		/* "intr" is longer than the buffer: the rest of it comes back as
		 * lines of digits, which match nothing below */
		if (!strncmp(line, "intr ", 5)) stat_intr = strtoull(line + 5, NULL, 10);
		else if (!strncmp(line, "ctxt ", 5)) stat_ctxt = strtoull(line + 5, NULL, 10);
		unsigned long long u, n, s, id, io, ir, so, st;
		if (strncmp(line, "cpu", 3) || line[3] == ' ') continue;
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &n, &s, &id, &io, &ir, &so, &st) < 8)
			continue;
		if (c < 0 || c >= MAX_CPUS) continue;
		busy[c] = u + n + s + ir + so + st;
		irq[c] = ir + so;
	}
	fclose(f);
}

static double brightness(void) {
	long long b = read_ll("/sys/class/backlight/s9p-panel/brightness");
	long long m = read_ll("/sys/class/backlight/s9p-panel/max_brightness");
	if (b < 0 || m <= 0) {
		DIR *d = opendir("/sys/class/backlight");
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char p[160];
			snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
			b = read_ll(p);
			snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
			m = read_ll(p);
			break;
		}
		if (d) closedir(d);
	}
	return b >= 0 && m > 0 ? (double)b / m : 0.5;
}

static bool wifi_associated(void) {
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	bool up = false;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "wl", 2)) continue;
		char p[160], s[32] = "";
		snprintf(p, sizeof(p), "/sys/class/net/%s/operstate", e->d_name);
		FILE *f = fopen(p, "re");
		if (f) {
			if (fgets(s, sizeof(s), f) && !strncmp(s, "up", 2)) up = true;
			fclose(f);
		}
	}
	if (d) closedir(d);
	return up;
}

static bool mobile_up(void) {
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	bool up = false;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "rmnet", 5)) continue;
		char p[160];
		snprintf(p, sizeof(p), "/sys/class/net/%s/flags", e->d_name);
		long long fl = -1;
		FILE *f = fopen(p, "re");
		if (f) {
			if (fscanf(f, "%lli", &fl) != 1) fl = -1;
			fclose(f);
		}
		if (fl > 0 && (fl & 1)) {
			snprintf(p, sizeof(p), "/sys/class/net/%s/carrier", e->d_name);
			if (read_ll(p) == 1) up = true;
		}
	}
	if (d) closedir(d);
	return up;
}

static unsigned long resumes(void) {
	if (!resume_path[0]) return 0;
	FILE *f = fopen(resume_path, "re");
	char line[128];
	unsigned long n = 0;
	if (!f) return 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "success:", 8)) n = strtoul(line + 8, NULL, 10);
		else if (strspn(line, "0123456789") == strlen(line) - 1) n = strtoul(line, NULL, 10);
	}
	fclose(f);
	return n;
}

/* ------------------------------------------------------ the accessories */

/* What the phone powers beyond itself, read on the scan from sysfs: no
 * wakes, a few small files. Paths are found at start (and looked for again
 * at a settle if missing: the DRM connector appears with the first hot
 * plug). */
static struct acc {
	char otg[96];		/* the boost regulator's state file */
	char dp_on[96];		/* card*-DP-* "enabled" */
	char modem[128];	/* mif modem_state */
	int bt_fd;		/* HCI control socket, for HCIGETDEVINFO */
	bool dp_link;		/* the last debugfs read */
	bool partner_was;	/* a USB-C partner at the last read */
	/* this scan */
	bool vbus, dp_video, bt_up, cp_on, airplane, waydroid;
	int usb_n, usb_active, bt_links;
} acc = { .bt_fd = -1 };

static bool read_word(const char *path, char *out, size_t size) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	ssize_t n = read(fd, out, size - 1);
	close(fd);
	if (n <= 0) return false;
	out[n] = 0;
	out[strcspn(out, "\n")] = 0;
	return true;
}

static void acc_find(void) {
	DIR *d;
	struct dirent *e;
	if (!acc.otg[0] && (d = opendir("/sys/class/regulator"))) {
		while ((e = readdir(d))) {
			char p[96], name[64];
			if (e->d_name[0] == '.') continue;
			snprintf(p, sizeof(p), "/sys/class/regulator/%s/name", e->d_name);
			if (read_word(p, name, sizeof(name)) && strstr(name, "otg")) {
				snprintf(acc.otg, sizeof(acc.otg), "/sys/class/regulator/%s/state", e->d_name);
				break;
			}
		}
		closedir(d);
	}
	if (!acc.dp_on[0] && (d = opendir("/sys/class/drm"))) {
		while ((e = readdir(d))) {
			if (!strncmp(e->d_name, "card", 4) && strstr(e->d_name, "-DP-")) {
				snprintf(acc.dp_on, sizeof(acc.dp_on), "/sys/class/drm/%s/enabled", e->d_name);
				break;
			}
		}
		closedir(d);
	}
	if (!acc.modem[0] && (d = opendir("/sys/devices/platform"))) {
		while ((e = readdir(d))) {
			char p[128];
			if (!strstr(e->d_name, "mif")) continue;
			snprintf(p, sizeof(p), "/sys/devices/platform/%s/modem_state", e->d_name);
			if (access(p, R_OK) == 0) {
				snprintf(acc.modem, sizeof(acc.modem), "%s", p);
				break;
			}
		}
		closedir(d);
	}
	if (acc.bt_fd < 0) acc.bt_fd = socket(31 /* AF_BLUETOOTH */, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
}

/* Whether the DP link is up is in the driver's debugfs state only. Reading it
 * touches PMU registers and, with the link's power domain off, logs a
 * warning; so it is read at a settle (every 5 minutes), and only while a
 * USB-C partner is there, or the last read found the link on, or the partner
 * just left: after the link goes off, one read and at most one warning. */
static void acc_dp_link(void) {
	bool partner = access("/sys/class/typec/port0-partner", F_OK) == 0;
	if (!partner && !acc.dp_link && !acc.partner_was) return;
	acc.partner_was = partner;
	FILE *f = fopen("/sys/kernel/debug/exynos9810-dp/state", "re");
	char line[256];
	bool on = false;
	while (f && fgets(line, sizeof(line), f)) {
		/* "id 1 pma 1 phy 1 ctrl 1; PHY not isolated; link powered and
		 * clocked": the combo PHY's DP part, the PHY and the link controller
		 * up. ("powered and clocked" is the power domain: on at every boot) */
		int id, pma, phy, ctrl;
		if (sscanf(line, "id %d pma %d phy %d ctrl %d", &id, &pma, &phy, &ctrl) == 4) {
			on = pma || phy || ctrl;
			break;
		}
	}
	if (f) fclose(f);
	acc.dp_link = on;
}

static void acc_read(void) {
	char s[64];
	/* VBUS out: the boost on; without its regulator, the port's power role */
	if (acc.otg[0]) acc.vbus = read_word(acc.otg, s, sizeof(s)) && !strcmp(s, "enabled");
	else acc.vbus = read_word("/sys/class/typec/port0/power_role", s, sizeof(s)) && strstr(s, "[source]");
	/* devices on the USB host bus (none in device mode: the controller is unbound) */
	acc.usb_n = acc.usb_active = 0;
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		/* "1-1", "1-1.4": devices; "usb1": root hubs; "1-1:1.0": interfaces */
		if (!isdigit((unsigned char)e->d_name[0]) || strchr(e->d_name, ':')) continue;
		char p[96];
		acc.usb_n++;
		snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/power/runtime_status", e->d_name);
		if (!read_word(p, s, sizeof(s)) || strcmp(s, "suspended")) acc.usb_active++;
	}
	if (d) closedir(d);
	acc.dp_video = acc.dp_on[0] && read_word(acc.dp_on, s, sizeof(s)) && !strcmp(s, "enabled");
	/* Bluetooth: hci0 up, and its links (each an hci0:HANDLE device) */
	acc.bt_up = false;
	if (acc.bt_fd >= 0) {
		/* struct hci_dev_info: dev_id u16, name[8], bdaddr[6], then flags u32 */
		unsigned char di[128] = { 0 };
		if (ioctl(acc.bt_fd, _IOR('H', 211, int), di) == 0) {
			uint32_t flags;
			memcpy(&flags, di + 16, 4);
			acc.bt_up = flags & 1;	/* HCI_UP */
		}
	}
	acc.bt_links = 0;
	if (acc.bt_up && (d = opendir("/sys/class/bluetooth"))) {
		while ((e = readdir(d))) if (!strncmp(e->d_name, "hci0:", 5)) acc.bt_links++;
		closedir(d);
	}
	acc.cp_on = acc.modem[0] && read_word(acc.modem, s, sizeof(s)) && !strcmp(s, "ONLINE");
	acc.airplane = false;
	FILE *f = fopen("/run/ztoscorenetworking/status", "re");
	if (f) {
		char line[96];
		while (fgets(line, sizeof(line), f)) if (!strncmp(line, "airplane=1", 10)) acc.airplane = true;
		fclose(f);
	}
	acc.waydroid = access("/sys/fs/cgroup/lxc.payload.waydroid", F_OK) == 0;
}

/* ------------------------------------------------------------- the model */

/* what the scan modelled, before settling, for the window and calib.csv */
static struct win {
	double t0, dt_boot, dt_mono, screen_s, bright_s, susp_s;
	double busy_s[MAX_CLUSTERS], cpu_j[MAX_CLUSTERS];
	double gpu_j, wifi_pkts, wifi_bytes, mob_bytes, wifi_assoc_s, mob_active_s;
	double model_dis_j, model_chg_j, meas_j, dis_s, chg_s, volt_sum, volt_n, resumes;
	/* the accessories' states, seconds (usb_dev_s, usb_active_s, bt_conn_s: x count) */
	double otg_s, usb_host_s, usb_dev_s, usb_active_s, dp_link_s, dp_video_s, bt_on_s, bt_conn_s;
	double cp_s, airplane_s, waydroid_s;
	double ctxt, irqs;	/* context switches and interrupts in the window */
	double e[NEK];
} w;
static unsigned long long ctxt_prev, intr_prev;
static double k_last = 1, unacc_total;
static char calib[CALIB_KEEP][448];
static int n_calib;
static double front_since = -1, mob_until;
static double mob_bytes_tick;

static void model(int g, struct proc *p, int kind, double j) {
	if (g < 0 || g >= n_groups || !(j > 0)) return;
	groups[g].pend[kind] += j;
	if (p) p->pend[kind] += j;
	w.e[kind] += j;
	if (now.charging) w.model_chg_j += j;
	else w.model_dis_j += j;
}

void energy_screen_change(void) {
	if (front_since >= 0 && now.screen_on) {
		double dt = now.boot_s - front_since;
		if (dt > 0) {
			struct tally d = { .fg_s = dt };
			group_add(now.front_g, &d);
			double b = brightness();
			model(now.front_g, NULL, E_SCREEN, (prof.screen_mw + prof.screen_full_mw * b) * dt / 1000);
			w.screen_s += dt;
			w.bright_s += b * dt;
		}
	}
	front_since = now.boot_s;
}

void energy_add_net(int g, struct proc *p, int cls, double bytes, double packets) {
	if (cls == NET_WIFI) {
		model(g, p, E_NET, packets * prof.wifi_pkt_mj / 1000 + bytes * prof.wifi_byte_uj / 1e6);
		w.wifi_pkts += packets;
		w.wifi_bytes += bytes;
	} else if (cls == NET_MOBILE) {
		/* the radio's active time is shared by bytes at the end of the scan:
		 * keep who moved them for now, as joules per byte to come */
		w.mob_bytes += bytes;
		mob_bytes_tick += bytes;
		double active = packets * prof.mobile_pkt_s;
		mob_until = fmax(mob_until, now.mono_s) + active;
		/* per byte at the active power over the scan's mobile bytes is not known
		 * yet: charge it at an estimated 1 s per MB, corrected below */
		model(g, p, E_NET, (active + bytes / 1e6) * prof.mobile_active_mw / 1000);
	}
}

static void calib_upgrade(void);

void energy_start(void) {
	profile_load();
	calib_upgrade();
	g_idle = group_get("Idle (awake)", "idle", K_MODEL);
	g_sleep = group_get("Deep sleep", "suspend", K_MODEL);
	g_wifi = group_get("Wi-Fi idle", "wifi", K_MODEL);
	g_mobile = group_get("Mobile radio idle", "mobile", K_MODEL);
	g_unacc = group_get("Unaccounted", "unaccounted", K_MODEL);
	g_kernel = group_get("Kernel", "kernel", K_KERNEL);
	g_shell = group_get("ztMETRO", "ztmetro", K_SHELL);
	g_gauge = group_get("__gauge", "gauge", K_GAUGE);
	g_usbc = group_get("USB-C accessories", "usbc", K_MODEL);
	g_display = group_get("Desktop (external display)", "display", K_MODEL);
	g_bt = group_get("Bluetooth", "bluetooth", K_MODEL);
	g_cp = group_get("Mobile radio (airplane)", "modem", K_MODEL);
	g_floor = group_get("Always-on (unseparated)", "floor", K_MODEL);
	g_wake = group_get("Wakeups (timers, interrupts)", "wakeups", K_MODEL);
	acc_find();
	acc.partner_was = true;		/* read the DP link at the first settle */
	/* the clusters */
	DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
	struct dirent *e;
	char names[MAX_CLUSTERS][32];
	int n = 0;
	while (d && (e = readdir(d)) && n < MAX_CLUSTERS) {
		if (!strncmp(e->d_name, "policy", 6)) snprintf(names[n++], 32, "%s", e->d_name);
	}
	if (d) closedir(d);
	/* in order of their first CPU */
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (atoi(names[j] + 6) < atoi(names[i] + 6)) {
				char t[32];
				memcpy(t, names[i], 32);
				memcpy(names[i], names[j], 32);
				memcpy(names[j], t, 32);
			}
	for (int i = 0; i < MAX_CPUS; i++) cpu_cluster[i] = 0;
	for (int i = 0; i < n; i++) {
		struct cluster *c = &cl[n_cl];
		snprintf(c->path, sizeof(c->path), "/sys/devices/system/cpu/cpufreq/%s", names[i]);
		char p[200], buf[128] = "";
		snprintf(p, sizeof(p), "%s/related_cpus", c->path);
		FILE *f = fopen(p, "re");
		if (f) {
			if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
			fclose(f);
		}
		for (char *s = buf, *end; *s && c->n_cpus < MAX_CPUS; s = end) {
			long cpu = strtol(s, &end, 10);
			if (end == s) break;
			if (cpu >= 0 && cpu < MAX_CPUS) {
				c->cpus[c->n_cpus++] = (int)cpu;
				cpu_cluster[cpu] = n_cl;
			}
		}
		read_tis(c, c->tis, true);
		double top = c->nf ? c->khz[0] : 0;
		for (int k = 0; k < c->nf; k++) top = fmax(top, c->khz[k]);
		if (prof.cl_set[n_cl]) c->tab = &prof.cl[n_cl];
		else {
			prof.cl[n_cl] = top <= 2000000 ? little_default : big_default;
			c->tab = &prof.cl[n_cl];
		}
		n_cl++;
	}
	read_cpus(cpu_busy, cpu_irq);
	ctxt_prev = stat_ctxt;
	intr_prev = stat_intr;
	/* the GPU's devfreq */
	d = opendir("/sys/class/devfreq");
	while (d && (e = readdir(d))) {
		if (strstr(e->d_name, "gpu")) {
			snprintf(gpu_path, sizeof(gpu_path), "/sys/class/devfreq/%s/trans_stat", e->d_name);
			break;
		}
	}
	if (d) closedir(d);
	if (!prof.gpu.n) prof.gpu = gpu_default;
	read_gpu(gpu_ms, true);
	if (access("/sys/power/suspend_stats/success", R_OK) == 0)
		snprintf(resume_path, sizeof(resume_path), "/sys/power/suspend_stats/success");
	else if (access("/sys/kernel/debug/suspend_stats", R_OK) == 0)
		snprintf(resume_path, sizeof(resume_path), "/sys/kernel/debug/suspend_stats");
	resumes_prev = resumes();
	charge_prev = read_ll("/sys/class/power_supply/max170xx_battery/charge_counter");
	volt_prev = read_ll("/sys/class/power_supply/max170xx_battery/voltage_now") / 1e6;
	w.t0 = now.wall;
}

void energy_tick(double dt_boot, double dt_mono) {
	energy_screen_change();
	if (dt_boot <= 0) return;
	if (dt_mono > dt_boot) dt_mono = dt_boot;
	double susp = dt_boot - dt_mono;
	w.dt_boot += dt_boot;
	w.dt_mono += dt_mono;
	w.susp_s += susp;
	/* CPU: busy core seconds per cluster, and their joules */
	unsigned long long busy[MAX_CPUS] = { 0 }, irq[MAX_CPUS] = { 0 };
	read_cpus(busy, irq);
	/* wakeups: each costs the SoC more than its CPU time says (the cores and
	 * the uncore leave WFI and come back); fitted per wakeup, wake_*_uj */
	double d_ctxt = stat_ctxt >= ctxt_prev ? (double)(stat_ctxt - ctxt_prev) : 0;
	double d_intr = stat_intr >= intr_prev ? (double)(stat_intr - intr_prev) : 0;
	ctxt_prev = stat_ctxt;
	intr_prev = stat_intr;
	w.ctxt += d_ctxt;
	w.irqs += d_intr;
	model(g_wake, NULL, E_BASE, (d_ctxt * prof.wake_ctxt_uj + d_intr * prof.wake_irq_uj) / 1e6);
	double hz = tick_ticks(), irq_s[MAX_CLUSTERS] = { 0 };
	for (int i = 0; i < n_cl; i++) cl[i].busy_s = 0;
	for (int c = 0; c < MAX_CPUS; c++) {
		/* zrnperfd takes big cores offline and back: an offline CPU has no line,
		 * and one back online is counted from the next scan */
		if (!busy[c] || !cpu_busy[c] || busy[c] < cpu_busy[c] || irq[c] < cpu_irq[c]) {
			cpu_busy[c] = busy[c];
			cpu_irq[c] = irq[c];
			continue;
		}
		cl[cpu_cluster[c]].busy_s += (busy[c] - cpu_busy[c]) / hz;
		irq_s[cpu_cluster[c]] += (irq[c] - cpu_irq[c]) / hz;
		cpu_busy[c] = busy[c];
		cpu_irq[c] = irq[c];
	}
	double all_e = 0, all_busy = 0;
	for (int i = 0; i < n_cl; i++) {
		struct cluster *c = &cl[i];
		unsigned long long t[MAX_FREQS] = { 0 };
		read_tis(c, t, false);
		double num = 0, den = 0;
		for (int k = 0; k < c->nf; k++) {
			double dt = (t[k] >= c->tis[k] ? t[k] - c->tis[k] : 0) / 100.0;
			num += dt * c->khz[k] * ptab_mw(c->tab, c->khz[k]);
			den += dt * c->khz[k];
			c->tis[k] = t[k];
		}
		double mw_per_core = den > 0 ? num / den : (c->nf ? ptab_mw(c->tab, c->khz[c->nf - 1]) : 0);
		c->eps = mw_per_core * prof.cpu_scale / 1000;	/* joules per busy second */
		c->e_j = c->busy_s * c->eps;
		c->busy_total += c->busy_s;
		c->e_total += c->e_j;
		w.busy_s[i] += c->busy_s;
		w.cpu_j[i] += prof.cpu_scale > 0 ? c->e_j / prof.cpu_scale : c->e_j;	/* at scale 1, for fitting */
		all_e += c->e_j;
		all_busy += c->busy_s;
		model(g_kernel, NULL, E_CPU, irq_s[i] * c->eps);
	}
	double eps_mean = all_busy > 0 ? all_e / all_busy : (n_cl ? cl[0].eps : 0);
	for (int i = 0; i < n_procs; i++) {
		struct proc *p = &procs[i];
		if (p->d_cpu <= 0) continue;
		int c = p->cpu >= 0 && p->cpu < MAX_CPUS ? cpu_cluster[p->cpu] : 0;
		model(p->g, p, E_CPU, p->d_cpu * cl[c].eps);
		p->d_cpu = 0;
	}
	for (int g = 0; g < n_groups; g++) {
		if (groups[g].tick_cpu > 0) model(g, NULL, E_CPU, groups[g].tick_cpu * eps_mean);
		groups[g].tick_cpu = 0;
	}
	/* GPU: time above its lowest frequency, to the app in front */
	unsigned long long gt[MAX_FREQS] = { 0 };
	read_gpu(gt, false);
	double gj = 0;
	for (int k = 0; k < gpu_nf; k++) {
		double dt = (gt[k] >= gpu_ms[k] ? gt[k] - gpu_ms[k] : 0) / 1000.0;
		gj += dt * ptab_mw(&prof.gpu, gpu_khz[k]) / 1000;
		gpu_ms[k] = gt[k];
	}
	w.gpu_j += gj;		/* at scale 1, for fitting */
	gj *= prof.gpu_scale;
	model(now.screen_on ? now.front_g : g_shell, NULL, E_GPU, gj);
	/* the radios when idle */
	if (wifi_associated()) {
		double mw = now.screen_on ? prof.wifi_idle_on_mw : prof.wifi_idle_off_mw;
		model(g_wifi, NULL, E_NET, mw * dt_boot / 1000);
		w.wifi_assoc_s += dt_boot;
	}
	if (mobile_up()) model(g_mobile, NULL, E_NET, prof.mobile_idle_mw * dt_boot / 1000);
	if (mob_bytes_tick > 0) {
		/* a tail after the last packets */
		model(g_mobile, NULL, E_NET, prof.mobile_tail_s * prof.mobile_active_mw / 1000);
		w.mob_active_s += prof.mobile_tail_s;
		mob_bytes_tick = 0;
	}
	/* the accessories: what the phone powers beyond itself */
	acc_read();
	double base_mw = prof.base_awake_mw;
	if (acc.vbus) {
		/* the boost, and the bus it powers (the dock's hubs and devices; the
		 * xHCI and PHY on our side) */
		double mw = prof.otg_mw;
		if (acc.usb_n > 0) mw += prof.usb_host_mw + prof.usb_dev_mw * acc.usb_n;
		model(g_usbc, NULL, E_BASE, mw * dt_boot / 1000);
	}
	w.otg_s += acc.vbus ? dt_boot : 0;
	w.usb_host_s += acc.usb_n > 0 ? dt_boot : 0;
	w.usb_dev_s += acc.usb_n * dt_boot;
	w.usb_active_s += acc.usb_active * dt_boot;
	if (acc.dp_link || acc.dp_video)
		model(g_display, NULL, E_BASE, ((acc.dp_link ? prof.dp_link_mw : 0) + (acc.dp_video ? prof.dp_video_mw : 0)) * dt_boot / 1000);
	w.dp_link_s += acc.dp_link ? dt_boot : 0;
	w.dp_video_s += acc.dp_video ? dt_boot : 0;
	/* the radios that were on when the base was measured: their share of it */
	if (acc.bt_up) {
		double own = fmin(prof.bt_on_mw, base_mw);
		base_mw -= own;
		model(g_bt, NULL, E_BASE, (own + prof.bt_conn_mw * acc.bt_links) * dt_mono / 1000);
	}
	w.bt_on_s += acc.bt_up ? dt_boot : 0;
	w.bt_conn_s += acc.bt_links * dt_boot;
	if (acc.cp_on && !mobile_up()) {
		double own = fmin(prof.cp_air_mw, base_mw);
		base_mw -= own;
		model(g_cp, NULL, E_BASE, own * dt_mono / 1000);
	}
	w.cp_s += acc.cp_on ? dt_boot : 0;
	w.airplane_s += acc.airplane ? dt_boot : 0;
	w.waydroid_s += acc.waydroid ? dt_boot : 0;
	/* the rest: awake and idle, asleep, and the wakes */
	model(g_idle, NULL, E_BASE, base_mw * dt_mono / 1000);
	model(g_floor, NULL, E_BASE, prof.base_extra_mw * dt_mono / 1000);
	model(g_sleep, NULL, E_BASE, prof.base_suspend_mw * susp / 1000);
	unsigned long r = resumes();
	if (r > resumes_prev) {
		model(g_sleep, NULL, E_BASE, (r - resumes_prev) * prof.wake_mj / 1000);
		w.resumes += r - resumes_prev;
	}
	resumes_prev = r;
	/* the gauge */
	long long q = read_ll("/sys/class/power_supply/max170xx_battery/charge_counter");
	double v = read_ll("/sys/class/power_supply/max170xx_battery/voltage_now") / 1e6;
	if (q >= 0 && charge_prev >= 0 && v > 0) {
		double used_uah = (double)(charge_prev - q);
		double vm = volt_prev > 0 ? (v + volt_prev) / 2 : v;
		if (now.charging) {
			w.chg_s += dt_boot;
		} else {
			/* a drop of the counter on battery is the phone's use; it
			 * cannot rise on battery, but the gauge corrects itself */
			w.meas_j += used_uah * 3.6e-3 * vm;	/* uAh * 3.6 mC/uAh * V */
			w.dis_s += dt_boot;
		}
		w.volt_sum += vm;
		w.volt_n++;
	}
	charge_prev = q;
	volt_prev = v;
	if (now.wall - w.t0 >= SETTLE_S) energy_settle(false);
}

void energy_settle(bool force) {
	(void)force;
	acc_find();
	acc_dp_link();
	if (w.dt_boot <= 0 && w.model_dis_j + w.model_chg_j <= 0) {
		w.t0 = now.wall;
		return;
	}
	double k = 1, unacc = 0;
	if (w.dis_s > 0 && w.meas_j > 0) {
		if (w.model_dis_j > w.meas_j) k = w.meas_j / w.model_dis_j;
		else unacc = w.meas_j - w.model_dis_j;
	}
	/* a window that was mostly on the charger is not scaled */
	if (w.chg_s > w.dis_s) {
		k = 1;
		unacc = 0;
	}
	k_last = k;
	for (int g = 0; g < n_groups; g++) {
		struct tally d = { 0 };
		bool any = false;
		for (int e = 0; e < NEK; e++) {
			if (groups[g].pend[e] > 0) any = true;
			d.e[e] = groups[g].pend[e] * k;
			groups[g].pend[e] = 0;
		}
		if (any) group_add(g, &d);
	}
	for (int i = 0; i < n_procs; i++) {
		for (int e = 0; e < NEK; e++) {
			procs[i].t.e[e] += procs[i].pend[e] * k;
			procs[i].pend[e] = 0;
		}
	}
	if (unacc > 0) {
		struct tally d = { .e = { [E_BASE] = unacc } };
		group_add(g_unacc, &d);
		unacc_total += unacc;
	}
	struct tally gd = { .fg_s = w.dis_s, .cpu_s = w.chg_s };
	gd.e[0] = w.dis_s > 0 ? w.meas_j : 0;
	gd.e[1] = w.model_dis_j;
	group_add(g_gauge, &gd);
	/* the window, for fitting the coefficients */
	if (w.dis_s > 0 && w.chg_s == 0) {
		if (n_calib == CALIB_KEEP) {
			memmove(calib[0], calib[1], sizeof(calib[0]) * (CALIB_KEEP - 1));
			n_calib--;
		}
		snprintf(calib[n_calib++], sizeof(calib[0]),
			"%lld,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%.0f,%.0f,%.1f,%.1f,%.0f,%.2f,%.2f,%.3f,"
			"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f",
			(long long)w.t0, w.dt_boot, w.dt_mono, w.susp_s, w.screen_s, w.bright_s,
			w.busy_s[0], n_cl > 1 ? w.busy_s[1] : 0, w.cpu_j[0], n_cl > 1 ? w.cpu_j[1] : 0, w.gpu_j,
			w.wifi_pkts, w.wifi_bytes, w.mob_bytes, w.wifi_assoc_s, w.mob_active_s, w.resumes,
			w.model_dis_j, w.meas_j, w.volt_n ? w.volt_sum / w.volt_n : 0,
			w.otg_s, w.usb_host_s, w.usb_dev_s, w.usb_active_s, w.dp_link_s, w.dp_video_s,
			w.bt_on_s, w.bt_conn_s, w.cp_s, w.airplane_s, w.waydroid_s, w.ctxt, w.irqs);
	}
	memset(&w, 0, sizeof(w));
	w.t0 = now.wall;
}

static const char calib_head[] = "start,boot_s,awake_s,asleep_s,screen_s,screen_bright_s,busy_little_s,busy_big_s,"
	"cpu_little_j,cpu_big_j,gpu_j,wifi_packets,wifi_bytes,mobile_bytes,wifi_assoc_s,mobile_active_s,"
	"resumes,model_j,measured_j,volts,"
	"otg_s,usb_host_s,usb_dev_s,usb_active_s,dp_link_s,dp_video_s,bt_on_s,bt_conn_s,cp_s,airplane_s,waydroid_s,"
	"ctxt,irqs";

/* A calib.csv from before the state columns: rewritten once with the new
 * header, its rows given empty fields (unknown, not 0: fit-calib.py fills
 * them from the kernel's log, or leaves those rows out of their fit). */
static void calib_upgrade(void) {
	FILE *f = fopen(ACCT_CALIB, "re");
	char line[1024];
	if (!f) return;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return;
	}
	line[strcspn(line, "\n")] = 0;
	if (!strcmp(line, calib_head)) {
		fclose(f);
		return;
	}
	int cols_new = 1, cols_old = 1;
	for (const char *c = calib_head; *c; c++) cols_new += *c == ',';
	for (const char *c = line; *c; c++) cols_old += *c == ',';
	FILE *o = fopen(ACCT_CALIB ".new", "we");
	if (!o) {
		fclose(f);
		return;
	}
	fprintf(o, "%s\n", calib_head);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		int n = 1;
		for (const char *c = line; *c; c++) n += *c == ',';
		fputs(line, o);
		for (int i = n; i < cols_new && n == cols_old; i++) fputc(',', o);
		fputc('\n', o);
	}
	fclose(f);
	if (fclose(o) == 0) rename(ACCT_CALIB ".new", ACCT_CALIB);
	else unlink(ACCT_CALIB ".new");
}

void energy_save_calib(void) {
	if (!n_calib) return;
	calib_upgrade();
	FILE *f = fopen(ACCT_CALIB, "ae");
	if (!f) return;
	if (ftell(f) == 0) fprintf(f, "%s\n", calib_head);
	for (int i = 0; i < n_calib; i++) fprintf(f, "%s\n", calib[i]);
	long size = ftell(f);
	fclose(f);
	n_calib = 0;
	if (size > 4 << 20) rename(ACCT_CALIB, ACCT_CALIB ".1");	/* one old one kept */
}

void energy_status(FILE *f) {
	fprintf(f, "\"energy\":{\"k_last\":%.3f,\"unaccounted_wh\":%.4f,\"window_s\":%.0f,\"window_model_j\":%.1f,"
		"\"window_measured_j\":%.1f,\"clusters\":[", k_last, unacc_total / 3600, (double)(now.wall - w.t0),
		w.model_dis_j, w.meas_j);
	for (int i = 0; i < n_cl; i++) {
		fprintf(f, "%s{\"path\":\"%s\",\"cpus\":%d,\"busy_s\":%.1f,\"wh\":%.4f,\"mw_per_core_now\":%.0f}", i ? "," : "",
			cl[i].path, cl[i].n_cpus, cl[i].busy_total, cl[i].e_total / 3600, cl[i].eps * 1000);
	}
	fprintf(f, "],\"profile\":{\"base_awake_mw\":%.0f,\"base_suspend_mw\":%.0f,\"screen_mw\":%.0f,"
		"\"screen_full_mw\":%.0f,\"wifi_idle_on_mw\":%.0f,\"wifi_idle_off_mw\":%.0f,\"wifi_pkt_mj\":%.3f,"
		"\"wifi_byte_uj\":%.3f,\"cpu_scale\":%.3f,\"gpu_scale\":%.3f,\"base_extra_mw\":%.0f,\"otg_mw\":%.0f,"
		"\"usb_host_mw\":%.0f,\"usb_dev_mw\":%.0f,\"dp_link_mw\":%.0f,\"dp_video_mw\":%.0f,\"bt_on_mw\":%.0f,"
		"\"bt_conn_mw\":%.0f,\"cp_air_mw\":%.0f},\"gpu\":\"%s\",\"resumes\":\"%s\",",
		prof.base_awake_mw, prof.base_suspend_mw, prof.screen_mw, prof.screen_full_mw, prof.wifi_idle_on_mw,
		prof.wifi_idle_off_mw, prof.wifi_pkt_mj, prof.wifi_byte_uj, prof.cpu_scale, prof.gpu_scale,
		prof.base_extra_mw, prof.otg_mw, prof.usb_host_mw, prof.usb_dev_mw, prof.dp_link_mw, prof.dp_video_mw,
		prof.bt_on_mw, prof.bt_conn_mw, prof.cp_air_mw, gpu_path, resume_path);
	/* what the accessories were at the last scan, and where it was read */
	fprintf(f, "\"accessories\":{\"vbus_out\":%d,\"usb_devices\":%d,\"usb_active\":%d,\"dp_link\":%d,"
		"\"dp_video\":%d,\"bt_up\":%d,\"bt_links\":%d,\"modem_online\":%d,\"airplane\":%d,\"waydroid\":%d,"
		"\"otg\":\"%s\",\"dp\":\"%s\",\"modem\":\"%s\"}}",
		acc.vbus, acc.usb_n, acc.usb_active, acc.dp_link, acc.dp_video, acc.bt_up, acc.bt_links, acc.cp_on,
		acc.airplane, acc.waydroid, acc.otg, acc.dp_on, acc.modem);
}

int energy_unaccounted_group(void) { return g_unacc;
}
