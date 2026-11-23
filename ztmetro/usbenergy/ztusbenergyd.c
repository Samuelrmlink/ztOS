// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztusbenergyd: what each USB accessory drew from the phone, kept per
 * accessory and per session after it is unplugged (ROADMAP 8.76, README.md).
 *
 * The user's service (ztusbenergyd.service). It waits on the kernel's
 * uevents and nothing else while no USB device is attached; while one is, it
 * also reads the state every 30 s on a CLOCK_MONOTONIC timer, which does not
 * wake a sleeping phone (the time asleep is CLOCK_BOOTTIME's lead over it).
 * Everything it reads is sysfs; it writes only its record:
 * ~/.local/state/ztos/usb-energy.json (usb-energy.h).
 *
 *	ztusbenergyd run	the daemon (one only: a second one exits)
 *	ztusbenergyd list	the record, as text
 *	ztusbenergyd json	the record's file
 *	ztusbenergyd state	what it reads now: VBUS, the gauge, the devices
 *
 * ZT_USB_SYSFS: another sysfs (tests). ZT_USB_ENERGY: another record.
 */
#define _GNU_SOURCE
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include "usb-energy-track.h"

static struct ue_store store;
static struct ue_tracker trk;
static char path[PATH_MAX];

static double clock_s(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void env_now(struct ue_env *e)
{
	ue_read_env(&trk, e, clock_s(CLOCK_BOOTTIME), clock_s(CLOCK_MONOTONIC), (long long)time(NULL));
}

static void save(double boot_s)
{
	if (!trk.dirty)
		return;
	if (!ue_save(path, &store))
		fprintf(stderr, "ztusbenergyd: cannot write %s: %s\n", path, strerror(errno));
	trk.dirty = false;
	trk.last_save = boot_s;
}

static void arm(int tfd)
{
	/* only while something is attached: otherwise no timer at all */
	struct itimerspec it = { 0 };
	if (trk.n_dev) {
		it.it_value.tv_sec = UE_TICK_S;
		it.it_interval.tv_sec = UE_TICK_S;
	}
	timerfd_settime(tfd, 0, &it, NULL);
}

static int open_uevents(void)
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -1;
	int size = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };	/* the kernel's own */
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Every waiting uevent; true if some were lost (read sysfs again) */
static bool uevents(int fd)
{
	char buf[8192];
	bool lost = false;
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) != 0) {
		if (n < 0) {
			if (errno == ENOBUFS) {
				lost = true;
				continue;
			}
			if (errno == EINTR)
				continue;
			break;
		}
		buf[n] = 0;
		struct ue_uevent u;
		if (!ue_uevent_parse(buf, n, &u))
			continue;
		struct ue_env e;
		env_now(&e);
		ue_track_uevent(&trk, &e, &u);
		if (!strcmp(u.action, "add") || !strcmp(u.action, "remove"))
			save(e.boot_s);
	}
	return lost;
}

