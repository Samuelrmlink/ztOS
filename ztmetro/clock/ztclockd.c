// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztclockd: rings ztclock's alarms and its timer, with the app closed and the
 * screen off, and from SoC sleep (clock/README.md has the whole of it).
 *
 * The app writes ~/.config/ztclock/{alarms,timer}; this reads them whenever
 * they change (inotify) and arms one timerfd for the soonest thing due, on
 * CLOCK_REALTIME_ALARM: the kernel's alarmtimer programs the RTC with it as
 * the phone suspends, so the SoC wakes for it. That needs CAP_WAKE_ALARM,
 * which this program gets as a file capability (make install: setcap); a
 * user service cannot be given it otherwise. Without it the timer is a plain
 * CLOCK_REALTIME one, which fires on time only while the phone is awake.
 *
 * While it rings it holds zrn-sleepd off: /run/zrn-sleepd/inhibit/timed-ztclock,
 * written before anything rings, its modification time the end of the ringing
 * (so a crash cannot hold the phone up for ever), removed on snooze and
 * dismiss. The directory is root's; ztclock.tmpfiles gives the user an ACL.
 *
 * The timer is absolute and TFD_TIMER_CANCEL_ON_SET: setting the clock
 * cancels it and everything is worked out again. A change of time zone
 * (/etc/localtime) does the same, and each alarm's next time comes from
 * mktime for that day, so a daylight-saving change is in it.
 *
 * Ringing: the screen on, the shell's alarm screen over the lock
 * ("alarm_screen" on its control pipe: ztclock --ring), the sound looping
 * through PipeWire (pw-play, as ztphone rings) getting louder over its first
 * half minute, the vibration (core/zrnvibe.h, the ring's pattern), until
 * snooze or dismiss, or CLOCK_AUTOSTOP minutes.
 *
 *	ztclockd		run (systemd --user: ztclockd.service)
 *	ztclockd --status	what is armed, from the files
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "clock-store.h"
#include "zrnvibe.h"

#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif

#define LATE_MAX_S	(30 * 60)	/* rung late (a sleep that overran, the clock set
					 * forward) up to this; older is let go */
#define UI_WAIT_S	8		/* no ring screen by then: start one ourselves */
#define SCREEN_KEEP_S	20		/* "screen on" again this often while it rings */
#define RAMP_S		30		/* the sound grows from 30 % to full over this */

static int alarm_fd = -1, tick_fd = -1, ino_fd = -1, sock_fd = -1;
static bool wake_clock;		/* alarm_fd is CLOCK_REALTIME_ALARM */
static time_t checked;		/* everything due up to this has been dealt with */

static struct clock_alarm alarms[CLOCK_MAX_ALARMS];
static int n_alarms;
struct snooze { int id; time_t at; };
static struct snooze snoozes[CLOCK_MAX_ALARMS];
static int n_snoozes;
static struct clock_timer timer;

/* what rings */
static struct {
	bool on;
	char kind;		/* 'a' an alarm, 't' the timer */
	int id;
	char label[CLOCK_LABEL_MAX];
	char sound[64];
	bool vibrate;
	int snooze;
	time_t for_time;	/* what it rang for */
	time_t started, until;
	time_t screen_told;
	bool ui_seen, ui_started;
	pid_t sound_pid, vibe_pid, ui_pid;
} ring;

static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);	/* the journal */
	va_end(ap);
	fputc('\n', stderr);
}

#define INHIBIT_DIR	"/run/zrn-sleepd/inhibit"
#define INHIBIT		INHIBIT_DIR "/timed-ztclock"

/* zrn-sleepd's inhibit, until `until` (wall clock) */
static void inhibit_hold(time_t until) {
	char tmp[] = INHIBIT_DIR "/.ztclock.new";
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "inhibit: %s: %s (the phone may sleep while it rings)\n", tmp, strerror(errno));
		return;
	}
	dprintf(fd, "ztclock ringing\n");
	struct timespec times[2] = { { until, 0 }, { until, 0 } };
	futimens(fd, times);
	close(fd);
	if (rename(tmp, INHIBIT) < 0) fprintf(stderr, "inhibit: %s: %s\n", INHIBIT, strerror(errno));
}

