// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztusbenergyd's accounting on a fake phone: a fake sysfs (the OTG boost's
 * regulator, the gauge, the charger, the backlight, USB devices), fake
 * uevents as the kernel sends them, and a fake coulomb counter stepping in
 * 1 mAh as the real one does. No USB, no root, no phone.
 *
 *	test-usbenergy
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>

#include "../usb-energy-track.h"

static char root[PATH_MAX], perf[PATH_MAX + 32];
static int failed, passed;

#define CHECK(cond, ...) do { \
	if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static void mkdirs(const char *rel)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s", root, rel);
	for (char *s = p + strlen(root) + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
	}
	mkdir(p, 0755);
}

static void put(const char *dir, const char *attr, const char *fmt, ...)
{
	char p[PATH_MAX];
	mkdirs(dir);
	snprintf(p, sizeof(p), "%s/%s/%s", root, dir, attr);
	FILE *f = fopen(p, "w");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

#define BAT "class/power_supply/max170xx_battery"
#define CHG "class/power_supply/max77705-charger"
#define USBC "class/power_supply/max77705-usbc"
#define OTG "class/regulator/regulator.5"
#define BL "class/backlight/s9p-panel"

/* ---- the fake phone ---- */

static struct {
	double boot, mono;	/* seconds */
	long long wall;
	double used_uah;	/* since the start, exact; the counter shows it in 1 mAh steps */
	double volts;
	double own_w;		/* the phone's own draw */
	double acc_w;		/* what the accessories cost the battery */
	bool charging, vbus, screen;
} ph;

static void phone_write(void)
{
	long long q = 3000000 - (long long)(ph.used_uah / 1000) * 1000;
	put(BAT, "charge_counter", "%lld", q);
	put(BAT, "voltage_now", "%lld", (long long)(ph.volts * 1e6));
	put(BAT, "voltage_avg", "%lld", (long long)(ph.volts * 1e6));
	double w = ph.charging ? -1.0 : ph.own_w + (ph.vbus ? ph.acc_w : 0);
	put(BAT, "current_avg", "%lld", (long long)(-w / ph.volts * 1e6));
	put(BAT, "status", ph.charging ? "Charging" : "Discharging");
	put(CHG, "online", ph.charging ? "1" : "0");
	put(OTG, "state", ph.vbus ? "enabled" : "disabled");
	put(BL, "bl_power", ph.screen ? "0" : "4");
	FILE *f = fopen(perf, "w");
	fprintf(f, "profile=balanced\nscreen=%s\ncpus=8\n", ph.screen ? "on" : "off");
	fclose(f);
}

static void phone_reset(void)
{
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd)) {
		/* a fresh directory follows either way */
	}
	mkdir(root, 0755);
	memset(&ph, 0, sizeof(ph));
	ph.boot = ph.mono = 1000;
	ph.wall = 1795400000;
	ph.volts = 3.8;
	ph.own_w = 0.40;
	put(BAT, "type", "Battery");
	put(CHG, "type", "USB");
	put(USBC, "type", "USB");
	put(USBC, "online", "1");	/* the port's own supply: must not read as charging */
	put(OTG, "name", "max77705-otg");
	put(BL, "brightness", "256");
	put(BL, "max_brightness", "512");
	mkdirs("bus/usb/devices/usb1");
	put("bus/usb/devices/usb1", "idVendor", "1d6b");	/* a root hub: not an accessory */
	put("bus/usb/devices/usb1", "idProduct", "0002");
	phone_write();
}

/* time passes: awake (or asleep), the battery draining as it does */
static void phone_run(double s, bool asleep)
{
	double w = ph.charging ? 0 : asleep ? 0.1 + (ph.vbus ? ph.acc_w * 0.1 : 0) : ph.own_w + (ph.vbus ? ph.acc_w : 0);
	ph.used_uah += w * s / ph.volts / 3600 * 1e6;
	ph.boot += s;
	if (!asleep)
		ph.mono += s;
	ph.wall += (long long)s;
	phone_write();
}

