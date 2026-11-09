// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztNTFY: which ntfy topics this phone listens to, in the Windows Phone 8.1 look.
 *
 * No network in here. ztos-ntfyd has it all: this writes the subscriptions file
 * (it notices by itself), reads how each topic is doing from
 * $XDG_RUNTIME_DIR/ztos-ntfyd.status, and has it send a test message. A sign-in is
 * typed here, kept in the 0600 file, and never shown again.
 *
 *	ztntfy --render list|add|topic FILE	draws a page, and touches nothing
 *	ztntfy --add SERVER TOPIC		adds a topic with no sign-in, as the page does
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ntfy-model.h"
#include "zrnmetrocore.h"

#define MARGIN ZMC_MARGIN

enum page { P_LIST, P_ADD, P_TOPIC };

enum {
	A_OPEN = 1, A_NEW, A_FIELD, A_AUTH, A_RANDOM, A_SAVE, A_TEST, A_REMOVE, A_REMOVE_YES,
};

enum field { F_SERVER, F_TOPIC, F_TOKEN, F_USER, F_PASS, F_COUNT };
enum auth { AUTH_NONE, AUTH_TOKEN, AUTH_USER };

static const char *const AUTH_NAMES[] = { "None", "Access token", "Username and password" };

static struct ntfy_sub_conf confs[NTFY_SUBS_MAX];
static int n_confs;
static struct { char id[NTFY_SUB_ID_LEN + 1], state[16], why[100]; long long shown; } stats[NTFY_SUBS_MAX];
static int n_stats;
static bool have_status;
static time_t status_mtime;
static long status_nsec;

static char edit[F_COUNT][NTFY_CRED_MAX + 1];
static int focus = -1;
static int auth_kind;
static bool auth_open, remove_asked;
static char add_error[120];

static char current[NTFY_SUB_ID_LEN + 1];	/* the topic on show */
static pid_t test_pid;
static char test_for[NTFY_SUB_ID_LEN + 1];
static const char *test_result;
static long fixture_now;

static long now_s(void) { return fixture_now ? fixture_now : (long)time(NULL); }

/* ---- the files --------------------------------------------------------------- */

static void conf_paths(char *dir, size_t dsize, char *path, size_t psize) {
	const char *home = getenv("HOME");
	snprintf(dir, dsize, "%s/.config/ztmetro/ntfy", home ? home : "");
	snprintf(path, psize, "%s/subscriptions", dir);
}

static void confs_load(void) {
	char dir[400], path[440];
	static char line[NTFY_CONF_LINE_MAX];
	conf_paths(dir, sizeof(dir), path, sizeof(path));
	n_confs = 0;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	FILE *f = fd >= 0 ? fdopen(fd, "re") : NULL;
	while (f && n_confs < NTFY_SUBS_MAX && fgets(line, sizeof(line), f)) {
		if (ntfy_conf_parse(line, &confs[n_confs])) {
			n_confs++;
		}
	}
	explicit_bzero(line, sizeof(line));
	if (f) {
		fclose(f);
	} else if (fd >= 0) {
		close(fd);
	}
}

