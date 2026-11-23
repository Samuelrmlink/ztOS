// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * usb-energy-track.h: ztusbenergyd's accounting, apart from its main loop so
 * the host tests can drive it with a fake sysfs, fake uevents and fake gauge
 * readings. Header only. README.md says why it measures what it does.
 *
 * Every USB device on the host bus (not root hubs, not interfaces) has a
 * session from its plug-in to its unplug. While any is attached, the state is
 * read at each uevent and every UE_TICK_S (a CLOCK_MONOTONIC timer: it never
 * wakes a sleeping phone), and the time since the last read, split into
 * awake and asleep by CLOCK_BOOTTIME - CLOCK_MONOTONIC, is accounted:
 *
 *  upper bound	each device's bMaxPower at 5 V while the phone gives VBUS
 *		(its OTG boost regulator enabled), 2.5 mA (USB's suspend
 *		current) while the device is runtime-suspended or the phone is
 *		asleep
 *  estimate	the gauge's coulomb counter (charge_counter x voltage) less
 *		the phone's own draw: the gauge's average power read at the
 *		plug-in, before anything was attached, moved by the known screen
 *		load (ztacct's power_profile) when the screen changes since, and
 *		ztacct's suspend figure while asleep. Not while charging, or
 *		without a baseline (the daemon started with the device in).
 *		Shared among the devices by their bounds.
 */
#ifndef ZT_USB_ENERGY_TRACK_H
#define ZT_USB_ENERGY_TRACK_H

#include <dirent.h>
#include "usb-energy.h"

#define UE_TICK_S	30	/* the state while attached; the coulomb counter integrates between */
#define UE_SAVE_S	300	/* a checkpoint of open sessions */
#define UE_MAX_DEV	32
#define UE_VBUS_V	5.0
#define UE_SUSPEND_MA	2.5	/* USB 2.0 7.2.3: a suspended device's most */
#define UE_BASE_FRESH_S	15	/* a baseline from the port's own attach, this recent, is used */
/* The phone's own draw moves on its own; how far, in 90 % of 5-minute steps
 * (ztacct's calib.csv and zrn-sleepd's soc.csv, 2026-11-11 to 11-22; README.md):
 * screen off 30-50 mW, screen on 0.6-1.0 W */
#define UE_SIGMA_OFF_W	0.06
#define UE_SIGMA_ON_W	0.80
#define UE_SIGMA_SLEEP_W 0.05
#define UE_Q_LSB_UAH	1000.0	/* charge_counter's step as the phone reports it */

struct ue_env {
	double boot_s, mono_s;
	long long wall;
	long long q_uah;	/* charge_counter, -1 unknown */
	double v_uv;		/* voltage_now */
	double i_avg_ua, v_avg_uv;	/* current_avg (+ charging), voltage_avg */
	bool gauge_ok;		/* current_avg and voltage_avg were read */
	bool charging, vbus, screen_on;
	double bright;		/* 0..1 */
};

struct ue_dev {
	char name[32];		/* sysfs: 1-1, 1-1.2 */
	char key[160];
	int max_ma;
	bool active;		/* not runtime-suspended */
	struct ue_sess cur, saved;	/* now, and as last put in the store */
};

struct ue_tracker {
	char sys[PATH_MAX];		/* "/sys", or a fake one */
	char perf_status[PATH_MAX];	/* zrnperfd's status: screen=on|off */
	struct ue_store *store;
	struct ue_dev dev[UE_MAX_DEV];
	int n_dev;
	struct ue_env last;
	bool have_last;
	/* the phone's own draw, read before the port's first device */
	bool base_ok;
	double base_w, base_bright, base_at;
	bool base_screen;
	bool est_run;		/* the last interval was estimated: no new counter step to allow for */
	/* ztacct's power_profile: the known loads */
	double screen_mw, screen_full_mw, suspend_mw;
	double last_save;	/* boot_s */
	bool dirty;
};

/* ---- sysfs ---- */

static inline bool ue_read(const char *path, char *out, size_t size)
{
	out[0] = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ssize_t n = read(fd, out, size - 1);
	close(fd);
	if (n <= 0)
		return false;
	out[n] = 0;
	out[strcspn(out, "\n")] = 0;
	return true;
}

static inline bool ue_read_attr(const char *dir, const char *attr, char *out, size_t size)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s", dir, attr);
	return ue_read(p, out, size);
}