static void inhibit_release(void) {
	unlink(INHIBIT);
}

static void tell_shell(const char *line) {
	char path[300];
	clock_runtime_path(path, sizeof(path), "ztmetro.ctl");
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;
	if (write(fd, line, strlen(line)) < 0) {}
	close(fd);
}

static void hhmm(time_t t, char *out, size_t size) {
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, size, "%a %Y-%m-%d %H:%M:%S %Z", &tm);
}

/* glibc reads /etc/localtime once: make it read it again */
static void zone_reload(void) {
	setenv("TZ", "UTC0", 1);
	tzset();
	unsetenv("TZ");
	tzset();
}

static void snoozes_load(void) {
	char path[512];
	clock_path(path, sizeof(path), "snooze");
	n_snoozes = 0;
	FILE *f = fopen(path, "r");
	int id;
	long long at;
	while (f && n_snoozes < CLOCK_MAX_ALARMS && fscanf(f, "%d %lld", &id, &at) == 2)
		snoozes[n_snoozes++] = (struct snooze){ id, (time_t)at };
	if (f) fclose(f);
}

static void snoozes_save(void) {
	char path[512], tmp[520];
	clock_path(path, sizeof(path), "snooze");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	for (int i = 0; i < n_snoozes; i++) fprintf(f, "%d %lld\n", snoozes[i].id, (long long)snoozes[i].at);
	if (fclose(f) == 0) rename(tmp, path);
}

static void snooze_drop(int id) {
	int k = 0;
	for (int i = 0; i < n_snoozes; i++)
		if (snoozes[i].id != id) snoozes[k++] = snoozes[i];
	if (k != n_snoozes) {
		n_snoozes = k;
		snoozes_save();
	}
}

static void load_all(void) {
	int lock = clock_lock();
	n_alarms = clock_alarms_load(alarms, CLOCK_MAX_ALARMS);
	clock_unlock(lock);
	snoozes_load();
	clock_timer_load(&timer);
	/* a snooze of an alarm that is gone goes with it. (One switched off stays
	 * snoozed: a one-time alarm is switched off as it rings, and its snooze
	 * must still come. The app sends "unsnooze ID" when someone switches one
	 * off or changes it.) */
	int k = 0;
	for (int i = 0; i < n_snoozes; i++) {
		bool keep = false;
		for (int a = 0; a < n_alarms; a++) keep |= alarms[a].id == snoozes[i].id;
		if (keep) snoozes[k++] = snoozes[i];
	}
	if (k != n_snoozes) {
		n_snoozes = k;
		snoozes_save();
	}
}

/* ---- ringing ------------------------------------------------------------ */

static void ring_file_write(void) {
	char path[300], tmp[310], when[16];
	struct tm tm;
	localtime_r(&ring.for_time, &tm);
	snprintf(when, sizeof(when), "%02d:%02d", tm.tm_hour, tm.tm_min);
	clock_runtime_path(path, sizeof(path), "ztclock.ring");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%s\n%d\n%s\n%s\n%d\n", ring.kind == 't' ? "timer" : "alarm", ring.id, ring.label, when,
		ring.kind == 't' ? 0 : ring.snooze);
	if (fclose(f) == 0) rename(tmp, path);
}

/* The sound, over and over, louder over the first RAMP_S: a process group of
 * its own, so one kill stops it and the pw-play in it */
