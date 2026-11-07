// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztos-ntfyd: messages pushed from outside, from ntfy servers, as the phone's
 * ordinary notifications. A user service; ztntfy is how it's set up (README.md).
 *
 * One streaming GET per topic (<server>/<topic>/json?since=<last id>), each with
 * its own saved place, its own sign-in and its own backoff. Each message goes to
 * org.freedesktop.Notifications, and the saved id moves on only when that has
 * taken it: a message that couldn't be shown is asked for again.
 *
 * With no topics it opens no socket at all, not even to the bus, and sleeps on
 * the subscriptions file. Otherwise it wakes for data, for the file, for a new
 * network address, and for its one timer. That is a CLOCK_BOOTTIME timerfd and
 * not an alarm: it never wakes a sleeping phone, and all that has fallen due is
 * done at the next wake. libcurl's speed limits aren't used (one was measured
 * waking this every second, on a silent line): the silence of a dead connection
 * is timed here, against ntfy's keepalive lines.
 *
 * Nothing from a server is believed (ntfy-model.h): no address it mentions is
 * ever fetched, and nothing it sends is run. Topic names are as good as passwords
 * on a public server, so the log and the status file name a subscription by its
 * id instead, and neither has ever seen a token.
 *
 *	~/.config/ztmetro/ntfy/subscriptions	0600, written by ztntfy
 *	~/.local/state/ztmetro/ntfy-state	where each topic has got to
 *	$XDG_RUNTIME_DIR/ztos-ntfyd.status	how each one is doing, for ztntfy
 *
 * ztos-ntfyd --publish SUBSCRIPTION-ID	sends a test message and exits (the secrets
 * come from the file, so that they aren't on a command line).
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "ntfy-model.h"

/* How long things take. ZTOS_NTFYD_TEST_FAST (tests/ntfyd-e2e.py) shortens these
 * and nothing else: it changes no check. */
static long long T_BACKOFF_BASE = 5000, T_BACKOFF_CAP = 300000;
static long long T_DEAD = 150000;		/* ntfy's keepalive is 45 s: three missed and a bit */
static long long T_CONNECT_DEAD = 45000;	/* asked, and no answer of any kind */
static long long T_STABLE = 60000;		/* a stream this old was a success: backoff starts again */
static long long T_CATCHUP_QUIET = 1500;	/* a backlog has ended when the line goes quiet */
static long long T_CATCHUP_MAX = 5000;
static long long T_TOKEN = 15000;		/* one more alert per this long, past the burst */
static long long T_BUS_MAX = 30000;		/* the longest between tries at a missing ztnotifyd */
static long long T_NET_SETTLE = 1500;		/* an address has come: a moment for the routes */
static long long T_NET_JITTER = 2000;
static long long T_NET_FRESH = 5000;		/* a stream that spoke this recently isn't doubted */

/* A server that won't stop talking is hung up on. (Not libcurl's speed limit:
 * with that it wakes every second, on a silent line too.) ntfy replays at most
 * about 10 MB of a topic's backlog, so this leaves room for one. */
#define FLOOD_BYTES (32 * 1024 * 1024)
#define FLOOD_WINDOW_MS 10000
#define ATTEMPT_LONGEST 16			/* past any cap */

enum sub_state { S_WAITING, S_CONNECTING, S_CONNECTED };

struct sub {
	struct ntfy_sub_conf cfg;
	enum sub_state state;
	CURL *easy;
	struct curl_slist *headers;
	struct ntfy_lines lines;
	char host[NTFY_SERVER_MAX + 1];
	char url[512];
	char since[NTFY_ID_MAX + 1];

	int attempt;
	long long retry_at, started_at, first_byte_at, last_data;
	long long flood_at;
	size_t flood_bytes;
	bool flooded;
	bool checked_status;
	const char *why;	/* our words for why it is waiting; never the server's */

	bool catchup;		/* what's coming may be a backlog: hold it a moment */
	long long catchup_end;

	char last_id[NTFY_ID_MAX + 1];
	long long last_time, last_shown;
	struct ntfy_seen seen;	/* come in, whether or not shown yet */
	struct ntfy_seen shown;	/* shown: all that's saved, so a restart loses nothing */
	struct ntfy_held held;
	struct ntfy_bucket bucket;
};

static struct sub *subs[NTFY_SUBS_MAX];
static int n_subs;
static CURLM *multi;
static bool curl_ready;
static sd_bus *bus;
static long long bus_retry_at, bus_wait;
static int inotify_fd = -1, signal_fd = -1, timer_fd = -1, net_fd = -1;
static long long net_kick_at;
static bool status_dirty = true, have_emoji;
static char conf_dir[400], conf_path[440], state_path[440], status_path[440];
static const char *const CONF_NAME = "subscriptions";

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	va_list ap;
	fprintf(stderr, "ztos-ntfyd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Time that goes on while the phone sleeps: what fell due in a sleep is due. */
static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static uint32_t random32(void) {
	uint32_t v = 0;
	if (getrandom(&v, sizeof(v), GRND_NONBLOCK) != sizeof(v)) {
		v = (uint32_t)now_ms() * 2654435761u;
	}
	return v;
}

/* ---- files ------------------------------------------------------------------ */

static void mkdir_p(const char *path, mode_t mode) {
	char tmp[440];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, mode);
			*p = '/';
		}
	}
	mkdir(tmp, mode);
}