static inline bool ue_read_ll(const char *dir, const char *attr, long long *v)
{
	char s[32];
	if (!ue_read_attr(dir, attr, s, sizeof(s)) || !s[0])
		return false;
	char *e;
	*v = strtoll(s, &e, 10);
	return e != s;
}

static inline void ue_profile(struct ue_tracker *t, const char *path)
{
	t->screen_mw = 235;
	t->screen_full_mw = 389;
	t->suspend_mw = 100;
	FILE *f = fopen(path, "re");
	char line[160], key[64];
	double v;
	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63s %lf", key, &v) != 2)
			continue;
		if (!strcmp(key, "screen_mw"))
			t->screen_mw = v;
		else if (!strcmp(key, "screen_full_mw"))
			t->screen_full_mw = v;
		else if (!strcmp(key, "base_suspend_mw"))
			t->suspend_mw = v;
	}
	if (f)
		fclose(f);
}

static inline void ue_track_init(struct ue_tracker *t, struct ue_store *store, const char *sys)
{
	memset(t, 0, sizeof(*t));
	t->store = store;
	snprintf(t->sys, sizeof(t->sys), "%s", sys ? sys : "/sys");
	snprintf(t->perf_status, sizeof(t->perf_status), "/run/zrnperfd.status");
	ue_profile(t, "/etc/ztacct/power_profile");
}

/* The phone's state now (the clocks are the caller's) */
static inline void ue_read_env(struct ue_tracker *t, struct ue_env *e, double boot_s, double mono_s, long long wall)
{
	char dir[PATH_MAX], p[PATH_MAX], s[256];
	memset(e, 0, sizeof(*e));
	e->boot_s = boot_s;
	e->mono_s = mono_s;
	e->wall = wall;
	e->q_uah = -1;

	/* VBUS out: the OTG boost's regulator, or else the USB-C port as source */
	bool have_reg = false;
	snprintf(dir, sizeof(dir), "%s/class/regulator", t->sys);
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (ue_read_attr(p, "name", s, sizeof(s)) && strstr(s, "otg")) {
			have_reg = true;
			e->vbus = e->vbus || (ue_read_attr(p, "state", s, sizeof(s)) && !strcmp(s, "enabled"));
		}
	}
	if (d)
		closedir(d);
	if (!have_reg) {
		snprintf(p, sizeof(p), "%s/class/typec/port0/power_role", t->sys);
		e->vbus = ue_read(p, s, sizeof(s)) && strstr(s, "[source]");
	}

	/* the gauge and the chargers */
	bool wired = false, wireless = false;
	snprintf(dir, sizeof(dir), "%s/class/power_supply", t->sys);
	d = opendir(dir);
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		char type[32];
		ue_read_attr(p, "type", type, sizeof(type));
		if (!strcmp(type, "Battery")) {
			long long v;
			if (ue_read_ll(p, "charge_counter", &v))
				e->q_uah = v;
			if (ue_read_ll(p, "voltage_now", &v))
				e->v_uv = v;
			long long ia, va;
			if (ue_read_ll(p, "current_avg", &ia) && ue_read_ll(p, "voltage_avg", &va)) {
				e->i_avg_ua = ia;
				e->v_avg_uv = va;
				e->gauge_ok = va > 0;
			}
			if (ue_read_attr(p, "status", s, sizeof(s)) && (!strcmp(s, "Charging") || !strcmp(s, "Full")))
				e->charging = true;
			continue;
		}
		/* the USB-C port's own supply says what the partner offers, not what flows */
		if (strstr(de->d_name, "usbc"))
			continue;
		long long on;
		if (!ue_read_ll(p, "online", &on) || on != 1)
			continue;
		if (!strcmp(type, "Wireless"))
			wireless = true;
		else
			wired = true;
	}
	if (d)
		closedir(d);
	/* with the boost giving VBUS the cable cannot be charging the phone */
	e->charging = e->charging || wireless || (wired && !e->vbus);

	/* the screen: zrnperfd's word, or the backlight's power */
	bool said = false, bl_on = false;
	FILE *f = fopen(t->perf_status, "re");
	while (f && fgets(s, sizeof(s), f)) {
		if (!strncmp(s, "screen=", 7)) {
			e->screen_on = !strncmp(s + 7, "on", 2);
			said = true;
		}
	}
	if (f)
		fclose(f);
	snprintf(dir, sizeof(dir), "%s/class/backlight", t->sys);
	d = opendir(dir);
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		long long b, m;
		if (ue_read_ll(p, "bl_power", &b) && b == 0)
			bl_on = true;
		if (ue_read_ll(p, "brightness", &b) && ue_read_ll(p, "max_brightness", &m) && m > 0)
			e->bright = (double)b / m;
	}
	if (d)
		closedir(d);
	if (!said)
		e->screen_on = bl_on;
}

