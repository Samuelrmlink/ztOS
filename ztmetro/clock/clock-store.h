/*
 * clock-store.h: what ztclock (the app) and ztclockd (the daemon that rings)
 * share: the alarms, the timer and the files between them. Header only.
 *
 * ~/.config/ztclock/alarms	the alarms, one a line, written by the app:
 *	ID ON HH:MM DAYS VIBRATE SNOOZE_MIN SOUND LABEL...
 *	DAYS is seven 0/1, Monday first; 0000000 rings once (and the daemon
 *	switches it off when it has rung). SOUND is a file in CLOCK_SOUNDS.
 * ~/.config/ztclock/timer	the countdown: "running END_MS TOTAL_MS",
 *	"paused LEFT_MS TOTAL_MS" or "idle 0 TOTAL_MS" (END_MS: wall clock, ms)
 * ~/.config/ztclock/snooze	alarms snoozed, "ID EPOCH" a line (the daemon's)
 * ~/.config/ztclock/stopwatch	the stopwatch (the app's alone)
 * ~/.config/ztclock/zones	the world clock's zones, one a line
 * ~/.config/ztclock/settings	"volume N" (alarm volume, 0-100), "autostop MIN"
 *
 * $XDG_RUNTIME_DIR/ztclock.next	from the daemon, replaced whole:
 *	wake EPOCH	the soonest thing that must wake the phone (0: none)
 *	alarm EPOCH	the next alarm (not a timer): the lock screen shows it
 *	ringing EPOCH	ringing, and stops by itself then (0: not ringing)
 *	clock realtime-alarm|realtime	which timer it armed (the first wakes
 *			the SoC from suspend; the second needs zrn-sleepd's help)
 *   (the shell's lock screen reads `alarm`; anything else may read `wake`)
 * $XDG_RUNTIME_DIR/ztclock.ring	while it rings: kind, id, label, the time
 *	it rang for, the snooze minutes (0: no snooze, a timer), one a line
 * $XDG_RUNTIME_DIR/ztclock.sock	datagrams to the daemon: "snooze",
 *	"dismiss", "reload", "unsnooze ID" (switched off, changed or deleted
 *	in the app), "hello PID" (a ring screen is up)
 */
#ifndef CLOCK_STORE_H
#define CLOCK_STORE_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CLOCK_SOUNDS		"/usr/local/share/sounds/ztos/phone/"
#define CLOCK_SOUND_DEFAULT	"Ring03-WP8.wav"
#define CLOCK_MAX_ALARMS	32
#define CLOCK_LABEL_MAX		64
#define CLOCK_SNOOZE_DEFAULT	10
#define CLOCK_VOLUME_DEFAULT	70
#define CLOCK_AUTOSTOP_DEFAULT	10	/* minutes of ringing, then it stops by itself */

struct clock_alarm {
	int id;
	bool on;
	int hour, minute;
	unsigned days;		/* bit 0 Monday .. bit 6 Sunday; 0: once */
	bool vibrate;
	int snooze;		/* minutes */
	char sound[64];
	char label[CLOCK_LABEL_MAX];
};

static inline const char *clock_home(void) {
	const char *h = getenv("HOME");
	return h && h[0] ? h : "/tmp";
}

static inline const char *clock_runtime(void) {
	static char fallback[32];
	const char *r = getenv("XDG_RUNTIME_DIR");
	if (r && r[0]) return r;
	snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
	return fallback;
}

/* ~/.config/ztclock/NAME (made if missing) */
static inline void clock_path(char *out, size_t size, const char *name) {
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/.config", clock_home());
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/ztclock", clock_home());
	mkdir(dir, 0755);
	snprintf(out, size, "%s/%s", dir, name);
}

static inline void clock_runtime_path(char *out, size_t size, const char *name) {
	snprintf(out, size, "%s/%s", clock_runtime(), name);
}

/* A sound's file name is a plain name in CLOCK_SOUNDS, nothing else. */
static inline bool clock_sound_ok(const char *s) {
	if (!s[0] || s[0] == '.') return false;
	return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(s);
}

/* The alarms file is written by the app and, for a one-time alarm that has
 * rung, by the daemon: both hold this lock while they read and write it. */
static inline int clock_lock(void) {
	char path[512];
	clock_path(path, sizeof(path), ".lock");
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) flock(fd, LOCK_EX);
	return fd;
}

static inline void clock_unlock(int fd) {
	if (fd >= 0) {
		flock(fd, LOCK_UN);
		close(fd);
	}
}

static inline int clock_alarms_load(struct clock_alarm *a, int max) {
	char path[512], line[256];
	clock_path(path, sizeof(path), "alarms");
	FILE *f = fopen(path, "r");
	int n = 0;
	while (f && n < max && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0]) continue;
		struct clock_alarm x = { 0 };
		int on = 0, vib = 1, used = 0;
		char days[16] = "", sound[64] = "";
		if (sscanf(line, "%d %d %d:%d %15s %d %d %63s %n", &x.id, &on, &x.hour, &x.minute, days,
				&vib, &x.snooze, sound, &used) < 8)
			continue;
		if (x.hour < 0 || x.hour > 23 || x.minute < 0 || x.minute > 59) continue;
		x.on = on != 0;
		x.vibrate = vib != 0;
		for (int d = 0; d < 7 && days[d]; d++)
			if (days[d] == '1') x.days |= 1u << d;
		if (x.snooze < 1 || x.snooze > 60) x.snooze = CLOCK_SNOOZE_DEFAULT;
		snprintf(x.sound, sizeof(x.sound), "%s", clock_sound_ok(sound) ? sound : CLOCK_SOUND_DEFAULT);
		if (used > 0) snprintf(x.label, sizeof(x.label), "%s", line + used);
		a[n++] = x;
	}
	if (f) fclose(f);
	return n;
}