/* In one piece, or not at all. `sync`: it should still be there after a power cut. */
static bool write_file(const char *path, const char *data, size_t len, bool sync) {
	char tmp[460];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	bool ok = write(fd, data, len) == (ssize_t)len;
	if (ok && sync) {
		ok = fdatasync(fd) == 0;
	}
	close(fd);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

#define STATE_LINE_MAX ((NTFY_SEEN_SAVED + 2) * (NTFY_ID_MAX + 2) + 64)

static void state_save(void) {
	char *all = malloc((size_t)(n_subs + 1) * STATE_LINE_MAX);
	size_t o = 0;
	if (!all) {
		return;
	}
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		char line[STATE_LINE_MAX];
		if (ntfy_state_format(line, sizeof(line), s->cfg.id, s->last_id, s->last_time, s->last_shown, &s->shown)) {
			size_t n = strlen(line);
			memcpy(all + o, line, n);
			o += n;
		}
	}
	if (!write_file(state_path, all, o, true)) {
		say("can't save where the topics have got to: %s", strerror(errno));
	}
	free(all);
}

static void state_load(struct sub *s) {
	char line[STATE_LINE_MAX];
	FILE *f = fopen(state_path, "re");
	while (f && fgets(line, sizeof(line), f)) {
		char id[NTFY_SUB_ID_LEN + 1], last[NTFY_ID_MAX + 1];
		long long t, shown;
		struct ntfy_seen seen;
		if (ntfy_state_parse(line, id, sizeof(id), last, sizeof(last), &t, &shown, &seen) && !strcmp(id, s->cfg.id)) {
			snprintf(s->last_id, sizeof(s->last_id), "%s", last);
			s->last_time = t;
			s->last_shown = shown;
			s->seen = seen;
			s->shown = seen;
		}
	}
	if (f) {
		fclose(f);
	}
}

static const char *state_word(enum sub_state st) {
	return st == S_CONNECTED ? "connected" : st == S_CONNECTING ? "connecting" : "waiting";
}

/* For ztntfy: only when something has changed. No topic, no address, no secret. */
static void status_write(void) {
	char text[NTFY_SUBS_MAX * 160] = "";
	size_t o = 0;
	status_dirty = false;
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		o += (size_t)snprintf(text + o, sizeof(text) - o, "%s\t%s\t%lld\t%s\n", s->cfg.id, state_word(s->state),
			s->last_shown, s->state == S_CONNECTED ? "" : s->why ? s->why : "");
	}
	write_file(status_path, text, o, false);
}

/* ---- connections ------------------------------------------------------------ */

/* What both a stream and a test message are held to. */
static void easy_common(CURL *e, const struct ntfy_sub_conf *c, struct curl_slist **headers, const char *url) {
	curl_easy_setopt(e, CURLOPT_URL, url);
	/* what the address says and nothing else; a redirect is not followed, so it
	 * can't take a sign-in to another place, or https down to http */
	curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, ntfy_server_is_https(c->server) ? "https" : "http");
	curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(e, CURLOPT_PROXY, "");
	curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(e, CURLOPT_USERAGENT, "ztos-ntfyd");
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(e, CURLOPT_FAILONERROR, 1L);
	if (c->token[0]) {
		char auth[NTFY_CRED_MAX + 32];
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->token);
		*headers = curl_slist_append(*headers, auth);
		explicit_bzero(auth, sizeof(auth));
	} else if (c->user[0]) {
		curl_easy_setopt(e, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		curl_easy_setopt(e, CURLOPT_USERNAME, c->user);
		curl_easy_setopt(e, CURLOPT_PASSWORD, c->pass);
	}
	curl_easy_setopt(e, CURLOPT_HTTPHEADER, *headers);
}

