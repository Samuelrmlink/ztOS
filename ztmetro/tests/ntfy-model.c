// SPDX-License-Identifier: GPL-3.0-or-later
/* ntfy's parsing and sanitising with hostile input: no network, no bus, no libraries
 * (make check-ntfy). Build it with -fsanitize=address,undefined when the machine can. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#include "../ntfy/ntfy-model.h"

#define LEN(s) (sizeof(s) - 1)

static void valid_utf8(const char *s) {
	size_t n = strlen(s);
	for (size_t i = 0; i < n;) {
		uint32_t c;
		size_t got = ntfy_utf8_next(s + i, n - i, &c);
		/* U+FFFD is fine if it was written as such, never if it stands for a bad byte */
		assert(!(c == 0xfffd && got == 1));
		i += got;
	}
}

static void clean_is(const char *in, size_t n, const char *want) {
	char out[256];
	ntfy_clean(out, sizeof(out), in, n, true);
	if (strcmp(out, want)) {
		fprintf(stderr, "clean: got [%s], wanted [%s]\n", out, want);
		assert(0);
	}
	valid_utf8(out);
}
#define CLEAN(in, want) clean_is(in, LEN(in), want)

static void test_clean(void) {
	CLEAN("hello", "hello");
	CLEAN("caf\xc3\xa9 \xf0\x9f\x94\xa5", "caf\xc3\xa9 \xf0\x9f\x94\xa5");
	/* one line, with single spaces, and none at the ends */
	CLEAN("  a\n\nb\tc\r\nd\xe2\x80\xa8" "e  ", "a b c d e");
	/* a terminal's escapes and other control characters */
	CLEAN("a\x1b[31mred\x07\x7f\xc2\x9b" "2Jb", "a[31mred2Jb");
	/* a NUL in the bytes ends nothing */
	CLEAN("a\0b", "ab");
	/* text that reads other than it is */
	CLEAN("safe\xe2\x80\xae" "exe.txt", "safeexe.txt");
	CLEAN("\xe2\x81\xa6" "a\xe2\x81\xa9\xe2\x80\x8f\xd8\x9c" "b", "ab");
	/* what can't be seen */
	CLEAN("a\xc2\xad" "b\xe2\x80\x8b" "c\xef\xbb\xbf" "d\xf3\xa0\x80\x81\xee\x80\x80" "e", "abcde");
	/* not UTF-8: overlong, surrogate, past the end, cut short, stray */
	CLEAN("\xc0\xaf", "\xef\xbf\xbd\xef\xbf\xbd");
	CLEAN("\xed\xa0\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
	CLEAN("\xf4\x90\x80\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
	CLEAN("a\xe2\x82", "a\xef\xbf\xbd\xef\xbf\xbd");
	CLEAN("\x80", "\xef\xbf\xbd");
	/* markup is words, and an open "<" doesn't cost ztnotifyd the rest of the text */
	CLEAN("<b>x</b> & <a href='y'>", "\xe2\x80\xb9" "b\xe2\x80\xba" "x\xe2\x80\xb9/b\xe2\x80\xba & \xe2\x80\xb9" "a href='y'\xe2\x80\xba");
	CLEAN("%s%n", "%s%n");
	/* a tower of accents stops at three */
	char zalgo[400] = "a";
	for (int i = 0; i < 60; i++) strcat(zalgo, "\xcc\x81");
	char out[256];
	ntfy_clean(out, sizeof(out), zalgo, strlen(zalgo), true);
	assert(strlen(out) == 1 + 3 * 2);
	/* a family emoji holds together */
	CLEAN("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb");

	/* no emoji font: gone, along with what was stacked on them */
	const char *family = "a \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\xef\xb8\x8f b";
	ntfy_clean(out, sizeof(out), family, strlen(family), false);
	assert(!strcmp(out, "a b"));

	/* cut on a character's edge, whatever the size, always ending in an ellipsis */
	for (size_t size = 4; size < 40; size++) {
		char small[64];
		memset(small, 'Z', sizeof(small));
		const char *text = "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac";
		ntfy_clean(small, size, text, strlen(text), true);
		assert(strlen(small) < size);
		valid_utf8(small);
		assert(small[size] == 'Z');
		if (size <= strlen(text)) assert(strlen(small) >= 3 && !strcmp(small + strlen(small) - 3, "\xe2\x80\xa6"));
	}
	/* an exact fit isn't cut */
	ntfy_clean(out, 6, "abcde", 5, true);
	assert(!strcmp(out, "abcde"));
	ntfy_clean(out, 3, "abcde", 5, true);
	assert(!strcmp(out, ""));
}

static struct ntfy_event ev;

static bool parse(const char *s) { return ntfy_event_parse(s, strlen(s), &ev); }

static void test_parse(void) {
	assert(parse("{\"id\":\"abcDEF123456\",\"time\":1700000000,\"expires\":1700043200,\"event\":\"message\","
		"\"topic\":\"phone\",\"title\":\"Hi\",\"message\":\"line\\none\",\"priority\":5,"
		"\"tags\":[\"warning\",\"cd\"],\"click\":\"https://evil.example\","
		"\"actions\":[{\"action\":\"http\",\"url\":\"https://evil.example\",\"clear\":true}],"
		"\"attachment\":{\"url\":\"https://evil.example/x\",\"name\":\"x.png\",\"size\":3}}"));
	assert(!strcmp(ev.event, "message") && !strcmp(ev.id, "abcDEF123456") && ev.time == 1700000000 && ev.priority == 5);
	assert(!strcmp(ev.title, "Hi") && !strcmp(ev.message, "line\none") && ev.n_tags == 2 && !strcmp(ev.tags[1], "cd"));
	assert(!strcmp(ev.attachment, "x.png"));

	assert(parse("{\"event\":\"keepalive\",\"id\":\"a\",\"time\":1,\"topic\":\"t\"}") && !strcmp(ev.event, "keepalive"));
	assert(parse("  {} \r") && !ev.event[0] && ev.priority == 3);

	/* nonsense for a priority is the default */
	static const char *const PRI[] = { "0", "6", "-1", "9999999999999999999999", "1.5", "1e0", "\"5\"", "null", "true", "[5]" };
	for (size_t i = 0; i < sizeof(PRI) / sizeof(PRI[0]); i++) {
		char line[80];
		snprintf(line, sizeof(line), "{\"priority\":%s}", PRI[i]);
		assert(parse(line) && ev.priority == 3);
	}
	/* wrong types are as if absent */
	assert(parse("{\"id\":1,\"event\":[\"message\"],\"message\":{\"a\":1},\"tags\":\"x\",\"attachment\":\"y\",\"time\":\"z\"}"));
	assert(!ev.id[0] && !ev.event[0] && !ev.message[0] && !ev.n_tags && !ev.attachment[0] && !ev.time);
	assert(parse("{\"time\":-5}") && !ev.time);
	assert(parse("{\"time\":99999999999999999999999}") && !ev.time);

	/* not JSON, or more than one thing */
	static const char *const BAD[] = {
		"", " ", "x", "[]", "1", "null", "\"s\"", "{", "}", "{\"a\"", "{\"a\":", "{\"a\":1", "{\"a\":1,", "{\"a\":1,}",
		"{a:1}", "{'a':1}", "{\"a\":1}x", "{\"a\":1}{\"b\":2}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":1e}", "{\"a\":-}",
		"{\"a\":+1}", "{\"a\":tru}", "{\"a\":\"x}", "{\"a\":\"\\x\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"\\uZZZZ\"}",
		"{\"a\":\"\\", "{\"a\":[1,]}", "{\"a\":[1 2]}", "{\"a\":{\"b\"}}", "{\"message\":\"raw\ttab\"}", "{\"message\":\"raw\nnl\"}",
		"{\"tags\":[\"a\",]}", "{\"tags\":[\"a\"", "{\"attachment\":{\"name\":}}", "{\"attachment\":{\"name\":\"a\"",
	};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		if (parse(BAD[i])) {
			fprintf(stderr, "took: %s\n", BAD[i]);
			assert(0);
		}
	}
	/* a NUL inside the line: not read as the line's end */
	assert(!ntfy_event_parse("{\"a\":1}\0garbage", 15, &ev));
	assert(!ntfy_event_parse("{\"message\":\"a\0b\"}", 17, &ev));

	/* nesting that would take the stack down */
	static char deep[NTFY_LINE_MAX];
	strcpy(deep, "{\"a\":");
	size_t o = strlen(deep);
	for (int i = 0; i < 40000; i++) deep[o++] = '[';
	deep[o] = 0;
	assert(!parse(deep));
	o = strlen("{\"a\":");
	for (int i = 0; i < 10; i++) deep[o++] = '[';
	for (int i = 0; i < 10; i++) deep[o++] = ']';
	deep[o++] = '}';
	deep[o] = 0;
	assert(parse(deep));

	/* \u: pairs joined, halves not, and no NUL in the text */
	assert(parse("{\"message\":\"\\ud83d\\udd25 \\u00e9 \\u0000x \\ud83d y \\udd25 z \\ud83d\\u0041\"}"));
	assert(!strcmp(ev.message, "\xf0\x9f\x94\xa5 \xc3\xa9 \xef\xbf\xbdx \xef\xbf\xbd y \xef\xbf\xbd z \xef\xbf\xbd" "A"));

	/* Far too long for its field: cut on a character's edge, and the rest still read */
	static char big[NTFY_LINE_MAX];
	o = (size_t)sprintf(big, "{\"id\":\"");
	for (int i = 0; i < 500; i++) big[o++] = 'i';
	o += (size_t)sprintf(big + o, "\",\"message\":\"");
	for (int i = 0; i < 15000; i++) o += (size_t)sprintf(big + o, "\xe2\x82\xac");
	o += (size_t)sprintf(big + o, "\",\"title\":\"end\",\"tags\":[");
	for (int i = 0; i < 100; i++) o += (size_t)sprintf(big + o, "%s\"tagtagtagtagtagtagtagtagtagtagtagtagtagtag\"", i ? "," : "");
	sprintf(big + o, "]}");
	assert(parse(big));
	assert(strlen(ev.id) == NTFY_ID_MAX && !ntfy_id_valid("") && !strcmp(ev.title, "end"));
	assert(strlen(ev.message) < NTFY_RAW_MESSAGE_MAX && strlen(ev.message) % 3 == 0);
	valid_utf8(ev.message);
	assert(ev.n_tags == NTFY_TAGS_MAX && strlen(ev.tags[0]) == NTFY_TAG_MAX);

	/* every cut of a real line is refused or read, and nothing runs off the end */
	const char *real = "{\"id\":\"x\",\"event\":\"message\",\"title\":\"\\ud83d\\udd25\",\"tags\":[\"a\"],\"attachment\":{\"name\":\"n\"},\"priority\":2}";
	for (size_t n = 0; n < strlen(real); n++) {
		char *copy = malloc(n ? n : 1);
		memcpy(copy, real, n);
		assert(!ntfy_event_parse(copy, n, &ev));
		free(copy);
	}
}

static int lines_got;
static char lines_last[128];
static void on_line(const char *s, size_t n, void *ctx) {
	(void)ctx;
	lines_got++;
	snprintf(lines_last, sizeof(lines_last), "%.*s", (int)n, s);
}

static void test_lines(void) {
	struct ntfy_lines l = { .buf = malloc(NTFY_LINE_MAX) };
	ntfy_lines_feed(&l, "ab", 2, on_line, NULL);
	assert(lines_got == 0);
	ntfy_lines_feed(&l, "c\nde\n\n\nf", 7, on_line, NULL);
	assert(lines_got == 2 && !strcmp(lines_last, "de"));

	/* a line past the limit in many pieces: dropped whole, once, and the next one is fine */
	static char chunk[4096];
	memset(chunk, 'x', sizeof(chunk));
	lines_got = 0;
	for (int i = 0; i < 100; i++) ntfy_lines_feed(&l, chunk, sizeof(chunk), on_line, NULL);
	assert(l.oversized == 1 && lines_got == 0 && l.skipping);
	ntfy_lines_feed(&l, "tail\nnext\n", 10, on_line, NULL);
	assert(lines_got == 1 && !strcmp(lines_last, "next") && !l.skipping && l.oversized == 1);

	/* exactly the limit is kept; one more isn't */
	static char exact[NTFY_LINE_MAX + 1];
	memset(exact, 'y', sizeof(exact));
	lines_got = 0;
	exact[NTFY_LINE_MAX] = '\n';
	ntfy_lines_feed(&l, exact, NTFY_LINE_MAX + 1, on_line, NULL);
	assert(lines_got == 1 && l.oversized == 1);
	memset(exact, 'y', sizeof(exact));
	ntfy_lines_feed(&l, exact, NTFY_LINE_MAX + 1, on_line, NULL);
	ntfy_lines_feed(&l, "\n", 1, on_line, NULL);
	assert(lines_got == 1 && l.oversized == 2);
	free(l.buf);
}

static void server_is(const char *in, const char *want) {
	char out[NTFY_SERVER_MAX + 1];
	bool ok = ntfy_server_normalise(in, out, sizeof(out));
	if (want ? !ok || strcmp(out, want) : ok) {
		fprintf(stderr, "server [%s]: %s [%s]\n", in, ok ? "got" : "refused", ok ? out : "");
		assert(0);
	}
}

static void test_names(void) {
	server_is("ntfy.sh", "https://ntfy.sh");
	server_is("  ntfy.sh/ ", "https://ntfy.sh");
	server_is("https://ntfy.sh", "https://ntfy.sh");
	server_is("HTTPS://ntfy.sh//", "https://ntfy.sh");
	server_is("http://10.0.0.2:8080", "http://10.0.0.2:8080");
	server_is("https://my.host/ntfy/", "https://my.host/ntfy");
	server_is("[::1]:80", "https://[::1]:80");
	server_is("http://[fe80::1]", "http://[fe80::1]");
	/* http never comes from anywhere but being typed */
	server_is("localhost:8080", "https://localhost:8080");

	static const char *const BAD[] = {
		"", " ", "/", "https://", "http://", "ftp://x", "file:///etc/passwd", "gopher://x", "javascript:alert(1)",
		"https://user:pass@host", "user@host", "https://host?x=1", "https://host#f", "https://ho st", "https://host:0",
		"https://host:99999", "https://host:123456", "https://host:x", "https://host:", "https://host/a b",
		"https://host/../x", "https://host/%2e", "https://ho\nst", "https://host\r\nX: y", "https://[::1", "https://[]",
		"https://[zz]", "https://-host", "https://.host", "https://h\xc3\xa9", "https://host\\x", "ntfy.sh/a://b",
	};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) server_is(BAD[i], NULL);
	static char long_host[1000];
	memset(long_host, 'a', sizeof(long_host) - 1);
	server_is(long_host, NULL);

	assert(ntfy_server_is_https("https://a") && !ntfy_server_is_https("http://a"));
	char host[64];
	ntfy_server_host("https://my.host:8/p/q", host, sizeof(host));
	assert(!strcmp(host, "my.host:8"));

	assert(ntfy_topic_valid("my-topic_1") && !ntfy_topic_valid("") && !ntfy_topic_valid("a/b") && !ntfy_topic_valid("a,b") &&
		!ntfy_topic_valid("a b") && !ntfy_topic_valid("..") && !ntfy_topic_valid("a?x") && !ntfy_topic_valid("\xc3\xa9"));
	char t65[66];
	memset(t65, 'a', 65);
	t65[65] = 0;
	assert(!ntfy_topic_valid(t65));
	t65[64] = 0;
	assert(ntfy_topic_valid(t65));

	/* an id from a server goes into the next address: none of this does */
	static const char *const IDS[] = { "", "a&b=c", "a b", "a\r\nX: y", "../x", "a#b", "a%00", "a?b", "\xc3\xa9" };
	for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) assert(!ntfy_id_valid(IDS[i]));
	assert(ntfy_id_valid("sPs71M8A2T9q"));

	char url[128];
	assert(ntfy_stream_url(url, sizeof(url), "https://a", "t", "x1") && !strcmp(url, "https://a/t/json?since=x1"));
	assert(ntfy_stream_url(url, sizeof(url), "https://a", "t", "") && !strcmp(url, "https://a/t/json"));
	assert(!ntfy_stream_url(url, 8, "https://a", "t", "x1"));
}