static inline bool clock_alarms_save(const struct clock_alarm *a, int n) {
	char path[512], tmp[520];
	clock_path(path, sizeof(path), "alarms");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	fprintf(f, "# ID ON HH:MM DAYS(Mon..Sun) VIBRATE SNOOZE_MIN SOUND LABEL (ztclock)\n");
	for (int i = 0; i < n; i++) {
		char days[8];
		for (int d = 0; d < 7; d++) days[d] = a[i].days & (1u << d) ? '1' : '0';
		days[7] = 0;
		char label[CLOCK_LABEL_MAX];
		snprintf(label, sizeof(label), "%s", a[i].label);
		for (char *p = label; *p; p++)
			if (*p == '\n' || *p == '\r') *p = ' ';
		fprintf(f, "%d %d %02d:%02d %s %d %d %s %s\n", a[i].id, a[i].on, a[i].hour, a[i].minute, days,
			a[i].vibrate, a[i].snooze, clock_sound_ok(a[i].sound) ? a[i].sound : CLOCK_SOUND_DEFAULT,
			label);
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	return ok && rename(tmp, path) == 0;
}

/* The first time after `after` (wall clock) that alarm `a` rings, in local
 * time as it is now: each day's hh:mm is made with mktime, so a day of a
 * daylight-saving change has its own offset. A time a change skips (02:30 on
 * the spring night) comes out as mktime makes it, an hour on. -1: none. */
static inline time_t clock_alarm_next(const struct clock_alarm *a, time_t after) {
	struct tm base;
	localtime_r(&after, &base);
	for (int d = 0; d <= 8; d++) {
		struct tm tm = { 0 };
		tm.tm_year = base.tm_year;
		tm.tm_mon = base.tm_mon;
		tm.tm_mday = base.tm_mday + d;
		tm.tm_hour = 12;	/* the day itself, first: its weekday */
		tm.tm_isdst = -1;
		if (mktime(&tm) == (time_t)-1) continue;
		int monday0 = (tm.tm_wday + 6) % 7;
		if (a->days && !(a->days & (1u << monday0))) continue;
		tm.tm_hour = a->hour;
		tm.tm_min = a->minute;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		time_t t = mktime(&tm);
		if (t != (time_t)-1 && t > after) return t;
	}
	return -1;
}

/* The countdown */
enum { CLOCK_TIMER_IDLE, CLOCK_TIMER_RUNNING, CLOCK_TIMER_PAUSED };
struct clock_timer {
	int state;
	int64_t value_ms;	/* running: when it ends (wall clock); paused: what is left */
	int64_t total_ms;	/* what it was set to */
};

static inline int64_t clock_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void clock_timer_load(struct clock_timer *t) {
	char path[512], word[16] = "";
	long long v = 0, total = 0;
	*t = (struct clock_timer){ CLOCK_TIMER_IDLE, 0, 5 * 60 * 1000 };
	clock_path(path, sizeof(path), "timer");
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fscanf(f, "%15s %lld %lld", word, &v, &total) == 3) {
		t->state = !strcmp(word, "running") ? CLOCK_TIMER_RUNNING :
			!strcmp(word, "paused") ? CLOCK_TIMER_PAUSED : CLOCK_TIMER_IDLE;
		t->value_ms = v;
		if (total > 0) t->total_ms = total;
	}
	fclose(f);
}

static inline bool clock_timer_save(const struct clock_timer *t) {
	char path[512], tmp[520];
	clock_path(path, sizeof(path), "timer");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	static const char *const words[] = { "idle", "running", "paused" };
	fprintf(f, "%s %lld %lld\n", words[t->state], (long long)t->value_ms, (long long)t->total_ms);
	bool ok = fclose(f) == 0;
	return ok && rename(tmp, path) == 0;
}

/* "volume N" and "autostop MIN" */
static inline int clock_setting(const char *key, int fallback) {
	char path[512], line[128], k[32];
	int v, value = fallback;
	clock_path(path, sizeof(path), "settings");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %d", k, &v) == 2 && !strcmp(k, key)) value = v;
	if (f) fclose(f);
	return value;
}

static inline void clock_setting_set(const char *key, int value) {
	char path[512], tmp[520], line[128], k[32];
	clock_path(path, sizeof(path), "settings");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
	if (!out) {
		if (in) fclose(in);
		return;
	}
	while (in && fgets(line, sizeof(line), in))
		if (sscanf(line, "%31s", k) != 1 || strcmp(k, key)) fputs(line, out);
	fprintf(out, "%s %d\n", key, value);
	if (in) fclose(in);
	if (fclose(out) == 0) rename(tmp, path);
}

/* A word to the daemon; false if it is not there */
static inline bool clock_tell(const char *what) {
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return false;
	struct sockaddr_un to = { .sun_family = AF_UNIX };
	snprintf(to.sun_path, sizeof(to.sun_path), "%s/ztclock.sock", clock_runtime());
	bool ok = sendto(fd, what, strlen(what), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) >= 0;
	close(fd);
	return ok;
}

/* one "KEY EPOCH" line of ztclock.next; 0 if absent */
static inline long long clock_next_value(const char *key) {
	char path[300], line[96], k[32];
	long long v, value = 0;
	clock_runtime_path(path, sizeof(path), "ztclock.next");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %lld", k, &v) == 2 && !strcmp(k, key)) value = v;
	if (f) fclose(f);
	return value;
}

#endif
