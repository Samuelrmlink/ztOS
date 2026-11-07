// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zrn-ledd: the one program that writes the front LED.
 *
 * Others do not write /sys/class/leds; each says what it wants in a small file
 * of its own under /run/zrn-led, and this shows the most important of them
 * (the rules: led-policy.h).
 *
 *	/run/zrn-led/charge		zrn-charge: none, charging, full or low
 *	/run/zrn-led/boost		zrnperfd: 1 at the top boost rung
 *	/run/zrn-led/user/notify	the shell: 1 while a notification is pending
 *	/run/zrn-led/user/settings	Settings: the config has changed
 *
 * The state is in those files rather than in this program, so it survives a
 * restart here, and a writer never has to wait for or even know of this
 * program: a charger plugged in while it is down is seen when it is back. A
 * claim missing says nothing; an empty one is being written, and its close
 * brings another look.
 *
 * It sits in poll() with no timeout and wakes only when a claim changes, and it
 * holds no wakeup source: the freezer stops it like anything else. Once written,
 * the light needs no one: the chip blinks and breathes by itself, asleep or not.
 *
 * SIGHUP: read everything again and write the light even if it looks the same.
 * udev sends it when the LED appears, since the driver may load after this.
 * Stopping this leaves the light as it is, so that a restart does not flash it.
 *
 * --state DIR, --sys DIR and --config FILE are for tests against a fake sysfs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include "led-policy.h"
#include "../ztos-user.h"

static const char *state_dir = LED_STATE_DIR, *sys_dir = LED_SYS_DIR;
static char user_dir[PATH_MAX - 64], config_file[PATH_MAX];

static struct led_config config;
static struct led_claims claims;
static struct led_output shown;
static bool shown_valid;

/* A claim's words, or false if it is missing or being written. Never follows a
 * link or waits on a pipe: the session's directory is not ours alone. */
static bool read_small(const char *path, char *out, size_t size, bool *missing) {
	out[0] = 0;
	*missing = false;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		*missing = errno == ENOENT;
		return false;
	}
	struct stat st;
	ssize_t n = -1;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, out, size - 1);
	close(fd);
	if (n <= 0) return false;
	out[n] = 0;
	return true;
}

static void claims_read(void) {
	char path[PATH_MAX], text[32];
	bool missing;
	snprintf(path, sizeof(path), "%s/%s", state_dir, LED_CLAIM_CHARGE);
	if (read_small(path, text, sizeof(text), &missing)) claims.charge = led_charge_parse(text);
	else if (missing) claims.charge = LED_CHARGE_NONE;
	snprintf(path, sizeof(path), "%s/%s", state_dir, LED_CLAIM_BOOST);
	if (read_small(path, text, sizeof(text), &missing)) claims.boost = led_flag_parse(text);
	else if (missing) claims.boost = false;
	snprintf(path, sizeof(path), "%s/%s", user_dir, LED_CLAIM_NOTIFY);
	if (read_small(path, text, sizeof(text), &missing)) claims.notify = led_flag_parse(text);
	else if (missing) claims.notify = false;
}

/* The same file as Settings and the shell; this reads only its own keys. */
static void config_read(void) {
	led_config_defaults(&config);
	int fd = open(config_file, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;
	struct stat st;
	FILE *f = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? fdopen(fd, "r") : NULL;
	if (!f) {
		close(fd);
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		char *key = line, *value = eq + 1, *end = eq;
		*eq = 0;
		while (*key == ' ' || *key == '\t') key++;
		while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
		while (*value == ' ' || *value == '\t') value++;
		value[strcspn(value, " \t\r\n")] = 0;
		led_config_line(&config, key, value);
	}
	fclose(f);
}

static bool put_attr(int channel, const char *attr, const char *value) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/%s", sys_dir, LED_CHANNELS[channel], attr);
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0) return false;
	bool ok = write(fd, value, strlen(value)) >= 0;
	close(fd);
	return ok;
}

/* Only when it differs, unless told to: each write is a trip over I2C, and
 * writing the same blink again would restart it. A light that was not there
 * is written again on any later look, and at udev's SIGHUP. */