static void test_conf(void) {
	struct ntfy_sub_conf a = { .id = "0123456789abcdef", .added = 1700000000, .server = "https://a.b", .topic = "t-1",
		.user = "me", .pass = "p w d%:\xc3\xa9!", .token = "" }, b;
	char line[NTFY_CONF_LINE_MAX];
	assert(ntfy_conf_format(line, sizeof(line), &a));
	assert(!strchr(line, ' ') && strchr(line, '\n') == line + strlen(line) - 1);
	assert(ntfy_conf_parse(line, &b));
	assert(ntfy_conf_same(&a, &b));

	/* the longest of everything fits the line */
	memset(&b, 0, sizeof(b));
	memset(b.user, 0xc3, NTFY_CRED_MAX);
	memset(b.pass, 0xc3, NTFY_CRED_MAX);
	memset(b.token, 'a', NTFY_CRED_MAX);
	memset(b.server, 'a', NTFY_SERVER_MAX);
	memcpy(b.server, "https://", 8);
	memset(b.topic, 'a', NTFY_TOPIC_MAX);
	memset(b.id, 'a', NTFY_SUB_ID_LEN);
	b.added = 1LL << 40;
	assert(ntfy_conf_format(line, sizeof(line), &b));
	struct ntfy_sub_conf c;
	assert(ntfy_conf_parse(line, &c) && ntfy_conf_same(&b, &c));

	static const char *const BAD[] = {
		"", "#x\t1\thttps://a\tt\t\t\t", "x", "a\t1\thttps://a\tt\t\t", "a\t1\thttps://a\tt\t\t\t\tmore",
		"a b\t1\thttps://a\tt\t\t\t", "a\tx\thttps://a\tt\t\t\t", "a\t\thttps://a\tt\t\t\t", "a\t-1\thttps://a\tt\t\t\t",
		"a\t1\tntfy.sh\tt\t\t\t", "a\t1\tftp://a\tt\t\t\t", "a\t1\thttps://a\tt/x\t\t\t", "a\t1\thttps://a\t\t\t\t",
		"a\t1\thttps://a\tt\t%0A\t\t", "a\t1\thttps://a\tt\t\t%0d\t", "a\t1\thttps://a\tt\t\t\tb%20c", "a\t1\thttps://a\tt\t\t\t%ZZ",
		"a\t1\thttps://a\tt\t\t\t%0", "a\t1\thttps://a\tt\t\t\t%", "a\t1\thttps://a\tt\tx%3Ay\t\t",
	};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		snprintf(line, sizeof(line), "%s", BAD[i]);
		if (ntfy_conf_parse(line, &c)) {
			fprintf(stderr, "took: %s\n", BAD[i]);
			assert(0);
		}
	}
	/* a line that names everything is the plain one */
	snprintf(line, sizeof(line), "a\t1\thttps://a\tt\tme\t%%41\ttok\r\n");
	assert(ntfy_conf_parse(line, &c) && !strcmp(c.pass, "A") && !strcmp(c.token, "tok"));

	assert(!ntfy_user_valid("a:b") && ntfy_user_valid("ab"));
	char enc[8];
	assert(!ntfy_pct_encode(enc, sizeof(enc), "\xc3\xa9\xc3\xa9\xc3\xa9"));
}