static void sub_line(const char *line, size_t n, void *ctx);

static size_t sub_write(char *data, size_t size, size_t count, void *ctx) {
	struct sub *s = ctx;
	size_t n = size * count;
	long long now = now_ms();
	if (!s->checked_status) {
		long code = 0;
		s->checked_status = true;
		curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &code);
		/* (300s among them: not followed) */
		if (code != 200) {
			s->why = "the server gave an unexpected answer";
			return CURL_WRITEFUNC_ERROR;
		}
		s->first_byte_at = now;
		s->state = S_CONNECTED;
		s->why = NULL;
		status_dirty = true;
		/* the backlog, if any, comes first */
		s->catchup = true;
		say("%s: connected", s->cfg.id);
	}
	s->last_data = now;
	if (now - s->flood_at >= FLOOD_WINDOW_MS) {
		s->flood_at = now;
		s->flood_bytes = 0;
	}
	s->flood_bytes += n;
	if (s->flood_bytes > FLOOD_BYTES) {
		s->why = "the server sent far too much";
		s->flooded = true;
		return CURL_WRITEFUNC_ERROR;
	}
	if (s->catchup) {
		long long end = now + T_CATCHUP_QUIET, latest = s->first_byte_at + T_CATCHUP_MAX;
		s->catchup_end = end < latest ? end : latest;
	}
	ntfy_lines_feed(&s->lines, data, n, sub_line, s);
	return n;
}

static void sub_stop(struct sub *s) {
	if (s->easy) {
		curl_multi_remove_handle(multi, s->easy);
		curl_easy_cleanup(s->easy);
		s->easy = NULL;
	}
	curl_slist_free_all(s->headers);
	s->headers = NULL;
	free(s->lines.buf);
	memset(&s->lines, 0, sizeof(s->lines));
}

static void sub_start(struct sub *s) {
	long long now = now_ms();
	s->retry_at = now + T_BACKOFF_CAP;	/* unless it comes to something */
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}
	if (!multi) {
		multi = curl_multi_init();
		if (!multi) {
			return;
		}
	}
	s->lines.buf = malloc(NTFY_LINE_MAX);
	s->easy = curl_easy_init();
	if (!s->lines.buf || !s->easy) {
		sub_stop(s);
		return;
	}
	/* From where it had got to. A topic that has never had a message: from when it
	 * was added, so nothing published since is missed, and nothing from before shows. */
	s->since[0] = 0;
	if (s->last_id[0]) {
		snprintf(s->since, sizeof(s->since), "%s", s->last_id);
	} else if (s->cfg.added > 0) {
		snprintf(s->since, sizeof(s->since), "%lld", s->cfg.added);
	}
	ntfy_stream_url(s->url, sizeof(s->url), s->cfg.server, s->cfg.topic, s->since);
	easy_common(s->easy, &s->cfg, &s->headers, s->url);
	curl_easy_setopt(s->easy, CURLOPT_WRITEFUNCTION, sub_write);
	curl_easy_setopt(s->easy, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(s->easy, CURLOPT_PRIVATE, s);
	if (curl_multi_add_handle(multi, s->easy) != CURLM_OK) {
		sub_stop(s);
		return;
	}
	s->state = S_CONNECTING;
	s->started_at = s->last_data = now;
	s->first_byte_at = 0;
	s->flood_at = now;
	s->flood_bytes = 0;
	s->flooded = false;
	s->checked_status = false;
	status_dirty = true;
}

/* It ended, one way or another: try again in a while. */
static void sub_ended(struct sub *s, CURLcode code) {
	long long now = now_ms();
	long http = 0;
	bool longest = false;
	if (s->easy) {
		curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &http);
	}
	bool stayed = s->first_byte_at && now - s->first_byte_at >= T_STABLE && !s->flooded;
	/* Which of our own words fits. The server's are not repeated to anyone. */
	if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE) {
		s->why = "the server's certificate can't be trusted";
	} else if (code == CURLE_SSL_CONNECT_ERROR) {
		s->why = "a secure connection couldn't be made";
	} else if (code == CURLE_HTTP_RETURNED_ERROR && (http == 401 || http == 403)) {
		s->why = "not allowed: check the sign-in";
		longest = true;
	} else if (code == CURLE_HTTP_RETURNED_ERROR && http == 429) {
		s->why = "the server says too many requests";
		longest = true;
	} else if (code == CURLE_HTTP_RETURNED_ERROR) {
		s->why = "the server gave an error";
	} else if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT) {
		s->why = "can't reach the server";
	} else if (code != CURLE_WRITE_ERROR) {
		s->why = "the connection dropped";
	}
	say("%s: ended: %s%s", s->cfg.id, curl_easy_strerror(code), http >= 400 ? " (the server refused)" : "");
	if (s->lines.oversized) {
		say("%s: %lu lines were too long, and left out", s->cfg.id, s->lines.oversized);
	}
	sub_stop(s);
	s->state = S_WAITING;
	s->catchup = false;
	if (stayed) {
		s->attempt = 0;
	}
	if (longest || s->flooded) {
		s->attempt = ATTEMPT_LONGEST;
	}
	s->retry_at = now + ntfy_backoff_ms(s->attempt, T_BACKOFF_BASE, T_BACKOFF_CAP, random32());
	if (s->attempt < ATTEMPT_LONGEST) {
		s->attempt++;
	}
	status_dirty = true;
}