static void show(bool force) {
	enum led_source who = led_winner(&config, &claims);
	struct led_output want = led_output_of(&config, who);
	if (!force && shown_valid && led_output_same(&want, &shown)) return;
	struct led_write writes[LED_MAX_WRITES];
	int n = led_plan(&want, writes);
	bool ok = true;
	for (int i = 0; i < n; i++) {
		if (!put_attr(writes[i].channel, writes[i].attr, writes[i].value) && writes[i].required) ok = false;
	}
	shown = want;
	shown_valid = ok;
	fprintf(stderr, "zrn-ledd: %s: %d %d %d%s\n", who < LED_NOBODY ? LED_LOOKS[who].title : "off",
		want.rgb[0], want.rgb[1], want.rgb[2], ok ? "" : " (no light to write yet)");
}

/* Normally made at boot by tmpfiles (zrn-led.conf); for a run without that, here. */
static void dirs_make(void) {
	mkdir(state_dir, 0755);
	if (mkdir(user_dir, 0775) == 0) {
		struct group *g = getgrnam("video");
		if (g && chown(user_dir, 0, g->gr_gid) != 0) {}
		chmod(user_dir, 0775);
	}
}

/* From the inotify buffer: which of the files we read did this name? */
static void note_events(const char *buf, ssize_t len, int user_wd, bool *look, bool *settings) {
	for (const char *p = buf; p < buf + len;) {
		const struct inotify_event *e = (const void *)p;
		p += sizeof(*e) + e->len;
		if (e->mask & IN_Q_OVERFLOW) {
			*look = *settings = true;
			continue;
		}
		if (!e->len) continue;
		if (e->wd == user_wd) {
			if (!strcmp(e->name, LED_POKE_SETTINGS)) *settings = true;
			else if (!strcmp(e->name, LED_CLAIM_NOTIFY)) *look = true;
		} else if (!strcmp(e->name, LED_CLAIM_CHARGE) || !strcmp(e->name, LED_CLAIM_BOOST)) {
			*look = true;
		}
	}
}

int main(int argc, char **argv) {
	config_file[0] = 0;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--state")) state_dir = argv[i + 1];
		else if (!strcmp(argv[i], "--sys")) sys_dir = argv[i + 1];
		else if (!strcmp(argv[i], "--config")) snprintf(config_file, sizeof(config_file), "%s", argv[i + 1]);
	}
	if (!config_file[0]) snprintf(config_file, sizeof(config_file), "%s/.config/ztmetro/config", ztos_user_home());
	snprintf(user_dir, sizeof(user_dir), "%s/user", state_dir);
	dirs_make();

	int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	const unsigned mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE;
	int user_wd = in < 0 ? -1 : inotify_add_watch(in, user_dir, mask);
	if (in < 0 || user_wd < 0 || inotify_add_watch(in, state_dir, mask) < 0) {
		fprintf(stderr, "zrn-ledd: cannot watch %s: %s\n", state_dir, strerror(errno));
		return 1;
	}
	sigset_t hup;
	sigemptyset(&hup);
	sigaddset(&hup, SIGHUP);
	sigprocmask(SIG_BLOCK, &hup, NULL);
	int sig = signalfd(-1, &hup, SFD_NONBLOCK | SFD_CLOEXEC);

	config_read();
	claims_read();
	show(true);		/* the light may hold anything: a last run, or nothing */

	struct pollfd fds[2] = { { .fd = in, .events = POLLIN }, { .fd = sig, .events = POLLIN } };
	for (;;) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) continue;
			return 1;
		}
		bool look = false, settings = false, force = false;
		if (fds[0].revents) {
			char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			ssize_t len;
			while ((len = read(in, buf, sizeof(buf))) > 0) note_events(buf, len, user_wd, &look, &settings);
		}
		if (fds[1].revents) {
			struct signalfd_siginfo si;
			while (read(sig, &si, sizeof(si)) == (ssize_t)sizeof(si)) force = look = settings = true;
		}
		if (settings) config_read();
		if (look) claims_read();
		if (look || settings) show(force);
	}
}