static void test_seen(void) {
	struct ntfy_seen s = { 0 };
	char id[16];
	for (int i = 0; i < NTFY_SEEN_MAX + 5; i++) {
		snprintf(id, sizeof(id), "id%d", i);
		ntfy_seen_add(&s, id);
		ntfy_seen_add(&s, id);
	}
	assert(s.n == NTFY_SEEN_MAX && ntfy_seen_has(&s, "id36") && !ntfy_seen_has(&s, "id4") && ntfy_seen_has(&s, "id5"));

	char line[(NTFY_SEEN_SAVED + 2) * (NTFY_ID_MAX + 2) + 64], sub[20], last[NTFY_ID_MAX + 1];
	long long t, shown;
	struct ntfy_seen back;
	assert(ntfy_state_format(line, sizeof(line), "abc", "last1", 17, 18, &s));
	assert(ntfy_state_parse(line, sub, sizeof(sub), last, sizeof(last), &t, &shown, &back));
	assert(!strcmp(sub, "abc") && !strcmp(last, "last1") && t == 17 && shown == 18);
	assert(back.n == NTFY_SEEN_SAVED && ntfy_seen_has(&back, "id36") && ntfy_seen_has(&back, "id29") && !ntfy_seen_has(&back, "id28"));

	struct ntfy_seen none = { 0 };
	assert(ntfy_state_format(line, sizeof(line), "abc", "", 0, 0, &none));
	assert(ntfy_state_parse(line, sub, sizeof(sub), last, sizeof(last), &t, &shown, &back) && !last[0] && !back.n);

	/* a state file that has been got at can't put anything in the next address */
	static const char *const BAD[] = { "a\tx&y=1\t1\t1\t", "a\tx\t1\t1\ty z", "a\tx\t1\t1", "a b\tx\t1\t1\t", "a\tx\tq\t1\t", "a\tx\t1\tq\t", "a\tx\t-1\t1\t", "" };
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		snprintf(line, sizeof(line), "%s\n", BAD[i]);
		assert(!ntfy_state_parse(line, sub, sizeof(sub), last, sizeof(last), &t, &shown, &back));
	}

	assert(!ntfy_is_old(100, 0) && !ntfy_is_old(0, 100) && !ntfy_is_old(1000, 1000) && !ntfy_is_old(1000, 1000 + NTFY_OLD_SLACK_S));
	assert(ntfy_is_old(1000, 1001 + NTFY_OLD_SLACK_S));
}