/* In one piece, readable by this user alone, or not at all. */
static bool confs_save(void) {
	char dir[400], path[440], tmp[460];
	static char line[NTFY_CONF_LINE_MAX];
	conf_paths(dir, sizeof(dir), path, sizeof(path));
	char up[400];
	snprintf(up, sizeof(up), "%s", dir);
	for (char *p = up + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(up, 0700);
			*p = '/';
		}
	}
	mkdir(dir, 0700);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	/* (a file that's been left there may have been made with other rights) */
	bool ok = fchmod(fd, 0600) == 0;
	static const char HEAD[] = "# ztNTFY's subscriptions: id, added, server, topic, user, password, token. Keep this private.\n";
	ok = ok && write(fd, HEAD, sizeof(HEAD) - 1) == (ssize_t)sizeof(HEAD) - 1;
	for (int i = 0; ok && i < n_confs; i++) {
		ok = ntfy_conf_format(line, sizeof(line), &confs[i]) && write(fd, line, strlen(line)) == (ssize_t)strlen(line);
	}
	explicit_bzero(line, sizeof(line));
	ok = ok && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static bool status_load(bool force) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[320], line[200];
	struct stat st;
	snprintf(path, sizeof(path), "%s/ztos-ntfyd.status", rt ? rt : "/tmp");
	if (stat(path, &st) != 0) {
		bool was = have_status;
		have_status = false;
		n_stats = 0;
		return was || force;
	}
	if (!force && have_status && st.st_mtim.tv_sec == status_mtime && st.st_mtim.tv_nsec == status_nsec) {
		return false;
	}
	status_mtime = st.st_mtim.tv_sec;
	status_nsec = st.st_mtim.tv_nsec;
	have_status = true;
	n_stats = 0;
	FILE *f = fopen(path, "re");
	while (f && n_stats < NTFY_SUBS_MAX && fgets(line, sizeof(line), f)) {
		char *fields[4] = { line, NULL, NULL, NULL };
		line[strcspn(line, "\n")] = 0;
		for (int i = 1; i < 4 && fields[i - 1]; i++) {
			fields[i] = strchr(fields[i - 1], '\t');
			if (fields[i]) {
				*fields[i]++ = 0;
			}
		}
		if (!fields[3]) {
			continue;
		}
		snprintf(stats[n_stats].id, sizeof(stats[0].id), "%.16s", fields[0]);
		snprintf(stats[n_stats].state, sizeof(stats[0].state), "%.15s", fields[1]);
		stats[n_stats].shown = atoll(fields[2]);
		snprintf(stats[n_stats].why, sizeof(stats[0].why), "%.99s", fields[3]);
		n_stats++;
	}
	if (f) {
		fclose(f);
	}
	return true;
}

/* ---- words -------------------------------------------------------------------- */

static int status_of(const char *id) {
	for (int i = 0; i < n_stats; i++) {
		if (!strcmp(stats[i].id, id)) {
			return i;
		}
	}
	return -1;
}

static const struct ntfy_sub_conf *conf_current(void) {
	for (int i = 0; i < n_confs; i++) {
		if (!strcmp(confs[i].id, current)) {
			return &confs[i];
		}
	}
	return NULL;
}

static void age_text(char *out, size_t size, long long seen) {
	long long s = now_s() - seen;
	if (s < 0) s = 0;
	if (s < 90) snprintf(out, size, "just now");
	else if (s < 5400) snprintf(out, size, "%lld minutes ago", (s + 30) / 60);
	else if (s < 129600) snprintf(out, size, "%lld hours ago", (s + 1800) / 3600);
	else snprintf(out, size, "%lld days ago", (s + 43200) / 86400);
}

static const char *state_text(const struct ntfy_sub_conf *c) {
	int k = status_of(c->id);
	if (!have_status || k < 0) return "not connected";
	if (!strcmp(stats[k].state, "connected")) return "connected";
	if (!strcmp(stats[k].state, "connecting")) return "connecting";
	return "waiting to try again";
}

/* ---- drawing -------------------------------------------------------------------- */

static double note(cairo_t *cr, double y, const char *s) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	return y + zmc_text(cr, s, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 14;
}