static void device(const char *name, const char *vid, const char *pid, const char *serial, const char *product,
	const char *power)
{
	char d[96];
	snprintf(d, sizeof(d), "bus/usb/devices/%s", name);
	put(d, "idVendor", "%s", vid);
	put(d, "idProduct", "%s", pid);
	if (serial)
		put(d, "serial", "%s", serial);
	put(d, "manufacturer", "Apple, Inc.");
	put(d, "product", "%s", product);
	put(d, "bMaxPower", "%s", power);
	snprintf(d, sizeof(d), "bus/usb/devices/%s/power", name);
	put(d, "runtime_status", "active");
}

static void unplug(const char *name)
{
	char cmd[PATH_MAX + 64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/bus/usb/devices/%s'", root, name);
	if (system(cmd)) {
		/* checked by the tracker reading it */
	}
}

/* ---- fake uevents, as the kernel's netlink messages ---- */

static size_t uevent_msg(char *buf, size_t size, const char *action, const char *devpath, const char *subsystem,
	const char *devtype)
{
	size_t n = 0;
	n += snprintf(buf + n, size - n, "%s@%s", action, devpath) + 1;
	n += snprintf(buf + n, size - n, "ACTION=%s", action) + 1;
	n += snprintf(buf + n, size - n, "DEVPATH=%s", devpath) + 1;
	n += snprintf(buf + n, size - n, "SUBSYSTEM=%s", subsystem) + 1;
	if (devtype)
		n += snprintf(buf + n, size - n, "DEVTYPE=%s", devtype) + 1;
	n += snprintf(buf + n, size - n, "SEQNUM=4242") + 1;
	return n;
}

static struct ue_store store;
static struct ue_tracker trk;

static void env(struct ue_env *e)
{
	ue_read_env(&trk, e, ph.boot, ph.mono, ph.wall);
}

/* the kernel says ACTION for DEVPATH; the daemon reads the state and acts */
static void kernel_says(const char *action, const char *devpath, const char *subsystem, const char *devtype)
{
	char buf[1024];
	size_t n = uevent_msg(buf, sizeof(buf), action, devpath, subsystem, devtype);
	struct ue_uevent u;
	if (!ue_uevent_parse(buf, n, &u))
		return;
	struct ue_env e;
	env(&e);
	ue_track_uevent(&trk, &e, &u);
}

static void tick(void)
{
	struct ue_env e;
	env(&e);
	ue_track_advance(&trk, &e);
}

#define PORT "/devices/platform/10c00000.usb/xhci-hcd.0.auto/usb1"

static void fresh_tracker(void)
{
	memset(&store, 0, sizeof(store));
	ue_track_init(&trk, &store, root);
	snprintf(trk.perf_status, sizeof(trk.perf_status), "%s", perf);
	trk.screen_mw = 235;
	trk.screen_full_mw = 389;
	trk.suspend_mw = 100;
}

/* plug in at the port: the partner first (VBUS on), then the device */
static void plug(const char *name, const char *vid, const char *pid, const char *serial, const char *product,
	const char *power)
{
	kernel_says("add", "/devices/platform/usb-c/typec/port0/port0-partner", "typec", NULL);
	ph.vbus = true;
	phone_run(0.5, false);
	device(name, vid, pid, serial, product, power);
	char dp[256];
	snprintf(dp, sizeof(dp), PORT "/%s", name);
	kernel_says("add", dp, "usb", "usb_device");
	snprintf(dp, sizeof(dp), PORT "/%s/%s:1.0", name, name);
	kernel_says("add", dp, "usb", "usb_interface");	/* ignored */
}

static void pull(const char *name)
{
	char dp[256];
	snprintf(dp, sizeof(dp), PORT "/%s/%s:1.0", name, name);
	kernel_says("remove", dp, "usb", "usb_interface");
	unplug(name);
	snprintf(dp, sizeof(dp), PORT "/%s", name);
	kernel_says("remove", dp, "usb", "usb_device");
	ph.vbus = false;
	phone_write();
	kernel_says("remove", "/devices/platform/usb-c/typec/port0/port0-partner", "typec", NULL);
}

static void run_for(double s, bool asleep)
{
	for (double t = 0; t < s; t += UE_TICK_S) {
		double step = s - t < UE_TICK_S ? s - t : UE_TICK_S;
		phone_run(step, asleep);
		tick();
	}
}

static const struct ue_sess *last_sess(const char *key)
{
	const struct ue_acc *a = ue_find(&store, key);
	return a && a->n_sess ? &a->sess[a->n_sess - 1] : NULL;
}

/* ---- the tests ---- */

static void test_format(void)
{
	char s[24];
	CHECK(!strcmp(ue_fmt_wh(0.0004, s, sizeof(s)), "0.40 mWh"), "%s", s);
	CHECK(!strcmp(ue_fmt_wh(0.0833, s, sizeof(s)), "83 mWh"), "%s", s);
	CHECK(!strcmp(ue_fmt_wh(1.2345, s, sizeof(s)), "1.23 Wh"), "%s", s);
	char k[160];
	ue_make_key(k, sizeof(k), "05ac", "110b", "ABC123", "1-1");
	CHECK(!strcmp(k, "05ac:110b:ABC123"), "%s", k);
	ue_make_key(k, sizeof(k), "05ac", "110b", "", "1-1.2");
	CHECK(!strcmp(k, "05ac:110b@1-1.2"), "%s", k);
}

static void test_uevents(void)
{
	char buf[1024];
	struct ue_uevent u;
	size_t n = uevent_msg(buf, sizeof(buf), "add", PORT "/1-1", "usb", "usb_device");
	CHECK(ue_uevent_parse(buf, n, &u) && !strcmp(u.name, "1-1") && !strcmp(u.action, "add"), "usb device add");
	n = uevent_msg(buf, sizeof(buf), "add", PORT "/1-1/1-1:1.0", "usb", "usb_interface");
	CHECK(!ue_uevent_parse(buf, n, &u), "an interface is not a device");
	n = uevent_msg(buf, sizeof(buf), "change", "/devices/platform/fuel-gauge/power_supply/max170xx_battery",
		"power_supply", NULL);
	CHECK(!ue_uevent_parse(buf, n, &u), "the battery's uevents are not ours");
	n = uevent_msg(buf, sizeof(buf), "add", "/devices/platform/usb-c/typec/port0/port0-partner", "typec", NULL);
	CHECK(ue_uevent_parse(buf, n, &u) && !strcmp(u.name, "port0-partner"), "typec partner");
	memcpy(buf, "libudev\0\xfe\xed\xca\xfe", 12);
	CHECK(!ue_uevent_parse(buf, 12, &u), "udev's messages are not the kernel's");
	CHECK(ue_is_device_name("1-1") && ue_is_device_name("1-1.4") && !ue_is_device_name("usb1") &&
		!ue_is_device_name("1-1:1.0"), "device names");
}

/* EarPods for ten minutes, screen off: too small to see in the battery
 * against the phone's own wander, so the record is the upper bound */
static void test_earpods(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 0.15;	/* a USB DAC and the bus: about what the gauge would see */
	plug("1-1", "05ac", "110b", NULL, "EarPods", "100mA");
	CHECK(trk.n_dev == 1 && trk.base_ok, "attached, with a baseline");
	CHECK(NEAR(trk.base_w, 0.40, 0.01), "baseline %.3f W is the phone before the plug", trk.base_w);
	run_for(600, false);
	pull("1-1");
	const struct ue_sess *s = last_sess("05ac:110b@1-1");
	CHECK(s && s->end > s->start, "session closed");
	if (!s)
		return;
	CHECK(NEAR(s->connected_s, 600, 0.1), "connected %.1f s", s->connected_s);
	CHECK(NEAR(s->upper_wh, 0.5 * 600 / 3600, 0.0005), "bound %.4f Wh = 100 mA x 5 V x 10 min", s->upper_wh);
	CHECK(NEAR(s->est_wh, 0.15 * 600 / 3600, s->est_err_wh), "estimate %.4f within its band %.4f of 25 mWh",
		s->est_wh, s->est_err_wh);
	CHECK(!strcmp(s->method, UE_METHOD_BOUND) && s->wh == s->upper_wh, "small: the bound (%s)", s->method);
	const struct ue_acc *a = ue_find(&store, "05ac:110b@1-1");
	CHECK(a && !strcmp(a->name, "Apple, Inc. EarPods") && a->n_sessions == 1 && a->max_power_ma == 100,
		"accessory record");
	CHECK(trk.n_dev == 0 && !trk.base_ok, "port empty, baseline gone");
}

/* a dock drawing 1.5 W: clear of the noise, and the record is the estimate */
static void test_dock(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.5;
	plug("1-1", "0bda", "5411", NULL, "4-Port USB 2.0 Hub", "500mA");
	run_for(600, false);
	pull("1-1");
	const struct ue_sess *s = last_sess("0bda:5411@1-1");
	CHECK(s && !strcmp(s->method, UE_METHOD_EST), "estimated (%s)", s ? s->method : "-");
	if (!s)
		return;
	CHECK(NEAR(s->wh, 1.5 * 600 / 3600, 0.010), "%.4f Wh, want 0.250 +- 0.010", s->wh);
	CHECK(s->est_err_wh > 0.010 && s->est_err_wh < 0.030, "band %.4f Wh: 60 mW x 10 min + 2 mAh", s->est_err_wh);
}

/* the screen comes on midway: the baseline moves by the known screen load */
static void test_screen_rebaseline(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.5;
	plug("1-1", "0bda", "5411", NULL, "Hub", "500mA");
	run_for(300, false);
	ph.screen = true;
	ph.own_w += (235 + 389 * 0.5) / 1000.0;	/* what the profile says the screen costs at half */
	tick();
	run_for(300, false);
	pull("1-1");
	const struct ue_sess *s = last_sess("0bda:5411@1-1");
	CHECK(s && NEAR(s->est_wh, 0.25, 0.012), "screen load taken out: %.4f Wh", s ? s->est_wh : 0);
	CHECK(s && s->est_err_wh > 0.06, "screen on widens the band: %.4f", s ? s->est_err_wh : 0);
}

/* asleep with the accessory in: time counts, the bound drops to the suspend
 * current, the estimate takes ztacct's suspend figure */
static void test_suspend(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.5;
	plug("1-1", "0bda", "5411", NULL, "Hub", "500mA");
	run_for(120, false);
	phone_run(3600, true);	/* the timer does not wake it: one reading after */
	tick();
	run_for(120, false);
	pull("1-1");
	const struct ue_sess *s = last_sess("0bda:5411@1-1");
	if (!s) {
		CHECK(0, "no session");
		return;
	}
	CHECK(NEAR(s->asleep_s, 3600, 1) && NEAR(s->connected_s, 3840, 0.1), "asleep %.0f of %.0f s", s->asleep_s,
		s->connected_s);
	double want = (2.5 * 240 + 0.0125 * 3600) / 3600;
	CHECK(NEAR(s->upper_wh, want, 0.001), "bound %.4f, want %.4f", s->upper_wh, want);
	/* the fake phone's accessory draws 10 % asleep: 0.15 W for an hour */
	CHECK(NEAR(s->est_wh, (1.5 * 240 + 0.15 * 3600) / 3600, s->est_err_wh), "estimate %.4f +- %.4f", s->est_wh,
		s->est_err_wh);
}

/* charging midway: the gauge cannot see it; and a powered dock gives VBUS itself */
static void test_charging_and_powered_dock(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.5;
	plug("1-1", "0bda", "5411", NULL, "Hub", "500mA");
	run_for(120, false);
	ph.charging = true;
	run_for(480, false);
	ph.charging = false;
	pull("1-1");
	const struct ue_sess *s = last_sess("0bda:5411@1-1");
	CHECK(s && !strcmp(s->method, UE_METHOD_BOUND) && s->est_s < 200, "charging: bound (%s, est %.0f s)",
		s ? s->method : "-", s ? s->est_s : 0);

	/* a dock with its own power: no boost */
	device("1-1", "2109", "2817", "000000001", "USB2.0 Hub", "0mA");
	ph.charging = true;
	phone_write();
	kernel_says("add", PORT "/1-1", "usb", "usb_device");
	run_for(300, false);
	unplug("1-1");
	kernel_says("remove", PORT "/1-1", "usb", "usb_device");
	s = last_sess("2109:2817:000000001");
	CHECK(s && !strcmp(s->method, UE_METHOD_NONE) && s->wh == 0 && s->powered_s == 0, "not powered (%s)",
		s ? s->method : "-");
}

/* the daemon started with the device in: no "before", so the bound */
static void test_started_attached(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.5;
	ph.vbus = true;
	device("1-1", "0bda", "5411", NULL, "Hub", "500mA");
	phone_write();
	struct ue_env e;
	env(&e);
	ue_track_advance(&trk, &e);
	ue_track_scan(&trk, &e, false);
	CHECK(trk.n_dev == 1 && !trk.base_ok, "found, no baseline");
	run_for(300, false);
	struct ue_env e2;
	env(&e2);
	ue_track_close_all(&trk, &e2);
	const struct ue_sess *s = last_sess("0bda:5411@1-1");
	CHECK(s && !strcmp(s->method, UE_METHOD_BOUND) && s->est_s == 0 && s->end, "bound only (%s)",
		s ? s->method : "-");
}

/* a hub with a device behind it: the estimate shared by the bounds; runtime
 * suspend drops a device to the suspend current */
static void test_hub_and_runtime_suspend(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 1.0;
	plug("1-1", "0bda", "5411", NULL, "Hub", "100mA");
	device("1-1.2", "05ac", "110b", "SER1", "EarPods", "400mA");
	put("bus/usb/devices/1-1.2/power", "runtime_status", "suspended");
	kernel_says("add", PORT "/1-1/1-1.2", "usb", "usb_device");
	run_for(300, false);
	const struct ue_sess *hub = last_sess("0bda:5411@1-1"), *ear = last_sess("05ac:110b:SER1");
	CHECK(hub && ear && hub->est_wh > 0 && ear->est_wh > 0, "both get a share");
	if (hub && ear) {
		/* the hub active at 0.5 W, the EarPods suspended at 12.5 mW */
		CHECK(NEAR(ear->upper_wh, 0.0125 * 300 / 3600, 0.0002), "suspended bound %.5f", ear->upper_wh);
		CHECK(hub->est_wh > 30 * ear->est_wh, "shared by the bounds: %.4f vs %.4f", hub->est_wh, ear->est_wh);
	}
	unplug("1-1.2");
	kernel_says("remove", PORT "/1-1/1-1.2", "usb", "usb_device");
	pull("1-1");
	CHECK(trk.n_dev == 0, "all gone");
}

/* the record outlives the unplug, the daemon and the reboot */
static void test_persist(void)
{
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/state/ztos/usb-energy.json", root);
	phone_reset();
	fresh_tracker();
	ph.acc_w = 0.15;
	plug("1-1", "05ac", "110b", "SER\"1\\", "Ear\"Pods", "100mA");
	run_for(600, false);
	pull("1-1");
	ph.acc_w = 1.5;
	plug("1-1", "05ac", "110b", "SER\"1\\", "Ear\"Pods", "500mA");
	run_for(600, false);
	pull("1-1");
	const char *key = "05ac:110b:SER\"1\\";
	struct ue_acc *a = ue_find(&store, key);
	CHECK(a && a->n_sessions == 2 && !strcmp(a->method, UE_METHOD_MIXED), "two sessions, mixed (%s)",
		a ? a->method : "-");
	CHECK(ue_save(path, &store), "saved");
	struct stat st;
	CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0644, "the file, readable");

	static struct ue_store back;
	CHECK(ue_load(path, &back), "loaded");
	struct ue_acc *b = ue_find(&back, key);
	CHECK(b != NULL, "found by its key with quotes and backslashes");
	if (a && b) {
		CHECK(!strcmp(b->name, "Apple, Inc. Ear\"Pods"), "name %s", b->name);
		CHECK(b->n_sessions == 2 && b->n_sess == 2, "sessions");
		CHECK(NEAR(b->wh, a->wh, 1e-6) && NEAR(b->upper_wh, a->upper_wh, 1e-6) &&
			NEAR(b->connected_s, a->connected_s, 0.1), "totals");
		CHECK(NEAR(b->wh, b->sess[0].wh + b->sess[1].wh, 1e-6), "total is the sessions' sum");
		CHECK(b->first_seen == a->first_seen && b->last_seen == a->last_seen && b->first_seen < b->last_seen,
			"first and last seen");
		CHECK(!strcmp(b->sess[0].method, a->sess[0].method) && b->sess[1].end == a->sess[1].end, "session fields");
	}

	/* a checkpoint with a session open, then a hard reset: it closes at its last numbers */
	plug("1-1", "05ac", "110b", "SER\"1\\", "Ear\"Pods", "100mA");
	run_for(120, false);
	CHECK(ue_save(path, &store), "checkpoint");
	CHECK(ue_load(path, &back), "after the reset");
	CHECK(ue_close_stale(&back) == 1, "one left open");
	b = ue_find(&back, key);
	CHECK(b && b->sess[b->n_sess - 1].end > b->sess[b->n_sess - 1].start, "closed");

	/* fields it does not know, and a damaged file */
	const char *extra = "{\"version\": 2, \"new\": [1, {\"x\": null}], \"accessories\": [{\"key\": \"a:b@1-1\", "
		"\"wh\": 0.5, \"future\": true, \"sessions\": []}]}";
	CHECK(ue_parse(&back, extra, strlen(extra)) && back.n == 1 && NEAR(back.acc[0].wh, 0.5, 1e-9), "tolerant");
	CHECK(!ue_parse(&back, "{\"accessories\": [{\"key\": ", 25), "truncated is an error");
}