static void test_note(void) {
	struct ntfy_note n;
	memset(&ev, 0, sizeof(ev));
	ev.priority = 3;
	strcpy(ev.message, "body");
	ntfy_note_make(&n, &ev, "topic", "ntfy.sh", true);
	assert(!strcmp(n.app, "topic") && !strcmp(n.title, "ntfy.sh") && !strcmp(n.body, "body") && !n.quiet);
	for (int p = 1; p <= 5; p++) {
		ev.priority = p;
		ntfy_note_make(&n, &ev, "t", "h", true);
		assert(n.quiet == (p <= 2) && n.priority == p);
	}
	ev.priority = 3;

	/* known tags in front as emoji, the others as words; with no font, all as words */
	strcpy(ev.title, "Disk");
	strcpy(ev.tags[0], "warning");
	strcpy(ev.tags[1], "backup-01");
	strcpy(ev.tags[2], "");
	ev.n_tags = 3;
	ntfy_note_make(&n, &ev, "t", "h", true);
	assert(!strcmp(n.title, "\xe2\x9a\xa0\xef\xb8\x8f Disk") && !strcmp(n.body, "body [backup-01]"));
	ntfy_note_make(&n, &ev, "t", "h", false);
	assert(!strcmp(n.title, "Disk") && !strcmp(n.body, "body [warning, backup-01]"));

	/* a tag is not a way in */
	strcpy(ev.tags[0], "\x1b[2J<b>\xe2\x80\xae");
	ev.n_tags = 1;
	ntfy_note_make(&n, &ev, "t", "h", true);
	assert(!strcmp(n.body, "body [[2J\xe2\x80\xb9" "b\xe2\x80\xba]"));

	/* of an attachment, its name and never its address */
	ev.n_tags = 0;
	strcpy(ev.attachment, "cat.png");
	ntfy_note_make(&n, &ev, "t", "h", true);
	assert(!strcmp(n.body, "body [attachment: cat.png]"));

	/* always something to read, and always within what the shell keeps */
	memset(&ev, 0, sizeof(ev));
	strcpy(ev.title, "\x1b\xe2\x80\xae");
	strcpy(ev.message, "\x07");
	ntfy_note_make(&n, &ev, "t", "ntfy.sh", true);
	assert(!strcmp(n.title, "ntfy.sh") && !strcmp(n.body, "(no text)"));

	memset(&ev, 0, sizeof(ev));
	for (size_t i = 0; i + 3 < sizeof(ev.title) - 1; i += 3) memcpy(ev.title + i, "\xe2\x82\xac", 3);
	for (size_t i = 0; i + 3 < sizeof(ev.message) - 1; i += 3) memcpy(ev.message + i, "\xe2\x82\xac", 3);
	for (int i = 0; i < NTFY_TAGS_MAX; i++) {
		memset(ev.tags[i], 'x', NTFY_TAG_MAX);
		ev.tags[i][NTFY_TAG_MAX] = 0;
	}
	ev.n_tags = NTFY_TAGS_MAX;
	memset(ev.attachment, 'a', sizeof(ev.attachment) - 1);
	char topic[NTFY_TOPIC_MAX + 1];
	memset(topic, 't', NTFY_TOPIC_MAX);
	topic[NTFY_TOPIC_MAX] = 0;
	ntfy_note_make(&n, &ev, topic, "h", true);
	assert(strlen(n.app) < NTFY_NOTE_APP && strlen(n.title) < NTFY_NOTE_TITLE && strlen(n.body) < NTFY_NOTE_BODY);
	valid_utf8(n.title);
	valid_utf8(n.body);
}