static double warning(cairo_t *cr, double y, const char *s) {
	cairo_set_source_rgb(cr, 0.95, 0.6, 0.2);
	return y + zmc_text(cr, s, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 14;
}

static double draw_list(cairo_t *cr) {
	double y = zmc_header(cr, "topics");
	if (!have_status) {
		y = warning(cr, y, "The push service isn't running, so nothing is arriving just now.");
	}
	if (!n_confs) {
		y = note(cr, y, "Anything that can make a web request can send a notification to this phone through "
			"an ntfy server. Add a topic to start listening; until then, this phone doesn't connect to anything.");
	}
	for (int i = 0; i < n_confs; i++) {
		const struct ntfy_sub_conf *c = &confs[i];
		char host[80], line[180], age[32];
		int k = status_of(c->id);
		ntfy_server_host(c->server, host, sizeof(host));
		if (k >= 0 && stats[k].shown > 0) {
			age_text(age, sizeof(age), stats[k].shown);
			snprintf(line, sizeof(line), "%s, %s, last message %s", host, state_text(c), age);
		} else {
			snprintf(line, sizeof(line), "%s, %s", host, state_text(c));
		}
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, c->topic, FONT_SEMILIGHT, 25.5, MARGIN, y, zmc.width - 2 * MARGIN);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text_line(cr, line, FONT_REGULAR, 12, MARGIN, y + 31, zmc.width - 2 * MARGIN);
		zmc_add_hit(0, y - 2, zmc.width, 57, A_OPEN, i);
		y += 59;
	}
	if (n_confs < NTFY_SUBS_MAX) {
		y = zmc_button(cr, MARGIN, y + 8, 150, "Add topic", A_NEW, 0) + 8;
	}
	return y;
}

/* What's in a box: the end of it when it's long, and a password as dots. */
static void box_text(char *out, size_t size, const char *value, bool secret) {
	size_t n = strlen(value);
	if (secret) {
		size_t o = 0, shown = n > 32 ? 32 : n;
		for (size_t i = 0; i < shown && o + 4 < size; i++) {
			o += ntfy_utf8_put(out + o, 0x2022);
		}
		out[o] = 0;
		return;
	}
	if (n <= 30) {
		snprintf(out, size, "%s", value);
		return;
	}
	const char *tail = value + n - 28;
	while (((unsigned char)*tail & 0xc0) == 0x80) tail++;
	snprintf(out, size, "\xe2\x80\xa6%s", tail);
}

static double field_box(cairo_t *cr, double y, const char *caption, int field, bool secret) {
	char shown[160];
	box_text(shown, sizeof(shown), edit[field], secret);
	return zmc_textbox(cr, y, caption, shown, focus == field, A_FIELD, field) + 6;
}

static double draw_add(cairo_t *cr) {
	double y = zmc_header(cr, "add topic");
	y = field_box(cr, y, "Server", F_SERVER, false);
	char server[NTFY_SERVER_MAX + 1] = "";
	bool known = ntfy_server_normalise(edit[F_SERVER], server, sizeof(server));
	if (known && !ntfy_server_is_https(server)) {
		y = warning(cr, y, "Plain http: anyone on the way can read these messages, and any sign-in below.");
	}
	y = field_box(cr, y, "Topic", F_TOPIC, false);
	y = zmc_button(cr, MARGIN, y + 2, 170, "Random name", A_RANDOM, 0) + 8;
	char host[80] = "";
	if (known) {
		ntfy_server_host(server, host, sizeof(host));
	}
	if (!strcmp(host, "ntfy.sh") && auth_kind == AUTH_NONE) {
		y = note(cr, y, "On a public server a topic's name is its password: anyone who knows it can read "
			"its messages and send to it. Use a name nobody will guess.");
	}
	y = zmc_picker(cr, y, "Sign-in", AUTH_NAMES, 3, auth_kind, auth_open, A_AUTH);
	if (auth_kind == AUTH_TOKEN) {
		y = field_box(cr, y, "Access token", F_TOKEN, true);
	} else if (auth_kind == AUTH_USER) {
		y = field_box(cr, y, "Username", F_USER, false);
		y = field_box(cr, y, "Password", F_PASS, true);
	}
	if (add_error[0]) {
		y = warning(cr, y + 4, add_error);
	}
	y = zmc_button(cr, MARGIN, y + 4, 120, "Add", A_SAVE, 0) + 8;
	return y;
}