/* ---- one message ------------------------------------------------------------ */

static void sub_line(const char *line, size_t n, void *ctx) {
	struct sub *s = ctx;
	static struct ntfy_event ev;	/* (some kilobytes, one at a time) */
	static struct ntfy_item item;
	if (!ntfy_event_parse(line, n, &ev)) {
		return;
	}
	if (!strcmp(ev.event, "keepalive")) {
		/* a backlog has been sent by now */
		if (s->catchup) {
			s->catchup_end = 0;
		}
		return;
	}
	if (strcmp(ev.event, "message")) {
		return;		/* open, poll_request, and whatever comes in future */
	}
	/* no way to resume after it or tell it from a repeat, or it isn't this topic's */
	if (!ntfy_id_valid(ev.id) || (ev.topic[0] && strcmp(ev.topic, s->cfg.topic))) {
		return;
	}
	/* a time far in the future would make everything after it look old, for good */
	if (ev.time > (long long)time(NULL) + 86400) {
		ev.time = 0;
	}
	if (ntfy_seen_has(&s->seen, ev.id) || ntfy_is_old(ev.time, s->last_time)) {
		return;
	}
	ntfy_seen_add(&s->seen, ev.id);
	memset(&item, 0, sizeof(item));
	ntfy_note_make(&item.note, &ev, s->cfg.topic, s->host, have_emoji);
	snprintf(item.id, sizeof(item.id), "%s", ev.id);
	item.time = ev.time;
	ntfy_held_add(&s->held, &item);
}

/* ---- the notification service ------------------------------------------------ */

static int bus_name_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
	bus_retry_at = 0;
	bus_wait = 0;
	return 0;
}

static void bus_close(void) {
	if (bus) {
		sd_bus_flush_close_unref(bus);
		bus = NULL;
	}
}

/* The first message is when the bus is first needed. */
static bool bus_open(void) {
	if (bus) {
		return true;
	}
	if (sd_bus_open_user(&bus) < 0) {
		bus = NULL;
		return false;
	}
	/* told at once when ztnotifyd comes */
	sd_bus_add_match(bus, NULL, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
		"arg0='org.freedesktop.Notifications'", bus_name_changed, NULL);
	return true;
}

static void bus_process(void) {
	while (bus) {
		int r = sd_bus_process(bus, NULL);
		if (r < 0) {
			bus_close();
		}
		if (r <= 0) {
			break;
		}
	}
}

static bool notify(const struct ntfy_note *note) {
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	if (!bus_open()) {
		return false;
	}
	int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
		"org.freedesktop.Notifications", "Notify");
	if (r >= 0) r = sd_bus_message_append(m, "susss", note->app, (uint32_t)0, "ztntfy", note->title, note->body);
	if (r >= 0) r = sd_bus_message_append(m, "as", 0);
	/* ztnotifyd goes by suppress-sound, and by the desktop entry for the app's
	 * glyph; it ignores urgency, which is here for what else may take the name */
	if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0 && note->quiet) r = sd_bus_message_append(m, "{sv}", "suppress-sound", "b", 1);
	if (r >= 0) r = sd_bus_message_append(m, "{sv}", "urgency", "y", (uint8_t)(note->priority <= 2 ? 0 : note->priority >= 5 ? 2 : 1));
	if (r >= 0) r = sd_bus_message_append(m, "{sv}", "desktop-entry", "s", "ztntfy");
	if (r >= 0) r = sd_bus_message_close_container(m);
	if (r >= 0) r = sd_bus_message_append(m, "i", (int32_t)-1);
	if (r >= 0) r = sd_bus_call(bus, m, 5 * 1000000ULL, &error, &reply);
	bool ok = r >= 0;
	if (!ok && !sd_bus_error_is_set(&error)) {
		/* the bus itself, not the service: start again */
		bus_close();
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_message_unref(m);
	return ok;
}