static struct ntfy_item item(const char *id, int priority, const char *text) {
	struct ntfy_item it;
	memset(&it, 0, sizeof(it));
	snprintf(it.id, sizeof(it.id), "%s", id);
	it.note.priority = priority;
	it.note.quiet = priority <= 2;
	snprintf(it.note.title, sizeof(it.note.title), "%s", text);
	snprintf(it.note.body, sizeof(it.note.body), "%s", text);
	return it;
}

static void test_held(void) {
	struct ntfy_held h = { 0 };
	struct ntfy_note n;
	struct ntfy_item last, it;
	assert(!ntfy_held_next(&h, &n, &last));

	/* a few are shown one by one, oldest first */
	for (int i = 0; i < NTFY_HELD_KEEP; i++) {
		char id[8];
		snprintf(id, sizeof(id), "m%d", i);
		it = item(id, 3, id);
		ntfy_held_add(&h, &it);
	}
	for (int i = 0; i < NTFY_HELD_KEEP; i++) {
		char id[8];
		snprintf(id, sizeof(id), "m%d", i);
		assert(ntfy_held_next(&h, &n, &last) && !strcmp(last.id, id) && !strcmp(n.body, id));
		ntfy_held_done(&h);
	}
	assert(!ntfy_held_next(&h, &n, &last) && !h.total);

	/* many are one summary, whose id is the newest, with the text of the most urgent */
	it = item("first", 2, "first");
	ntfy_held_add(&h, &it);
	it = item("urgent", 5, "urgent");
	ntfy_held_add(&h, &it);
	for (int i = 0; i < 500; i++) {
		char id[8];
		snprintf(id, sizeof(id), "m%d", i);
		it = item(id, 2, id);
		ntfy_held_add(&h, &it);
	}
	assert(ntfy_held_next(&h, &n, &last));
	assert(!strcmp(n.title, "502 new messages") && !strcmp(n.body, "urgent") && !n.quiet && !strcmp(last.id, "m499"));
	ntfy_held_done(&h);
	assert(!ntfy_held_next(&h, &n, &last));

	/* a backlog of nothing but quiet ones stays quiet */
	for (int i = 0; i < 9; i++) {
		it = item("q", 1, "q");
		ntfy_held_add(&h, &it);
	}
	assert(ntfy_held_next(&h, &n, &last) && n.quiet);
	ntfy_held_done(&h);

	/* quiet ones between loud ones keep their own say */
	it = item("a", 4, "a");
	ntfy_held_add(&h, &it);
	it = item("b", 1, "b");
	ntfy_held_add(&h, &it);
	assert(ntfy_held_next(&h, &n, &last) && !n.quiet);
	ntfy_held_done(&h);
	assert(ntfy_held_next(&h, &n, &last) && n.quiet);
	ntfy_held_done(&h);
}