static double draw_topic(cairo_t *cr) {
	const struct ntfy_sub_conf *c = conf_current();
	double y = zmc_header(cr, "topic");
	if (!c) {
		return note(cr, y, "This topic has been removed.");
	}
	char host[NTFY_SERVER_MAX + 1], line[NTFY_SERVER_MAX + NTFY_TOPIC_MAX + 64], age[32];
	ntfy_server_host(c->server, host, sizeof(host));
	y = zmc_field(cr, y, "Name", c->topic);
	y = zmc_field(cr, y, "Server", c->server);
	y = zmc_field(cr, y, "Sign-in", c->token[0] ? "Access token" : c->user[0] ? c->user : "None");
	int k = status_of(c->id);
	if (k >= 0 && stats[k].why[0]) {
		snprintf(line, sizeof(line), "%s: %s", state_text(c), stats[k].why);
	} else {
		snprintf(line, sizeof(line), "%s", have_status ? state_text(c) : "the push service isn't running");
	}
	y = zmc_field(cr, y, "State", line);
	if (k >= 0 && stats[k].shown > 0) {
		age_text(age, sizeof(age), stats[k].shown);
		y = zmc_field(cr, y, "Last message", age);
	} else {
		y = zmc_field(cr, y, "Last message", "none yet");
	}
	y = zmc_button(cr, MARGIN, y + 4, 150, test_pid > 0 ? "Sending..." : "Send a test", A_TEST, 0) + 8;
	if (test_result) {
		y = note(cr, y, test_result);
	}
	snprintf(line, sizeof(line), "From anywhere:\ncurl -d \"hello\" %s/%s", ntfy_server_is_https(c->server) ? host : c->server, c->topic);
	y = note(cr, y + 6, line);
	y = zmc_button(cr, MARGIN, y + 6, remove_asked ? 180 : 120, remove_asked ? "Really remove" : "Remove",
		remove_asked ? A_REMOVE_YES : A_REMOVE, 0) + 8;
	return y;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_LIST: return draw_list(cr);
	case P_ADD: return draw_add(cr);
	case P_TOPIC: return draw_topic(cr);
	}
	return 0;
}

/* ---- touch -------------------------------------------------------------------- */

static void focus_field(int field) {
	focus = field;
	zmc_keyboard(field >= 0);
}

static void edit_clear(void) {
	for (int i = 0; i < F_COUNT; i++) {
		explicit_bzero(edit[i], sizeof(edit[i]));
	}
}

static void random_hex(char *out, size_t bytes) {
	unsigned char raw[16];
	if (bytes > sizeof(raw) || getrandom(raw, bytes, 0) != (ssize_t)bytes) {
		/* (no randomness to be had this early in a boot is not expected) */
		for (size_t i = 0; i < bytes && i < sizeof(raw); i++) raw[i] = (unsigned char)(rand() ^ time(NULL));
	}
	for (size_t i = 0; i < bytes; i++) {
		sprintf(out + 2 * i, "%02x", raw[i]);
	}
}

/* Long enough that nobody guesses it, and short enough to read out. */
static void random_topic(void) {
	char hex[33];
	random_hex(hex, 12);
	snprintf(edit[F_TOPIC], sizeof(edit[0]), "zt-%s", hex);
}