static pid_t sound_start(const char *file, int volume) {
	char path[300];
	snprintf(path, sizeof(path), "%s%s", CLOCK_SOUNDS, file);
	if (access(path, R_OK)) snprintf(path, sizeof(path), "%s%s", CLOCK_SOUNDS, CLOCK_SOUND_DEFAULT);
	if (access(path, R_OK)) snprintf(path, sizeof(path), "%sNokia-Tune.wav", CLOCK_SOUNDS);
	pid_t pid = fork();
	if (pid != 0) {
		if (pid > 0) setpgid(pid, pid);
		return pid > 0 ? pid : 0;
	}
	setpgid(0, 0);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	signal(SIGTERM, SIG_DFL);
	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double s = (double)(now.tv_sec - t0.tv_sec), share = 0.3 + 0.7 * (s >= RAMP_S ? 1 : s / RAMP_S);
		char vol[24];
		snprintf(vol, sizeof(vol), "%.2f", volume / 100.0 * share);
		pid_t child = fork();
		if (child == 0) {
			prctl(PR_SET_PDEATHSIG, SIGTERM);
			execlp("pw-play", "pw-play", "--volume", vol, path, (char *)NULL);
			_exit(127);
		}
		int status = 0;
		if (child < 0 || waitpid(child, &status, 0) < 0 || status != 0)
			sleep(1);	/* PipeWire not ready yet after a wake: again shortly */
	}
}

static void ring_ui_start(void) {
	/* the shell (with alarm_screen) starts it over the lock screen; an older
	 * shell does not know the word, and then it is started from here, to be
	 * seen once the phone is unlocked */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) dup2(null, 0);
		execlp("ztclock", "ztclock", "--ring", (char *)NULL);
		_exit(127);
	}
	ring.ui_pid = pid > 0 ? pid : 0;
	ring.ui_started = true;
	say("ring: no ring screen after %d s: started ztclock --ring itself", UI_WAIT_S);
}

static void next_write(void);

static void ring_stop(const char *why) {
	if (!ring.on) return;
	if (ring.sound_pid > 0) {
		kill(-ring.sound_pid, SIGTERM);
		waitpid(ring.sound_pid, NULL, 0);
	}
	vibe_stop(&ring.vibe_pid);
	inhibit_release();
	char path[300];
	clock_runtime_path(path, sizeof(path), "ztclock.ring");
	unlink(path);
	tell_shell("alarm_screen end\n");
	struct itimerspec off = { 0 };
	timerfd_settime(tick_fd, 0, &off, NULL);
	say("ring: %s %d stopped (%s) after %ld s", ring.kind == 't' ? "timer" : "alarm", ring.id, why,
		(long)(time(NULL) - ring.started));
	ring.on = false;
	ring.sound_pid = 0;
	next_write();
}

static void ring_start(char kind, const struct clock_alarm *a, time_t for_time) {
	if (ring.on) ring_stop("another rings");
	memset(&ring, 0, sizeof(ring));
	ring.on = true;
	ring.kind = kind;
	ring.for_time = for_time;
	ring.started = time(NULL);
	ring.until = ring.started + 60 * clock_setting("autostop", CLOCK_AUTOSTOP_DEFAULT);
	if (a) {
		ring.id = a->id;
		snprintf(ring.label, sizeof(ring.label), "%s", a->label);
		snprintf(ring.sound, sizeof(ring.sound), "%s", a->sound);
		ring.vibrate = a->vibrate;
		ring.snooze = a->snooze;
	} else {
		ring.id = 0;
		snprintf(ring.label, sizeof(ring.label), "Timer");
		snprintf(ring.sound, sizeof(ring.sound), "%s", CLOCK_SOUND_DEFAULT);
		ring.vibrate = true;
	}
	/* first what keeps the phone up, then the rest */
	inhibit_hold(ring.until + 30);
	next_write();
	ring_file_write();
	tell_shell("screen on\n");
	tell_shell("alarm_screen\n");
	ring.screen_told = ring.started;
	int volume = clock_setting("volume", CLOCK_VOLUME_DEFAULT);
	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;
	if (volume > 0) ring.sound_pid = sound_start(ring.sound, volume);
	if (ring.vibrate) {
		int r, s;
		char pattern[40];
		vibe_config_load(&r, &s, pattern, sizeof(pattern));
		if (!strcmp(pattern, "off")) snprintf(pattern, sizeof(pattern), "long");
		ring.vibe_pid = vibe_start(pattern, ring.sound, false);
	}
	struct itimerspec every = { { 1, 0 }, { 1, 0 } };
	timerfd_settime(tick_fd, 0, &every, NULL);
	char when[64];
	hhmm(for_time, when, sizeof(when));
	say("ring: %s %d \"%s\" for %s, volume %d%s", kind == 't' ? "timer" : "alarm", ring.id, ring.label, when,
		volume, ring.vibrate ? ", vibrating" : "");
}