static void test_bucket(void) {
	struct ntfy_bucket b = { 0 };
	ntfy_bucket_fill(&b, 1000, 100);
	assert(b.tokens == NTFY_BUCKET_BURST);
	b.tokens = 0;
	ntfy_bucket_fill(&b, 1099, 100);
	assert(b.tokens == 0);
	ntfy_bucket_fill(&b, 1100, 100);
	assert(b.tokens == 1 && b.at == 1100);
	ntfy_bucket_fill(&b, 1100 + 100LL * 1000000, 100);
	assert(b.tokens == NTFY_BUCKET_BURST);
}

static void test_backoff(void) {
	for (int attempt = 0; attempt < 40; attempt++) {
		long long expect = 5000;
		for (int i = 0; i < attempt && expect < 300000; i++) expect *= 2;
		if (expect > 300000) expect = 300000;
		for (uint32_t r = 0; r < 200; r++) {
			long long w = ntfy_backoff_ms(attempt, 5000, 300000, r * 2654435761u);
			assert(w >= expect / 2 && w <= expect);
		}
	}
	assert(ntfy_backoff_ms(1 << 30, 5000, 300000, 0xffffffffu) <= 300000);
	assert(ntfy_backoff_ms(0, 5000, 300000, 0) == 2500);
}

int main(void) {
	test_clean();
	test_parse();
	test_lines();
	test_names();
	test_conf();
	test_seen();
	test_note();
	test_held();
	test_bucket();
	test_backoff();
	puts("ntfy-model: ok");
	return 0;
}