/* What's ready goes out, oldest first, as far as the service takes it. */
static void deliver(long long now) {
	bool changed = false;
	for (int i = 0; i < n_subs; i++) {
		if (subs[i]->catchup && now >= subs[i]->catchup_end) {
			subs[i]->catchup = false;
		}
	}
	if (bus_retry_at && now < bus_retry_at) {
		return;
	}
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		if (s->catchup) {
			continue;
		}
		struct ntfy_note note;
		struct ntfy_item last;
		while (ntfy_held_next(&s->held, &note, &last)) {
			ntfy_bucket_fill(&s->bucket, now, T_TOKEN);
			if (s->bucket.tokens <= 0) {
				break;
			}
			if (!notify(&note)) {
				bus_wait = bus_wait ? (bus_wait * 2 > T_BUS_MAX ? T_BUS_MAX : bus_wait * 2) : T_CATCHUP_QUIET;
				bus_retry_at = now + bus_wait;
				say("the notification service isn't there: trying again in %lld ms", bus_wait);
				if (changed) {
					state_save();
				}
				return;
			}
			bus_wait = 0;
			bus_retry_at = 0;
			s->bucket.tokens--;
			snprintf(s->last_id, sizeof(s->last_id), "%s", last.id);
			ntfy_seen_add(&s->shown, last.id);
			if (last.time > s->last_time) {
				s->last_time = last.time;
			}
			s->last_shown = time(NULL);
			ntfy_held_done(&s->held);
			changed = true;
			status_dirty = true;
		}
	}
	if (changed) {
		state_save();
	}
}

/* ---- what the network is doing -------------------------------------------------- */

#define NET_KNOWN_MAX 16
static uint64_t net_known[NET_KNOWN_MAX];

/* Told only while there is something for it to matter to. */
static void net_watch(bool on) {
	if (on && net_fd < 0) {
		net_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
		struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR };
		if (net_fd >= 0 && bind(net_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(net_fd);
			net_fd = -1;
		}
		if (net_fd < 0) {
			say("no word of network changes: reconnecting by the timer alone");
		}
	} else if (!on && net_fd >= 0) {
		close(net_fd);
		net_fd = -1;
		net_kick_at = 0;
	}
}

/* Addresses are told again whenever their lifetime is refreshed, which isn't news. */
static bool net_note(bool add, uint64_t h) {
	for (int i = 0; i < NET_KNOWN_MAX; i++) {
		if (net_known[i] == h) {
			if (!add) {
				net_known[i] = 0;
			}
			return false;
		}
	}
	if (!add) {
		return false;
	}
	for (int i = 0; i < NET_KNOWN_MAX; i++) {
		if (!net_known[i]) {
			net_known[i] = h;
			return true;
		}
	}
	net_known[random32() % NET_KNOWN_MAX] = h;
	return true;
}

static void net_readable(long long now) {
	char buf[8192] __attribute__((aligned(4)));
	ssize_t got;
	while ((got = recv(net_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		int left = (int)got;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_type != RTM_NEWADDR && h->nlmsg_type != RTM_DELADDR) {
				continue;
			}
			struct ifaddrmsg *ifa = NLMSG_DATA(h);
			if (ifa->ifa_scope != RT_SCOPE_UNIVERSE || (ifa->ifa_flags & IFA_F_TENTATIVE)) {
				continue;
			}
			/* which address: enough to tell one from another */
			uint64_t id = 1469598103934665603ULL ^ ifa->ifa_index;
			int alen = (int)IFA_PAYLOAD(h);
			for (struct rtattr *a = IFA_RTA(ifa); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
				if (a->rta_type == IFA_LOCAL || (a->rta_type == IFA_ADDRESS && ifa->ifa_family == AF_INET6)) {
					const unsigned char *b = RTA_DATA(a);
					for (size_t i = 0; i < RTA_PAYLOAD(a); i++) {
						id = (id ^ b[i]) * 1099511628211ULL;
					}
				}
			}
			if (net_note(h->nlmsg_type == RTM_NEWADDR, id ? id : 1)) {
				net_kick_at = now + T_NET_SETTLE;
			}
		}
	}
}

/* A new address: nothing that was waiting need wait, and a stream that has been
 * quiet may be on a network that has gone. It has a since= to come back with. */