static void ring_tick(void) {
	uint64_t n;
	if (read(tick_fd, &n, sizeof(n)) < 0) {}
	if (!ring.on) return;
	time_t now = time(NULL);
	if (now >= ring.until) {
		ring_stop("rang its full time");
		return;
	}
	if (!ring.ui_seen && !ring.ui_started && now - ring.started >= UI_WAIT_S) ring_ui_start();
	if (now - ring.screen_told >= SCREEN_KEEP_S) {
		tell_shell("screen on\n");
		ring.screen_told = now;
	}
}

/* ---- the schedule ------------------------------------------------------- */

/* what the app and zrn-sleepd see of it; `wake` is also what is armed */
static time_t armed_for;

static time_t next_alarm_time(time_t after, bool with_snoozes) {
	time_t best = 0;
	for (int i = 0; i < n_alarms; i++) {
		if (!alarms[i].on) continue;
		time_t t = clock_alarm_next(&alarms[i], after);
		if (t > 0 && (!best || t < best)) best = t;
	}
	for (int i = 0; with_snoozes && i < n_snoozes; i++)
		if (snoozes[i].at > after && (!best || snoozes[i].at < best)) best = snoozes[i].at;
	return best;
}

static void next_write(void) {
	char path[300], tmp[310];
	clock_runtime_path(path, sizeof(path), "ztclock.next");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "wake %lld\nalarm %lld\nringing %lld\nclock %s\n", (long long)armed_for,
		(long long)next_alarm_time(time(NULL), true), (long long)(ring.on ? ring.until : 0),
		wake_clock ? "realtime-alarm" : "realtime");
	if (fclose(f) == 0) rename(tmp, path);
}

static void arm(void) {
	time_t now = time(NULL), best = next_alarm_time(checked > now ? checked : now, true);
	struct itimerspec it = { 0 };
	if (timer.state == CLOCK_TIMER_RUNNING) {
		time_t end = (time_t)((timer.value_ms + 999) / 1000);
		if (!best || end < best) best = end;
	}
	armed_for = best;
	if (best) {
		it.it_value.tv_sec = best;
		if (timer.state == CLOCK_TIMER_RUNNING && (time_t)((timer.value_ms + 999) / 1000) == best) {
			it.it_value.tv_sec = (time_t)(timer.value_ms / 1000);
			it.it_value.tv_nsec = (long)(timer.value_ms % 1000) * 1000000L;
		}
	}
	if (timerfd_settime(alarm_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL) < 0)
		say("timerfd_settime: %s", strerror(errno));
	char when[64] = "nothing";
	if (best) hhmm(best, when, sizeof(when));
	say("armed: %s (%s)", when, wake_clock ? "CLOCK_REALTIME_ALARM" : "CLOCK_REALTIME");
	next_write();
}

/* Everything due in (checked, now]: the newest of it rings; one-time alarms
 * that came due are switched off, snoozes and the timer used up. */