/* many sessions: the last 100 kept, the totals keep all */
static void test_many_sessions(void)
{
	phone_reset();
	fresh_tracker();
	ph.acc_w = 0.15;
	for (int i = 0; i < 105; i++) {
		plug("1-1", "05ac", "110b", "S", "EarPods", "100mA");
		run_for(60, false);
		pull("1-1");
	}
	struct ue_acc *a = ue_find(&store, "05ac:110b:S");
	CHECK(a && a->n_sessions == 105 && a->n_sess == UE_MAX_SESS, "105 sessions, 100 kept");
	if (a)
		CHECK(NEAR(a->upper_wh, 105 * 0.5 * 60 / 3600, 0.001), "totals keep all: %.4f", a->upper_wh);
}

int main(void)
{
	snprintf(root, sizeof(root), "/tmp/ztmetro-usbenergy-test.%d", (int)getpid());
	snprintf(perf, sizeof(perf), "%s.perf", root);
	test_format();
	test_uevents();
	test_earpods();
	test_dock();
	test_screen_rebaseline();
	test_suspend();
	test_charging_and_powered_dock();
	test_started_attached();
	test_hub_and_runtime_suspend();
	test_persist();
	test_many_sessions();
	char cmd[PATH_MAX + 64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", root, perf);
	if (system(cmd)) {
		/* left behind in /tmp */
	}
	printf("%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