static void net_kick(long long now) {
	net_kick_at = 0;
	say("the network has changed");
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		long long soon = now + (long long)(random32() % (uint32_t)(T_NET_JITTER + 1));
		if (s->state == S_WAITING) {
			s->attempt = 0;
			s->retry_at = soon;
		} else if (now - s->last_data > T_NET_FRESH) {
			sub_stop(s);
			s->state = S_WAITING;
			s->catchup = false;
			s->attempt = 0;
			s->why = "the network changed";
			s->retry_at = soon;
			status_dirty = true;
		}
	}
}

/* ---- the subscriptions file ---------------------------------------------------- */

static void sub_free(struct sub *s) {
	sub_stop(s);
	explicit_bzero(&s->cfg, sizeof(s->cfg));	/* a sign-in doesn't stay about */
	free(s);
}

static int conf_read(struct ntfy_sub_conf *out, int max) {
	int n = 0;
	int fd = open(conf_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size > 256 * 1024) {
		say("%s isn't usable: not a file of this user's, or far too big", CONF_NAME);
		close(fd);
		return 0;
	}
	if (st.st_mode & 077) {
		say("%s could be read by others: closing it to this user alone", CONF_NAME);
		if (fchmod(fd, 0600) != 0) {
			say("%s: %s", CONF_NAME, strerror(errno));
		}
	}
	FILE *f = fdopen(fd, "re");
	if (!f) {
		close(fd);
		return 0;
	}
	static char line[NTFY_CONF_LINE_MAX];
	int number = 0;
	while (fgets(line, sizeof(line), f)) {
		number++;
		if (line[0] == '#' || line[0] == '\n') {
			continue;
		}
		if (n == max) {
			say("%s: only the first %d subscriptions are used", CONF_NAME, max);
			break;
		}
		if (ntfy_conf_parse(line, &out[n])) {
			bool dup = false;
			for (int i = 0; i < n; i++) {
				dup = dup || !strcmp(out[i].id, out[n].id);
			}
			if (!dup) {
				n++;
			}
		} else {
			say("%s: line %d can't be read, and is left out", CONF_NAME, number);
		}
	}
	explicit_bzero(line, sizeof(line));
	fclose(f);
	return n;
}

/* A subscription that hasn't changed keeps its connection, and a new one starts
 * without waiting. */
static void conf_load(void) {
	static struct ntfy_sub_conf wanted[NTFY_SUBS_MAX];
	int n = conf_read(wanted, NTFY_SUBS_MAX);
	bool removed = false;
	for (int i = 0; i < n_subs;) {
		bool keep = false;
		for (int k = 0; k < n; k++) {
			keep = keep || ntfy_conf_same(&subs[i]->cfg, &wanted[k]);
		}
		if (keep) {
			i++;
			continue;
		}
		say("%s: removed", subs[i]->cfg.id);
		sub_free(subs[i]);
		memmove(&subs[i], &subs[i + 1], sizeof(subs[0]) * (size_t)(n_subs - i - 1));
		n_subs--;
		removed = true;
		status_dirty = true;
	}
	for (int k = 0; k < n; k++) {
		bool have = false;
		for (int i = 0; i < n_subs; i++) {
			have = have || !strcmp(subs[i]->cfg.id, wanted[k].id);
		}
		if (have) {
			continue;
		}
		struct sub *s = calloc(1, sizeof(*s));
		if (!s) {
			continue;
		}
		s->cfg = wanted[k];
		ntfy_server_host(s->cfg.server, s->host, sizeof(s->host));
		state_load(s);
		s->state = S_WAITING;
		subs[n_subs++] = s;
		say("%s: added", s->cfg.id);
		status_dirty = true;
	}
	explicit_bzero(wanted, sizeof(wanted));
	if (removed) {
		state_save();	/* (after those added again: an edited one keeps its place) */
	}
	net_watch(n_subs > 0);
	if (!n_subs) {
		if (multi) {
			curl_multi_cleanup(multi);
			multi = NULL;
		}
		bus_close();
	}
}

/* ---- the loop ---------------------------------------------------------------- */

static void timers(long long now) {
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		if (s->state == S_WAITING && now >= s->retry_at) {
			sub_start(s);
		} else if (s->state == S_CONNECTING && now - s->started_at >= T_CONNECT_DEAD) {
			sub_ended(s, CURLE_OPERATION_TIMEDOUT);
		} else if (s->state == S_CONNECTED && now - s->last_data >= T_DEAD) {
			s->why = "the connection went quiet";
			say("%s: nothing heard: starting again", s->cfg.id);
			sub_ended(s, CURLE_WRITE_ERROR);
		}
	}
}