static void fire_due(void) {
	time_t now = time(NULL);
	time_t from = checked;
	if (from < now - LATE_MAX_S) from = now - LATE_MAX_S;
	if (from >= now) {
		checked = now;
		return;
	}
	char ring_kind = 0;
	struct clock_alarm *ring_alarm = NULL;
	time_t ring_time = 0;
	bool once_done = false;
	for (int i = 0; i < n_alarms; i++) {
		if (!alarms[i].on) continue;
		time_t t = clock_alarm_next(&alarms[i], from);
		if (t <= 0 || t > now) continue;
		if (t >= ring_time) {
			ring_kind = 'a';
			ring_alarm = &alarms[i];
			ring_time = t;
		}
		if (!alarms[i].days) once_done = true;
	}
	int k = 0;
	for (int i = 0; i < n_snoozes; i++) {
		if (snoozes[i].at <= now) {
			if (snoozes[i].at > from) {
				for (int a = 0; a < n_alarms; a++) {
					if (alarms[a].id == snoozes[i].id && snoozes[i].at >= ring_time) {
						ring_kind = 'a';
						ring_alarm = &alarms[a];
						ring_time = snoozes[i].at;
					}
				}
			}
			continue;	/* used up, or too old */
		}
		snoozes[k++] = snoozes[i];
	}
	if (k != n_snoozes) {
		n_snoozes = k;
		snoozes_save();
	}
	if (timer.state == CLOCK_TIMER_RUNNING && timer.value_ms <= (int64_t)now * 1000 + 999) {
		time_t end = (time_t)(timer.value_ms / 1000);
		if (end > now - LATE_MAX_S && end >= ring_time) {
			ring_kind = 't';
			ring_alarm = NULL;
			ring_time = end;
		}
		timer.state = CLOCK_TIMER_IDLE;
		timer.value_ms = 0;
		clock_timer_save(&timer);
	}
	struct clock_alarm rung;
	if (ring_alarm) rung = *ring_alarm;
	if (once_done) {
		/* switched off in the file, under the app's lock, as it is there now */
		int lock = clock_lock();
		struct clock_alarm now_there[CLOCK_MAX_ALARMS];
		int n = clock_alarms_load(now_there, CLOCK_MAX_ALARMS);
		for (int i = 0; i < n; i++) {
			if (!now_there[i].on || now_there[i].days) continue;
			time_t t = clock_alarm_next(&now_there[i], from);
			if (t > 0 && t <= now) now_there[i].on = false;
		}
		clock_alarms_save(now_there, n);
		memcpy(alarms, now_there, sizeof(now_there[0]) * (size_t)n);
		n_alarms = n;
		clock_unlock(lock);
	}
	checked = now;
	if (ring_kind) ring_start(ring_kind, ring_alarm ? &rung : NULL, ring_time);
}

static void alarm_fired(void) {
	uint64_t n;
	ssize_t r = read(alarm_fd, &n, sizeof(n));
	if (r < 0 && errno == ECANCELED) {
		/* the clock was set: what is due is worked out again from here */
		time_t now = time(NULL);
		say("the clock was set");
		if (checked > now) checked = now;	/* set back: nothing rings twice */
	} else if (r < 0) {
		return;
	}
	fire_due();
	arm();
}

static void reload(const char *why) {
	say("reload (%s)", why);
	load_all();
	fire_due();
	arm();
}

static void snooze_now(void) {
	if (!ring.on) return;
	if (ring.kind == 'a' && ring.snooze > 0) {
		snooze_drop(ring.id);
		if (n_snoozes < CLOCK_MAX_ALARMS) {
			snoozes[n_snoozes++] = (struct snooze){ ring.id, time(NULL) + 60 * ring.snooze };
			snoozes_save();
		}
		ring_stop("snoozed");
		say("snoozed %d min", ring.snooze);
	} else {
		ring_stop("dismissed");
	}
	arm();
}

static void command(void) {
	char buf[128];
	ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
	if (n <= 0) return;
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	int pid;
	if (!strcmp(buf, "snooze")) {
		snooze_now();
	} else if (!strcmp(buf, "dismiss")) {
		if (ring.on && ring.kind == 'a') snooze_drop(ring.id);
		ring_stop("dismissed");
		arm();
	} else if (!strcmp(buf, "reload")) {
		reload("asked");
	} else if (sscanf(buf, "unsnooze %d", &pid) == 1) {
		snooze_drop(pid);
		arm();
	} else if (sscanf(buf, "hello %d", &pid) == 1) {
		ring.ui_seen = true;
		if (ring.on) ring_file_write();
	}
}