/* From what is in the boxes. False: add_error says why. */
static bool add_from_boxes(void) {
	struct ntfy_sub_conf c;
	memset(&c, 0, sizeof(c));
	add_error[0] = 0;
	if (!ntfy_server_normalise(edit[F_SERVER], c.server, sizeof(c.server))) {
		snprintf(add_error, sizeof(add_error), "That isn't a server address. For example: https://ntfy.sh");
		return false;
	}
	if (!ntfy_topic_valid(edit[F_TOPIC])) {
		snprintf(add_error, sizeof(add_error), "A topic's name is 1 to 64 letters, digits, dashes and underscores.");
		return false;
	}
	snprintf(c.topic, sizeof(c.topic), "%s", edit[F_TOPIC]);
	if (auth_kind == AUTH_TOKEN) {
		if (!edit[F_TOKEN][0] || !ntfy_token_valid(edit[F_TOKEN])) {
			snprintf(add_error, sizeof(add_error), "That isn't an access token: no spaces, and not empty.");
			return false;
		}
		snprintf(c.token, sizeof(c.token), "%s", edit[F_TOKEN]);
	} else if (auth_kind == AUTH_USER) {
		if (!edit[F_USER][0] || !ntfy_user_valid(edit[F_USER]) || !edit[F_PASS][0]) {
			snprintf(add_error, sizeof(add_error), "A username, with no colon in it, and a password.");
			return false;
		}
		snprintf(c.user, sizeof(c.user), "%s", edit[F_USER]);
		snprintf(c.pass, sizeof(c.pass), "%s", edit[F_PASS]);
	}
	confs_load();
	for (int i = 0; i < n_confs; i++) {
		if (!strcmp(confs[i].server, c.server) && !strcmp(confs[i].topic, c.topic)) {
			snprintf(add_error, sizeof(add_error), "This phone already listens to that topic.");
			return false;
		}
	}
	if (n_confs >= NTFY_SUBS_MAX) {
		snprintf(add_error, sizeof(add_error), "That's as many topics as this can follow.");
		return false;
	}
	random_hex(c.id, NTFY_SUB_ID_LEN / 2);
	c.added = (long long)time(NULL);
	confs[n_confs++] = c;
	if (!confs_save()) {
		n_confs--;
		snprintf(add_error, sizeof(add_error), "It couldn't be saved: %s", strerror(errno));
		return false;
	}
	explicit_bzero(&c, sizeof(c));
	return true;
}

static void add_topic(void) {
	if (add_from_boxes()) {
		edit_clear();
		focus_field(-1);
		zmc_go(P_LIST, -1);
	}
}

/* Done by the daemon's program, which keeps the secrets off every command line. */
static void test_send(const struct ntfy_sub_conf *c) {
	char path[400];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
	if (test_pid > 0) {
		return;
	}
	if (n > 0) {
		path[n] = 0;
		char *slash = strrchr(path, '/');
		strcpy(slash ? slash + 1 : path, "ztos-ntfyd");
	} else {
		path[0] = 0;
	}
	snprintf(test_for, sizeof(test_for), "%s", c->id);
	test_result = NULL;
	pid_t pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		if (path[0]) {
			execl(path, "ztos-ntfyd", "--publish", test_for, (char *)NULL);
		}
		execlp("ztos-ntfyd", "ztos-ntfyd", "--publish", test_for, (char *)NULL);
		_exit(127);
	}
	test_pid = pid > 0 ? pid : 0;
	if (pid < 0) {
		test_result = "It couldn't be sent.";
	}
}

static void test_check(void) {
	int status;
	if (test_pid <= 0 || waitpid(test_pid, &status, WNOHANG) != test_pid) {
		return;
	}
	test_pid = 0;
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
	test_result = code == 0 ? "Sent. It should arrive in a moment." :
		code == 3 ? "Not sent: a secure connection couldn't be made." :
		code == 4 ? "Not sent: this sign-in isn't allowed to publish here." :
		"Not sent: the server couldn't be reached.";
	zmc_redraw();
}

static void remove_current(void) {
	confs_load();
	for (int i = 0; i < n_confs; i++) {
		if (!strcmp(confs[i].id, current)) {
			explicit_bzero(&confs[i], sizeof(confs[i]));
			memmove(&confs[i], &confs[i + 1], sizeof(confs[0]) * (size_t)(n_confs - i - 1));
			n_confs--;
			break;
		}
	}
	if (!confs_save()) {
		confs_load();
		return;
	}
	remove_asked = false;
	zmc_go(P_LIST, -1);
}