static void timer_arm(long long now) {
	long long next = 0;
#define SOONER(t) do { long long at_ = (t); if (!next || at_ < next) next = at_; } while (0)
	for (int i = 0; i < n_subs; i++) {
		struct sub *s = subs[i];
		if (s->state == S_WAITING) SOONER(s->retry_at);
		if (s->state == S_CONNECTING) SOONER(s->started_at + T_CONNECT_DEAD);
		if (s->state == S_CONNECTED) SOONER(s->last_data + T_DEAD);
		if (s->catchup) SOONER(s->catchup_end);
		if (!s->catchup && s->held.total) {
			if (bus_retry_at) SOONER(bus_retry_at);
			else if (s->bucket.tokens <= 0 && s->bucket.at) SOONER(s->bucket.at + T_TOKEN);
			else SOONER(now);	/* ztnotifyd has just come */
		}
	}
	if (net_kick_at) SOONER(net_kick_at);
#undef SOONER
	struct itimerspec its = { 0 };
	if (next) {
		if (next <= now) {
			next = now + 1;
		}
		its.it_value.tv_sec = next / 1000;
		its.it_value.tv_nsec = (next % 1000) * 1000000;
	}
	timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void conf_changed(void) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	bool ours = false;
	while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->len && !strcmp(e->name, CONF_NAME)) {
				ours = true;
			}
			p += sizeof(*e) + e->len;
		}
	}
	if (ours) {
		conf_load();
	}
}

static int wait_for_something(void) {
	enum { EXTRA_MAX = 5 };
	struct curl_waitfd extra[EXTRA_MAX];
	unsigned count = 0;
	int fds[] = { inotify_fd, signal_fd, timer_fd, net_fd };
	for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
		if (fds[i] >= 0) {
			extra[count++] = (struct curl_waitfd){ .fd = fds[i], .events = CURL_WAIT_POLLIN };
		}
	}
	if (bus) {
		int ev = sd_bus_get_events(bus);
		extra[count++] = (struct curl_waitfd){ .fd = sd_bus_get_fd(bus),
			.events = (short)((ev & POLLIN ? CURL_WAIT_POLLIN : 0) | (ev & POLLOUT ? CURL_WAIT_POLLOUT : 0)) };
	}
	if (!multi) {
		/* no curl in it at all while there are no topics */
		struct pollfd p[EXTRA_MAX];
		for (unsigned i = 0; i < count; i++) {
			p[i] = (struct pollfd){ .fd = extra[i].fd, .events = POLLIN };
		}
		return poll(p, count, -1);
	}
	long left = -1;
	curl_multi_timeout(multi, &left);
	/* (curl's own timers, for a connection being made; ours are in the timerfd) */
	int timeout = left < 0 || left > 24 * 3600 * 1000 ? 24 * 3600 * 1000 : (int)left;
	int numfds;
	return curl_multi_poll(multi, extra, count, timeout, &numfds) == CURLM_OK ? 0 : -1;
}

static void curl_finished(void) {
	int running, left;
	CURLMsg *msg;
	curl_multi_perform(multi, &running);
	while ((msg = curl_multi_info_read(multi, &left))) {
		if (msg->msg != CURLMSG_DONE) {
			continue;
		}
		char *p = NULL;
		curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &p);
		if (p) {
			sub_ended((struct sub *)p, msg->data.result);
		}
	}
}

/* ---- a test message ---------------------------------------------------------- */

static size_t discard(char *data, size_t size, size_t count, void *ctx) {
	return size * count;
}

/* The exit status is for ztntfy: 0 sent, 2 no such subscription, 3 the secure
 * connection, 4 the sign-in, 5 anything else. */
static int publish(const char *id) {
	static struct ntfy_sub_conf all[NTFY_SUBS_MAX];
	int n = conf_read(all, NTFY_SUBS_MAX);
	const struct ntfy_sub_conf *c = NULL;
	for (int i = 0; i < n; i++) {
		if (!strcmp(all[i].id, id)) {
			c = &all[i];
		}
	}
	if (!c) {
		return 2;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	CURL *e = curl_easy_init();
	if (!e) {
		return 5;
	}
	char url[512];
	static const char BODY[] = "This is a test from ztNTFY. If you can read this, pushes reach this phone.";
	snprintf(url, sizeof(url), "%s/%s", c->server, c->topic);
	struct curl_slist *h = curl_slist_append(NULL, "Title: ztNTFY test");
	easy_common(e, c, &h, url);
	curl_easy_setopt(e, CURLOPT_POSTFIELDS, BODY);
	curl_easy_setopt(e, CURLOPT_TIMEOUT, 25L);
	curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, discard);
	CURLcode r = curl_easy_perform(e);
	long code = 0;
	curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(h);
	curl_easy_cleanup(e);
	if (r == CURLE_OK && code >= 200 && code < 300) return 0;
	if (r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CONNECT_ERROR) return 3;
	if (code == 401 || code == 403) return 4;
	return 5;
}