static void inotified(void) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n = read(ino_fd, buf, sizeof(buf));
	bool config = false, zone = false;
	for (char *p = buf; n > 0 && p < buf + n;) {
		struct inotify_event *e = (struct inotify_event *)p;
		if (e->len && (!strcmp(e->name, "alarms") || !strcmp(e->name, "timer") || !strcmp(e->name, "settings")))
			config = true;
		if (e->len && (!strcmp(e->name, "localtime") || !strcmp(e->name, "timezone"))) zone = true;
		p += sizeof(*e) + e->len;
	}
	if (zone) {
		zone_reload();
		say("the time zone changed");
	}
	if (config || zone) reload(zone ? "time zone" : "files");
}

static int dir_watch_cfg = -1;

static int status(void) {
	long long wake = clock_next_value("wake"), next = clock_next_value("alarm"), ringing = clock_next_value("ringing");
	char a[64] = "none", b[64] = "none";
	if (wake) hhmm((time_t)wake, a, sizeof(a));
	if (next) hhmm((time_t)next, b, sizeof(b));
	printf("armed for: %s\nnext alarm: %s\nringing: %s\n", a, b, ringing ? "yes" : "no");
	char path[300], line[64];
	clock_runtime_path(path, sizeof(path), "ztclock.next");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "clock ", 6)) printf("timer clock: %s", line + 6);
	if (f) fclose(f);
	return 0;
}

static volatile sig_atomic_t quit;
static void on_term(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "--status")) return status();
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);
	struct sigaction sa = { .sa_handler = on_term };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	alarm_fd = timerfd_create(CLOCK_REALTIME_ALARM, TFD_CLOEXEC | TFD_NONBLOCK);
	wake_clock = alarm_fd >= 0;
	if (alarm_fd < 0) {
		say("CLOCK_REALTIME_ALARM: %s: alarms cannot wake the SoC by themselves "
			"(zrn-sleepd still keeps the phone up for them through ztclock.next)", strerror(errno));
		alarm_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	}
	tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (alarm_fd < 0 || tick_fd < 0) {
		say("timerfd: %s", strerror(errno));
		return 1;
	}

	ino_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	char cfg[512];
	clock_path(cfg, sizeof(cfg), "alarms");
	*strrchr(cfg, '/') = 0;
	dir_watch_cfg = inotify_add_watch(ino_fd, cfg, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
	inotify_add_watch(ino_fd, "/etc", IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB);

	char sock_path[300];
	clock_runtime_path(sock_path, sizeof(sock_path), "ztclock.sock");
	unlink(sock_path);
	sock_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (sock_fd < 0 || bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		say("%s: %s", sock_path, strerror(errno));
		return 1;
	}
	chmod(sock_path, 0600);

	checked = time(NULL);
	load_all();
	/* a timer that ran out while the phone was off rings now, if not long ago */
	if (timer.state == CLOCK_TIMER_RUNNING && timer.value_ms <= clock_now_ms()) checked = (time_t)(timer.value_ms / 1000) - 1;
	fire_due();
	arm();
	say("ztclockd: %d alarms, timer %s", n_alarms,
		timer.state == CLOCK_TIMER_RUNNING ? "running" : timer.state == CLOCK_TIMER_PAUSED ? "paused" : "idle");

	struct pollfd p[4] = {
		{ .fd = alarm_fd, .events = POLLIN }, { .fd = tick_fd, .events = POLLIN },
		{ .fd = ino_fd, .events = POLLIN }, { .fd = sock_fd, .events = POLLIN },
	};
	while (!quit) {
		if (poll(p, 4, -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (p[0].revents & POLLIN) alarm_fired();
		if (p[1].revents & POLLIN) ring_tick();
		if (p[2].revents & POLLIN) inotified();
		if (p[3].revents & POLLIN) command();
		while (waitpid(-1, NULL, WNOHANG) > 0) {}
	}
	ring_stop("ztclockd stopping");
	char path[300];
	clock_runtime_path(path, sizeof(path), "ztclock.next");
	unlink(path);
	unlink(sock_path);
	return 0;
}