static int run(void)
{
	char lock[PATH_MAX];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(lock, sizeof(lock), "%s/ztusbenergyd.lock", rt && rt[0] ? rt : "/tmp");
	int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "ztusbenergyd: already running (%s)\n", lock);
		return 1;
	}
	if (!ue_load(path, &store)) {
		/* keep what did not parse beside it, and go on with what did */
		char bad[PATH_MAX + 32];
		snprintf(bad, sizeof(bad), "%s.bad-%lld", path, (long long)time(NULL));
		rename(path, bad);
		fprintf(stderr, "ztusbenergyd: %s did not parse; kept as %s\n", path, bad);
	}
	int stale = ue_close_stale(&store);
	if (stale)
		fprintf(stderr, "ztusbenergyd: %d session(s) left open closed at their last checkpoint\n", stale);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	int nfd = open_uevents();
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (nfd < 0 || sfd < 0 || tfd < 0) {
		perror("ztusbenergyd");
		return 1;
	}

	/* what is in now has no "before": its session gives an upper bound only */
	struct ue_env e;
	env_now(&e);
	ue_track_advance(&trk, &e);
	ue_track_scan(&trk, &e, false);
	trk.dirty = trk.dirty || stale;
	save(e.boot_s);
	arm(tfd);
	fprintf(stderr, "ztusbenergyd: %d accessories on record, %d attached; %s\n", store.n, trk.n_dev, path);

	for (;;) {
		struct pollfd p[3] = { { nfd, POLLIN, 0 }, { tfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
		if (poll(p, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		int had = trk.n_dev;
		if (p[0].revents & POLLIN && uevents(nfd)) {
			env_now(&e);
			ue_track_advance(&trk, &e);
			ue_track_scan(&trk, &e, true);
			save(e.boot_s);
		}
		if (p[1].revents & POLLIN) {
			uint64_t n;
			if (read(tfd, &n, sizeof(n)) < 0) {
				/* nothing: the timer was disarmed meanwhile */
			}
			env_now(&e);
			ue_track_advance(&trk, &e);
			if (e.boot_s - trk.last_save >= UE_SAVE_S)
				save(e.boot_s);
		}
		if (p[2].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof(si)) < 0) {
				/* nothing to do: we stop either way */
			}
			break;
		}
		if ((had > 0) != (trk.n_dev > 0))
			arm(tfd);
	}
	/* stopping (a shutdown, a restart): every session ends here */
	env_now(&e);
	ue_track_close_all(&trk, &e);
	trk.dirty = true;
	save(e.boot_s);
	return 0;
}

static int list(void)
{
	if (!ue_load(path, &store)) {
		fprintf(stderr, "ztusbenergyd: %s did not parse\n", path);
		return 1;
	}
	const struct ue_acc *by[UE_MAX_ACC];
	for (int i = 0; i < store.n; i++)
		by[i] = &store.acc[i];
	qsort(by, store.n, sizeof(by[0]), ue_by_last_seen);
	if (!store.n)
		printf("No USB accessory on record yet (%s)\n", path);
	for (int i = 0; i < store.n; i++) {
		const struct ue_acc *a = by[i];
		char d[160], t1[32], t2[32], w[24];
		ue_describe(d, sizeof(d), a->wh, a->method, a->est_err_wh);
		time_t f = a->first_seen, l = a->last_seen;
		strftime(t1, sizeof(t1), "%Y-%m-%d %H:%M", localtime(&f));
		strftime(t2, sizeof(t2), "%Y-%m-%d %H:%M", localtime(&l));
		printf("%s  [%s]\n  total %s\n  %d session(s), connected %.0f min (powered %.0f, asleep %.0f), "
			"bMaxPower %d mA\n  first seen %s, last seen %s\n", a->name, a->key, d, a->n_sessions,
			a->connected_s / 60, a->powered_s / 60, a->asleep_s / 60, a->max_power_ma, t1, t2);
		printf("  upper bound %s", ue_fmt_wh(a->upper_wh, w, sizeof(w)));
		if (a->est_s > 0) {
			char w2[24];
			printf(", battery estimate %s ± %s over %.0f min",
				ue_fmt_wh(a->est_wh > 0 ? a->est_wh : 0, w, sizeof(w)),
				ue_fmt_wh(a->est_err_wh, w2, sizeof(w2)), a->est_s / 60);
		}
		printf("\n");
		for (int k = a->n_sess - 1; k >= 0 && k >= a->n_sess - 5; k--) {
			const struct ue_sess *s = &a->sess[k];
			time_t st = s->start;
			strftime(t1, sizeof(t1), "%m-%d %H:%M", localtime(&st));
			ue_describe(d, sizeof(d), s->wh, s->method, s->est_err_wh);
			printf("    %s  %5.1f min%s  %s\n", t1, s->connected_s / 60, s->end ? "" : " (now)", d);
		}
	}
	return 0;
}

/* what the daemon would read now (nothing is written) */
static int state(void)
{
	struct ue_env e;
	env_now(&e);
	printf("vbus_out=%d charging=%d screen=%s backlight=%.2f\n", e.vbus, e.charging, e.screen_on ? "on" : "off",
		e.bright);
	printf("charge_counter_uah=%lld voltage_now_uv=%.0f current_avg_ua=%.0f voltage_avg_uv=%.0f\n", e.q_uah,
		e.v_uv, e.i_avg_ua, e.v_avg_uv);
	printf("battery_power_w=%.3f (positive: discharging)\n", e.gauge_ok ? -e.i_avg_ua * e.v_avg_uv / 1e12 : 0);
	printf("profile: screen_mw=%.0f screen_full_mw=%.0f base_suspend_mw=%.0f\n", trk.screen_mw,
		trk.screen_full_mw, trk.suspend_mw);
	ue_track_scan(&trk, &e, false);
	for (int i = 0; i < trk.n_dev; i++)
		printf("device %s key=%s bMaxPower=%d mA %s\n", trk.dev[i].name, trk.dev[i].key, trk.dev[i].max_ma,
			trk.dev[i].active ? "active" : "suspended");
	return 0;
}

int main(int argc, char **argv)
{
	const char *p = getenv("ZT_USB_ENERGY");
	if (p && p[0])
		snprintf(path, sizeof(path), "%s", p);
	else
		ue_store_path(path, sizeof(path));
	ue_track_init(&trk, &store, getenv("ZT_USB_SYSFS"));
	const char *cmd = argc > 1 ? argv[1] : "";
	if (!strcmp(cmd, "run"))
		return run();
	if (!strcmp(cmd, "list"))
		return list();
	if (!strcmp(cmd, "state")) {
		static struct ue_store scratch;	/* the record is not touched */
		trk.store = &scratch;
		return state();
	}
	if (!strcmp(cmd, "json")) {
		execlp("cat", "cat", path, (char *)NULL);
		return 1;
	}
	fprintf(stderr, "usage: ztusbenergyd run|list|json\n"
		"  run   the daemon (normally ztusbenergyd.service: systemctl --user)\n"
		"  list  what each USB accessory drew, from %s\n"
		"  state what it reads now (VBUS, charger, screen, gauge, devices)\n", path);
	return 2;
}