/* ---- the start ---------------------------------------------------------------- */

static bool paths(void) {
	const char *home = getenv("HOME"), *rt = getenv("XDG_RUNTIME_DIR");
	if (!home || !home[0]) {
		say("no HOME");
		return false;
	}
	snprintf(conf_dir, sizeof(conf_dir), "%s/.config/ztmetro/ntfy", home);
	snprintf(conf_path, sizeof(conf_path), "%s/%s", conf_dir, CONF_NAME);
	snprintf(state_path, sizeof(state_path), "%s/.local/state/ztmetro/ntfy-state", home);
	snprintf(status_path, sizeof(status_path), "%s/ztos-ntfyd.status", rt ? rt : "/tmp");
	return true;
}

/* Whether there's a font to draw emoji with. Looked for by name, not by asking
 * fontconfig: this is one access(), once. os/packages/common installs this one. */
static bool emoji_font(void) {
	if (getenv("ZTOS_NTFYD_TEST_NO_EMOJI")) {
		return false;
	}
	return access("/usr/share/fonts/noto/NotoColorEmoji.ttf", R_OK) == 0;
}

int main(int argc, char **argv) {
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	umask(077);
	if (!paths()) {
		return 1;
	}
	if (argc == 3 && !strcmp(argv[1], "--publish")) {
		return publish(argv[2]);
	}
	if (argc != 1) {
		fprintf(stderr, "usage: ztos-ntfyd [--publish SUBSCRIPTION-ID]\n");
		return 2;
	}
	if (getenv("ZTOS_NTFYD_TEST_FAST")) {
		T_BACKOFF_BASE = 200, T_BACKOFF_CAP = 1500, T_DEAD = 3000, T_CONNECT_DEAD = 2000, T_STABLE = 1000;
		T_CATCHUP_QUIET = 300, T_CATCHUP_MAX = 1000, T_TOKEN = 800, T_BUS_MAX = 600;
		T_NET_SETTLE = 100, T_NET_JITTER = 100, T_NET_FRESH = 500;
	}
	have_emoji = emoji_font();

	char dir[440];
	snprintf(dir, sizeof(dir), "%s", state_path);
	*strrchr(dir, '/') = 0;
	mkdir_p(dir, 0700);
	mkdir_p(conf_dir, 0700);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd < 0 || inotify_add_watch(inotify_fd, conf_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE) < 0) {
		say("can't watch the subscriptions: %s", strerror(errno));
		return 1;
	}
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);
	signal_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	timer_fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (signal_fd < 0 || timer_fd < 0) {
		say("can't set up: %s", strerror(errno));
		return 1;
	}
	conf_load();
	if (n_subs) {
		say("%d topics", n_subs);
	} else {
		say("no topics: nothing to connect to");
	}

	for (;;) {
		long long now = now_ms();
		bus_process();
		timers(now);
		if (net_kick_at && now >= net_kick_at) {
			net_kick(now);
			timers(now);
		}
		deliver(now);
		if (status_dirty) {
			status_write();
		}
		bus_process();
		timer_arm(now_ms());
		if (wait_for_something() < 0 && errno != EINTR) {
			say("waiting failed: %s", strerror(errno));
			sleep(5);
		}
		struct signalfd_siginfo si;
		bool stop = false, reload = false;
		while (read(signal_fd, &si, sizeof(si)) == sizeof(si)) {
			if (si.ssi_signo == SIGHUP) {
				reload = true;
			} else {
				stop = true;
			}
		}
		if (stop) {
			break;
		}
		if (reload) {
			conf_load();
		}
		uint64_t ticks;
		if (read(timer_fd, &ticks, sizeof(ticks)) < 0 && errno != EAGAIN) {
			say("the timer: %s", strerror(errno));
		}
		conf_changed();
		if (net_fd >= 0) {
			net_readable(now_ms());
		}
		if (multi) {
			curl_finished();
		}
	}
	for (int i = 0; i < n_subs; i++) {
		sub_stop(subs[i]);
	}
	unlink(status_path);
	bus_close();
	return 0;
}