/* ---- the phone's own draw ---- */

static inline double ue_screen_w(const struct ue_tracker *t, bool on, double bright)
{
	return on ? (t->screen_mw + t->screen_full_mw * bright) / 1000 : 0;
}

/* The baseline: the gauge's average (a few seconds), from before the plug */
static inline void ue_take_baseline(struct ue_tracker *t, const struct ue_env *e)
{
	t->base_at = e->boot_s;
	t->base_ok = e->gauge_ok && !e->charging && e->i_avg_ua < 0;
	t->base_w = t->base_ok ? -e->i_avg_ua * e->v_avg_uv / 1e12 : 0;
	t->base_screen = e->screen_on;
	t->base_bright = e->bright;
	t->est_run = false;
}

/* ---- sessions ---- */

static inline void ue_decide(struct ue_sess *s)
{
	if (s->powered_s <= 0) {
		s->wh = 0;
		snprintf(s->method, sizeof(s->method), "%s", UE_METHOD_NONE);
	} else if (s->est_s >= 0.9 * s->powered_s && s->est_wh >= 2 * s->est_err_wh && s->est_wh <= s->upper_wh) {
		/* resolved above the noise, and no more than the device may draw */
		s->wh = s->est_wh;
		snprintf(s->method, sizeof(s->method), "%s", UE_METHOD_EST);
	} else {
		s->wh = s->upper_wh;
		snprintf(s->method, sizeof(s->method), "%s", UE_METHOD_BOUND);
	}
}

/* The device's session into the store: its accessory's totals and kept sessions */
static inline void ue_store_dev(struct ue_tracker *t, struct ue_dev *d, long long wall)
{
	ue_decide(&d->cur);
	struct ue_acc *a = ue_find(t->store, d->key);
	if (!a)
		return;
	for (int k = a->n_sess - 1; k >= 0; k--) {
		if (a->sess[k].start == d->cur.start) {
			a->sess[k] = d->cur;
			break;
		}
	}
	ue_acc_add(a, &d->saved, &d->cur);
	ue_acc_method(a);
	a->last_seen = wall;
	d->saved = d->cur;
	t->store->updated = wall;
	t->dirty = true;
}

/* The time from the last reading to E */
static inline void ue_interval(struct ue_tracker *t, const struct ue_env *a, const struct ue_env *b)
{
	double dt = b->boot_s - a->boot_s;
	if (dt <= 0 || !t->n_dev)
		return;
	double awake = b->mono_s - a->mono_s;
	if (awake < 0)
		awake = 0;
	if (awake > dt)
		awake = dt;
	double asleep = dt - awake;
	bool powered = a->vbus || b->vbus;

	double bound[UE_MAX_DEV], sum = 0;
	for (int i = 0; i < t->n_dev; i++) {
		struct ue_dev *d = &t->dev[i];
		double full = d->max_ma * UE_VBUS_V / 1000;
		double susp = (d->max_ma < UE_SUSPEND_MA ? d->max_ma : UE_SUSPEND_MA) * UE_VBUS_V / 1000;
		bound[i] = powered ? (d->active ? full : susp) * awake + susp * asleep : 0;	/* joules */
		sum += bound[i];
	}

	bool est = t->base_ok && a->vbus && b->vbus && !a->charging && !b->charging && a->q_uah >= 0 &&
		b->q_uah >= 0 && a->v_uv > 0 && b->v_uv > 0;
	double d_wh = 0, err_wh = 0;
	if (est) {
		double used = (double)(a->q_uah - b->q_uah) * (a->v_uv + b->v_uv) / 2 / 1e12;	/* uAh x uV */
		double own = t->base_w + ue_screen_w(t, a->screen_on, a->bright) -
			ue_screen_w(t, t->base_screen, t->base_bright);
		double base = (own * awake + t->suspend_mw / 1000 * asleep) / 3600;
		double sig = a->screen_on || t->base_screen ? UE_SIGMA_ON_W : UE_SIGMA_OFF_W;
		d_wh = used - base;
		err_wh = (sig * awake + UE_SIGMA_SLEEP_W * asleep) / 3600;
		if (!t->est_run)	/* the counter's steps at either end of a run */
			err_wh += 2 * UE_Q_LSB_UAH * b->v_uv / 1e12;
	}
	t->est_run = est;

	for (int i = 0; i < t->n_dev; i++) {
		struct ue_sess *s = &t->dev[i].cur;
		s->connected_s += dt;
		s->asleep_s += asleep;
		if (powered)
			s->powered_s += dt;
		s->upper_wh += bound[i] / 3600;
		if (est) {
			double share = sum > 0 ? bound[i] / sum : 1.0 / t->n_dev;
			s->est_wh += share * d_wh;
			s->est_err_wh += share * err_wh;
			s->est_s += dt;
		}
	}
}