static void activate(int action, int arg) {
	const struct ntfy_sub_conf *c;
	switch (action) {
	case A_OPEN:
		if (arg >= 0 && arg < n_confs) {
			snprintf(current, sizeof(current), "%s", confs[arg].id);
			remove_asked = false;
			test_result = NULL;
			zmc_go(P_TOPIC, 1);
		}
		break;
	case A_NEW:
		edit_clear();
		snprintf(edit[F_SERVER], sizeof(edit[0]), "https://ntfy.sh");
		auth_kind = AUTH_NONE;
		auth_open = false;
		add_error[0] = 0;
		focus = -1;
		zmc_go(P_ADD, 1);
		break;
	case A_FIELD:
		focus_field(arg);
		break;
	case A_AUTH:
		if (arg < 0) {
			auth_open = !auth_open;
		} else {
			auth_kind = arg;
			auth_open = false;
		}
		break;
	case A_RANDOM:
		random_topic();
		break;
	case A_SAVE:
		add_topic();
		break;
	case A_TEST:
		c = conf_current();
		if (c) {
			test_send(c);
		}
		break;
	case A_REMOVE:
		remove_asked = true;
		break;
	case A_REMOVE_YES:
		remove_current();
		break;
	}
	zmc_redraw();
}

static void typed(const char *t) {
	if (focus < 0) {
		return;
	}
	char *text = edit[focus];
	size_t n = strlen(text);
	if (!strcmp(t, "\b")) {
		while (n > 0 && ((unsigned char)text[n - 1] & 0xc0) == 0x80) n--;
		if (n > 0) n--;
		text[n] = 0;
	} else if (!strcmp(t, "\n")) {
		focus_field(-1);
	} else if (n + strlen(t) < sizeof(edit[0])) {
		strcat(text, t);
	}
	add_error[0] = 0;
	zmc_redraw();
}

static bool back(void) {
	if (auth_open) {
		auth_open = false;
		zmc_redraw();
		return true;
	}
	if (focus >= 0) {
		focus_field(-1);
		zmc_redraw();
		return true;
	}
	if (zmc.page == P_LIST) {
		return false;
	}
	edit_clear();
	zmc_go(P_LIST, -1);
	return true;
}

static void tick(void) {
	static uint32_t redrawn;
	uint32_t now = zmc_now_ms();
	test_check();
	if (status_load(false) || now - redrawn > 30000) {	/* the ages move on by themselves */
		redrawn = now;
		zmc_redraw();
	}
}

static const struct zmc_app app = {
	.app_id = "ztntfy", .title = "NTFY", .caps = "NTFY",
	.render = render, .activate = activate, .back = back, .text = typed,
};

int main(int argc, char **argv) {
	confs_load();
	status_load(true);
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		const char *when = getenv("ZTNTFY_RENDER_NOW");
		fixture_now = when ? atol(when) : 0;
		if (n_confs) {
			snprintf(current, sizeof(current), "%s", confs[0].id);
		}
		if (!strcmp(argv[2], "list")) {
			return zmc_render_png(&app, P_LIST, 1200, argv[3]);
		}
		if (!strcmp(argv[2], "add")) {
			snprintf(edit[F_SERVER], sizeof(edit[0]), "https://ntfy.sh");
			snprintf(edit[F_TOPIC], sizeof(edit[0]), "zt-5c1e07a29db64f83a1e0d7c2");
			return zmc_render_png(&app, P_ADD, 1200, argv[3]);
		}
		if (!strcmp(argv[2], "topic")) {
			return zmc_render_png(&app, P_TOPIC, 1200, argv[3]);
		}
		return 2;
	}
	if (argc == 4 && !strcmp(argv[1], "--add")) {
		snprintf(edit[F_SERVER], sizeof(edit[0]), "%s", argv[2]);
		snprintf(edit[F_TOPIC], sizeof(edit[0]), "%s", argv[3]);
		if (!add_from_boxes()) {
			fprintf(stderr, "ztntfy: %s\n", add_error);
			return 1;
		}
		return 0;
	}
	zmc_every(1000, tick);
	return zmc_run(&app, P_LIST);
}