static inline void ue_dev_active(struct ue_tracker *t, struct ue_dev *d)
{
	char p[PATH_MAX], s[32];
	snprintf(p, sizeof(p), "%s/bus/usb/devices/%s/power/runtime_status", t->sys, d->name);
	d->active = !ue_read(p, s, sizeof(s)) || strcmp(s, "suspended");
}

/* Account up to E, and read what changes the next interval */
static inline void ue_track_advance(struct ue_tracker *t, const struct ue_env *e)
{
	if (t->have_last)
		ue_interval(t, &t->last, e);
	t->last = *e;
	t->have_last = true;
	for (int i = 0; i < t->n_dev; i++) {
		ue_dev_active(t, &t->dev[i]);
		ue_store_dev(t, &t->dev[i], e->wall);
	}
}

/* sysfs device names: "1-1", "1-1.2"; not "usb1" (a root hub) or "1-1:1.0" */
static inline bool ue_is_device_name(const char *n)
{
	return n[0] >= '0' && n[0] <= '9' && strchr(n, '-') && !strchr(n, ':');
}

static inline int ue_dev_index(const struct ue_tracker *t, const char *name)
{
	for (int i = 0; i < t->n_dev; i++)
		if (!strcmp(t->dev[i].name, name))
			return i;
	return -1;
}

/* A device appeared (call ue_track_advance first) */
static inline void ue_track_add(struct ue_tracker *t, const struct ue_env *e, const char *name, bool fresh)
{
	if (!ue_is_device_name(name) || ue_dev_index(t, name) >= 0 || t->n_dev == UE_MAX_DEV)
		return;
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/bus/usb/devices/%s", t->sys, name);
	char vid[8] = "", pid[8] = "", serial[64] = "", mfr[64] = "", prod[96] = "", pw[16] = "";
	if (!ue_read_attr(dir, "idVendor", vid, sizeof(vid)) || !ue_read_attr(dir, "idProduct", pid, sizeof(pid)))
		return;	/* gone already, or not a device */
	ue_read_attr(dir, "serial", serial, sizeof(serial));
	ue_read_attr(dir, "manufacturer", mfr, sizeof(mfr));
	ue_read_attr(dir, "product", prod, sizeof(prod));
	ue_read_attr(dir, "bMaxPower", pw, sizeof(pw));

	/* the first device on the port: the phone's own draw from just before */
	if (!t->n_dev) {
		if (!fresh)
			t->base_ok = false;	/* it was in when we started: no before */
		else if (!(t->base_at > 0 && e->boot_s - t->base_at <= UE_BASE_FRESH_S))
			ue_take_baseline(t, e);
	}

	struct ue_dev *d = &t->dev[t->n_dev++];
	memset(d, 0, sizeof(*d));
	snprintf(d->name, sizeof(d->name), "%s", name);
	d->max_ma = atoi(pw);
	ue_make_key(d->key, sizeof(d->key), vid, pid, serial, name);
	ue_dev_active(t, d);

	struct ue_acc *a = ue_get(t->store, d->key);
	snprintf(a->vid, sizeof(a->vid), "%s", vid);
	snprintf(a->pid, sizeof(a->pid), "%s", pid);
	snprintf(a->serial, sizeof(a->serial), "%s", serial);
	snprintf(a->port, sizeof(a->port), "%s", name);
	if (mfr[0] && prod[0] && strncmp(prod, mfr, strlen(mfr)))
		snprintf(a->name, sizeof(a->name), "%s %s", mfr, prod);
	else if (prod[0] || mfr[0])
		snprintf(a->name, sizeof(a->name), "%s", prod[0] ? prod : mfr);
	else if (!a->name[0])
		snprintf(a->name, sizeof(a->name), "USB device %s:%s", vid, pid);
	a->max_power_ma = d->max_ma;
	ue_new_session(a, e->wall);
	d->cur.start = e->wall;
	ue_store_dev(t, d, e->wall);
}

/* A device went (call ue_track_advance first) */
static inline void ue_track_remove(struct ue_tracker *t, const struct ue_env *e, const char *name)
{
	int i = ue_dev_index(t, name);
	if (i < 0)
		return;
	struct ue_dev *d = &t->dev[i];
	d->cur.end = e->wall;
	ue_store_dev(t, d, e->wall);
	t->dev[i] = t->dev[--t->n_dev];
	if (!t->n_dev) {
		/* the port is empty: the next plug-in takes its own baseline */
		t->base_ok = false;
		t->base_at = 0;
		t->have_last = false;
	}
}

/* Make the tracked devices what sysfs has: at start, and after lost uevents */
static inline void ue_track_scan(struct ue_tracker *t, const struct ue_env *e, bool fresh)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/bus/usb/devices", t->sys);
	for (int i = t->n_dev - 1; i >= 0; i--) {
		char p[PATH_MAX + 40];
		snprintf(p, sizeof(p), "%s/%s/idVendor", dir, t->dev[i].name);
		if (access(p, F_OK))
			ue_track_remove(t, e, t->dev[i].name);
	}
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d)))
		if (ue_is_device_name(de->d_name))
			ue_track_add(t, e, de->d_name, fresh);
	if (d)
		closedir(d);
}

/* Every session ends (the daemon stops: shutdown or restart) */
static inline void ue_track_close_all(struct ue_tracker *t, const struct ue_env *e)
{
	ue_track_advance(t, e);
	while (t->n_dev)
		ue_track_remove(t, e, t->dev[t->n_dev - 1].name);
}

/* Sessions left open by a daemon that did not stop cleanly (a crash, a hard
 * reset): they end at their last checkpoint, with what it had. */
static inline int ue_close_stale(struct ue_store *s)
{
	int n = 0;
	for (int i = 0; i < s->n; i++) {
		struct ue_acc *a = &s->acc[i];
		for (int k = 0; k < a->n_sess; k++) {
			struct ue_sess *e = &a->sess[k];
			if (e->end)
				continue;
			e->end = e->start + (long long)e->connected_s;
			if (e->end < e->start + 1)
				e->end = e->start + 1;
			n++;
		}
	}
	return n;
}

/* ---- uevents ---- */

struct ue_uevent {
	char action[16], subsystem[32], devtype[32], name[64];
};

/* One kernel uevent (NETLINK_KOBJECT_UEVENT group 1): "ACTION@DEVPATH\0KEY=VALUE\0...".
 * True if it concerns USB devices or the USB-C port. */
static inline bool ue_uevent_parse(const char *buf, size_t n, struct ue_uevent *u)
{
	memset(u, 0, sizeof(*u));
	if (!n || !memchr(buf, '@', strnlen(buf, n)))
		return false;	/* udev's own messages start "libudev" */
	for (const char *p = buf; p < buf + n; p += strlen(p) + 1) {
		if (!strncmp(p, "ACTION=", 7))
			snprintf(u->action, sizeof(u->action), "%s", p + 7);
		else if (!strncmp(p, "SUBSYSTEM=", 10))
			snprintf(u->subsystem, sizeof(u->subsystem), "%s", p + 10);
		else if (!strncmp(p, "DEVTYPE=", 8))
			snprintf(u->devtype, sizeof(u->devtype), "%s", p + 8);
		else if (!strncmp(p, "DEVPATH=", 8)) {
			const char *slash = strrchr(p + 8, '/');
			snprintf(u->name, sizeof(u->name), "%s", slash ? slash + 1 : p + 8);
		}
	}
	if (!strcmp(u->subsystem, "usb") && !strcmp(u->devtype, "usb_device"))
		return true;
	return !strcmp(u->subsystem, "typec") && strstr(u->name, "partner");
}

/* What a uevent does (E read just now) */
static inline void ue_track_uevent(struct ue_tracker *t, const struct ue_env *e, const struct ue_uevent *u)
{
	if (!strcmp(u->subsystem, "typec")) {
		/* a partner on the port: the moment before its devices draw */
		if (!strcmp(u->action, "add") && !t->n_dev)
			ue_take_baseline(t, e);
		return;
	}
	ue_track_advance(t, e);
	if (!strcmp(u->action, "add"))
		ue_track_add(t, e, u->name, true);
	else if (!strcmp(u->action, "remove"))
		ue_track_remove(t, e, u->name);
}

#endif
