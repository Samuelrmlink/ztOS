// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ntfy: everything that can be decided without a socket, a bus or a clock.
 *
 * Whatever a server sends is untrusted, and so is the file a person may have
 * edited by hand, so this is where both are read: one JSON line of the
 * subscription, text made fit to show, addresses and names checked, what the
 * saved files hold, what to do with a backlog. tests/ntfy-model.c runs it all
 * on any machine with hostile input (make check-ntfy).
 */
#ifndef ZTOS_NTFY_MODEL_H
#define ZTOS_NTFY_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NTFY_LINE_MAX		65536	/* one line of JSON; ntfy's own limit for a message is 4 KiB */
#define NTFY_TOPIC_MAX		64	/* as ntfy's own rule for a topic */
#define NTFY_ID_MAX		64
#define NTFY_SERVER_MAX		200
#define NTFY_CRED_MAX		256
#define NTFY_SUB_ID_LEN		16
#define NTFY_SUBS_MAX		16
#define NTFY_TAGS_MAX		5
#define NTFY_TAG_MAX		32
#define NTFY_RAW_TITLE_MAX	512
#define NTFY_RAW_MESSAGE_MAX	8192
#define NTFY_RAW_NAME_MAX	128

/* The shell keeps a notification in fields of these sizes and cuts by bytes
 * (ztmetro.c's notes[]), which can leave half a character: everything is made
 * to fit here, on a character's edge. */
#define NTFY_NOTE_APP		32
#define NTFY_NOTE_TITLE		48
#define NTFY_NOTE_BODY		200

/* ---- text fit to show --------------------------------------------------- */

/* The next character of s (n bytes, n > 0): how many bytes it took. Anything
 * that isn't valid UTF-8 (overlong, surrogate, past the end of Unicode, cut
 * short) is U+FFFD, and takes one byte. */
static inline size_t ntfy_utf8_next(const char *s, size_t n, uint32_t *cp) {
	const unsigned char *p = (const unsigned char *)s;
	unsigned c = p[0];
	size_t len;
	uint32_t v, min;
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		len = 2, v = c & 0x1f, min = 0x80;
	} else if (c >= 0xe0 && c <= 0xef) {
		len = 3, v = c & 0x0f, min = 0x800;
	} else if (c >= 0xf0 && c <= 0xf4) {
		len = 4, v = c & 0x07, min = 0x10000;
	} else {
		*cp = 0xfffd;
		return 1;
	}
	if (n < len) {
		*cp = 0xfffd;
		return 1;
	}
	for (size_t i = 1; i < len; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*cp = 0xfffd;
			return 1;
		}
		v = (v << 6) | (p[i] & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
		*cp = 0xfffd;
		return 1;
	}
	*cp = v;
	return len;
}

static inline size_t ntfy_utf8_put(char *out, uint32_t c) {
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | c >> 6);
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | c >> 12);
		out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | c >> 18);
	out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

enum ntfy_char { NTFY_KEEP, NTFY_DROP, NTFY_SPACE, NTFY_MARK };

static inline enum ntfy_char ntfy_char_class(uint32_t c) {
	if (c == '\n' || c == '\r' || c == '\t' || c == 0x0b || c == 0x0c || c == 0x85) return NTFY_SPACE;
	if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) return NTFY_DROP;	/* terminal escapes among them */
	if (c == 0xa0 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
			c == 0x205f || c == 0x3000) return NTFY_SPACE;
	/* what reorders the text around it, so that a line reads other than it is */
	if (c == 0x061c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) ||
			(c >= 0x2066 && c <= 0x2069)) return NTFY_DROP;
	/* what is there but can't be seen */
	if (c == 0xad || c == 0x034f || c == 0x180e || c == 0x200b || (c >= 0x2060 && c <= 0x206f) ||
			c == 0x115f || c == 0x1160 || c == 0x3164 || c == 0xfeff || c == 0xffa0 ||
			(c >= 0xfff0 && c <= 0xfffc)) return NTFY_DROP;
	/* private use, language tags, and code points that are promised never to be characters */
	if ((c >= 0xe000 && c <= 0xf8ff) || (c >= 0xe0000 && c <= 0xe0fff) || c >= 0xf0000 ||
			(c >= 0xfdd0 && c <= 0xfdef) || (c & 0xfffe) == 0xfffe) return NTFY_DROP;
	/* What stacks on the character before. Kept (joiners make emoji and some
	 * scripts' words), but only a few in a row: see ntfy_clean. */
	if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05bd) ||
			(c >= 0x0610 && c <= 0x061a) || (c >= 0x064b && c <= 0x065f) || c == 0x0e31 ||
			(c >= 0x0e34 && c <= 0x0e3a) || (c >= 0x0e47 && c <= 0x0e4e) ||
			(c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
			c == 0x200c || c == 0x200d || (c >= 0x20d0 && c <= 0x20ff) ||
			(c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f)) return NTFY_MARK;
	return NTFY_KEEP;
}

#define NTFY_MARKS_MAX 3

static inline bool ntfy_is_emoji(uint32_t c) { return c >= 0x1f000 && c <= 0x1faff; }

/* in, n bytes, as plain text of fewer than `size` bytes on one line: valid
 * UTF-8, nothing that can't be seen or that turns the text around, spaces
 * single and none at the ends. Cut on a character's edge, with an ellipsis.
 *
 * "<" and ">" become their look-alikes: ztnotifyd takes out what looks like
 * markup, and an open "<" takes the rest of the text with it. Nothing else here
 * is special: not "&", not "%", and "|" is ztnotifyd's to deal with.
 *
 * `emoji`: false when there's no font to draw them, and they are left out. */
static inline void ntfy_clean(char *out, size_t size, const char *in, size_t n, bool emoji) {
	size_t o = 0, fit = 0;	/* fit: the last place the ellipsis would still go */
	bool space = false, cut = false, gone = false;
	int marks = 0;
	if (size < 4) {
		if (size) out[0] = 0;
		return;
	}
	for (size_t i = 0; i < n;) {
		uint32_t c;
		i += ntfy_utf8_next(in + i, n - i, &c);
		enum ntfy_char kind = ntfy_char_class(c);
		if (kind == NTFY_DROP) continue;
		if (kind == NTFY_SPACE || c == ' ') {
			space = true;
			gone = false;
			marks = 0;
			continue;
		}
		if (!emoji && ntfy_is_emoji(c)) {
			gone = true;
			continue;
		}
		if (kind == NTFY_MARK) {
			/* (what was stacked on an emoji that has been left out goes with it) */
			if (gone || !o || ++marks > NTFY_MARKS_MAX) continue;
		} else {
			marks = 0;
			gone = false;
		}
		if (c == '<') c = 0x2039;
		else if (c == '>') c = 0x203a;
		char enc[4];
		size_t len = ntfy_utf8_put(enc, c), extra = space && o ? 1 : 0;
		if (o + extra + len >= size) {
			cut = true;
			break;
		}
		if (extra) out[o++] = ' ';
		space = false;
		memcpy(out + o, enc, len);
		o += len;
		if (o + 3 < size) fit = o;
	}
	if (cut) {
		o = fit;
		while (o && out[o - 1] == ' ') o--;
		memcpy(out + o, "\xe2\x80\xa6", 3);
		o += 3;
	}
	out[o] = 0;
}

/* ---- one line of the subscription ---------------------------------------- */

struct ntfy_event {
	char event[24];
	char id[NTFY_ID_MAX + 1];
	char topic[NTFY_TOPIC_MAX + 1];
	long long time;
	int priority;		/* 1..5; 3 when it says nothing or nonsense */
	char title[NTFY_RAW_TITLE_MAX];
	char message[NTFY_RAW_MESSAGE_MAX];
	char attachment[NTFY_RAW_NAME_MAX];
	char tags[NTFY_TAGS_MAX][NTFY_TAG_MAX + 1];
	int n_tags;
};

struct ntfy_json { const char *p, *end; };

static inline void ntfy_json_ws(struct ntfy_json *j) {
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static inline int ntfy_json_hex4(const char *p) {
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

/* A string, at its opening quote. What doesn't fit in `out` is read and let go,
 * cut on a character's edge; out == NULL only checks it. False: it isn't JSON. */
static inline bool ntfy_json_string(struct ntfy_json *j, char *out, size_t size) {
	size_t o = 0;
	bool full = out == NULL || size == 0;
	if (j->p >= j->end || *j->p != '"') return false;
	j->p++;
	while (j->p < j->end && *j->p != '"') {
		unsigned char c = (unsigned char)*j->p;
		char enc[4];
		size_t len;
		if (c < 0x20) return false;	/* JSON has no raw control characters in a string */
		if (c != '\\') {
			enc[0] = (char)c;
			len = 1;
			j->p++;
		} else {
			if (j->end - j->p < 2) return false;
			char e = j->p[1];
			j->p += 2;
			uint32_t cp;
			switch (e) {
			case '"': case '\\': case '/': cp = (uint32_t)e; break;
			case 'b': cp = 8; break;
			case 'f': cp = 12; break;
			case 'n': cp = 10; break;
			case 'r': cp = 13; break;
			case 't': cp = 9; break;
			case 'u': {
				if (j->end - j->p < 4) return false;
				int u = ntfy_json_hex4(j->p);
				if (u < 0) return false;
				j->p += 4;
				cp = (uint32_t)u;
				if (u >= 0xdc00 && u <= 0xdfff) {
					cp = 0xfffd;	/* the second half of a pair, alone */
				} else if (u >= 0xd800 && u <= 0xdbff) {
					int low = -1;
					if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') low = ntfy_json_hex4(j->p + 2);
					if (low >= 0xdc00 && low <= 0xdfff) {
						cp = 0x10000 + (((uint32_t)u - 0xd800) << 10) + ((uint32_t)low - 0xdc00);
						j->p += 6;
					} else {
						cp = 0xfffd;
					}
				}
				if (cp == 0) cp = 0xfffd;	/* no NUL in a C string to end the text early */
				break;
			}
			default: return false;
			}
			len = ntfy_utf8_put(enc, cp);
		}
		if (!full) {
			if (o + len >= size) {
				full = true;
				/* not half a character at the end */
				while (o > 0 && ((unsigned char)out[o - 1] & 0xc0) == 0x80) o--;
				if (o > 0 && (unsigned char)out[o - 1] >= 0xc0) o--;
			} else {
				memcpy(out + o, enc, len);
				o += len;
			}
		}
	}
	if (j->p >= j->end) return false;
	j->p++;
	if (out && size) out[o] = 0;
	return true;
}

/* A number, as JSON writes it. An integer that fits goes to *value when asked;
 * *is_int says whether it was one. */
static inline bool ntfy_json_number(struct ntfy_json *j, long long *value, bool *is_int) {
	const char *p = j->p;
	bool neg = false, big = false, plain = true;
	unsigned long long v = 0;
	if (p < j->end && *p == '-') {
		neg = true;
		p++;
	}
	if (p >= j->end || *p < '0' || *p > '9') return false;
	if (*p == '0' && p + 1 < j->end && p[1] >= '0' && p[1] <= '9') return false;
	for (; p < j->end && *p >= '0' && *p <= '9'; p++) {
		if (v > (unsigned long long)(INT64_MAX - 9) / 10) big = true;
		else v = v * 10 + (unsigned long long)(*p - '0');
	}
	if (p < j->end && *p == '.') {
		plain = false;
		p++;
		if (p >= j->end || *p < '0' || *p > '9') return false;
		while (p < j->end && *p >= '0' && *p <= '9') p++;
	}
	if (p < j->end && (*p == 'e' || *p == 'E')) {
		plain = false;
		p++;
		if (p < j->end && (*p == '+' || *p == '-')) p++;
		if (p >= j->end || *p < '0' || *p > '9') return false;
		while (p < j->end && *p >= '0' && *p <= '9') p++;
	}
	j->p = p;
	if (is_int) *is_int = plain && !big;
	if (value) *value = plain && !big ? (neg ? -(long long)v : (long long)v) : 0;
	return true;
}

#define NTFY_JSON_DEPTH 16

/* Any value, read and let go. Nested no deeper than a real message ever is. */
static inline bool ntfy_json_skip(struct ntfy_json *j, int depth) {
	if (depth > NTFY_JSON_DEPTH) return false;
	ntfy_json_ws(j);
	if (j->p >= j->end) return false;
	char c = *j->p;
	if (c == '"') return ntfy_json_string(j, NULL, 0);
	if (c == '{' || c == '[') {
		char close = c == '{' ? '}' : ']';
		j->p++;
		ntfy_json_ws(j);
		if (j->p < j->end && *j->p == close) {
			j->p++;
			return true;
		}
		for (;;) {
			if (c == '{') {
				ntfy_json_ws(j);
				if (!ntfy_json_string(j, NULL, 0)) return false;
				ntfy_json_ws(j);
				if (j->p >= j->end || *j->p != ':') return false;
				j->p++;
			}
			if (!ntfy_json_skip(j, depth + 1)) return false;
			ntfy_json_ws(j);
			if (j->p >= j->end) return false;
			if (*j->p == close) {
				j->p++;
				return true;
			}
			if (*j->p != ',') return false;
			j->p++;
		}
	}
	size_t left = (size_t)(j->end - j->p);
	if (left >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; return true; }
	if (left >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return true; }
	if (left >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return true; }
	return ntfy_json_number(j, NULL, NULL);
}

/* The attachment's name, and nothing else of it: never its address. */
static inline bool ntfy_json_attachment(struct ntfy_json *j, char *name, size_t size) {
	ntfy_json_ws(j);
	if (j->p >= j->end || *j->p != '{') return ntfy_json_skip(j, 1);
	j->p++;
	ntfy_json_ws(j);
	if (j->p < j->end && *j->p == '}') {
		j->p++;
		return true;
	}
	for (;;) {
		char key[16];
		ntfy_json_ws(j);
		if (!ntfy_json_string(j, key, sizeof(key))) return false;
		ntfy_json_ws(j);
		if (j->p >= j->end || *j->p != ':') return false;
		j->p++;
		ntfy_json_ws(j);
		if (!strcmp(key, "name") && j->p < j->end && *j->p == '"') {
			if (!ntfy_json_string(j, name, size)) return false;
		} else if (!ntfy_json_skip(j, 2)) {
			return false;
		}
		ntfy_json_ws(j);
		if (j->p >= j->end) return false;
		if (*j->p == '}') {
			j->p++;
			return true;
		}
		if (*j->p != ',') return false;
		j->p++;
	}
}

static inline bool ntfy_json_tags(struct ntfy_event *ev, struct ntfy_json *j) {
	ntfy_json_ws(j);
	if (j->p >= j->end || *j->p != '[') return ntfy_json_skip(j, 1);
	j->p++;
	ntfy_json_ws(j);
	if (j->p < j->end && *j->p == ']') {
		j->p++;
		return true;
	}
	for (;;) {
		ntfy_json_ws(j);
		if (j->p < j->end && *j->p == '"' && ev->n_tags < NTFY_TAGS_MAX) {
			if (!ntfy_json_string(j, ev->tags[ev->n_tags], sizeof(ev->tags[0]))) return false;
			ev->n_tags++;
		} else if (!ntfy_json_skip(j, 2)) {
			return false;
		}
		ntfy_json_ws(j);
		if (j->p >= j->end) return false;
		if (*j->p == ']') {
			j->p++;
			return true;
		}
		if (*j->p != ',') return false;
		j->p++;
	}
}

/* One line, as one JSON object. False for anything else: it is then ignored.
 * Fields of the wrong type count as absent; the rest of the object is checked
 * all the same. */
static inline bool ntfy_event_parse(const char *line, size_t n, struct ntfy_event *ev) {
	struct ntfy_json j = { line, line + n };
	memset(ev, 0, sizeof(*ev));
	ev->priority = 3;
	ntfy_json_ws(&j);
	if (j.p >= j.end || *j.p != '{') return false;
	j.p++;
	ntfy_json_ws(&j);
	if (j.p < j.end && *j.p == '}') {
		j.p++;
	} else {
		for (;;) {
			char key[32];
			ntfy_json_ws(&j);
			if (!ntfy_json_string(&j, key, sizeof(key))) return false;
			ntfy_json_ws(&j);
			if (j.p >= j.end || *j.p != ':') return false;
			j.p++;
			ntfy_json_ws(&j);
			if (j.p >= j.end) return false;
			char *text = NULL;
			size_t size = 0;
			if (!strcmp(key, "event")) text = ev->event, size = sizeof(ev->event);
			else if (!strcmp(key, "id")) text = ev->id, size = sizeof(ev->id);
			else if (!strcmp(key, "topic")) text = ev->topic, size = sizeof(ev->topic);
			else if (!strcmp(key, "title")) text = ev->title, size = sizeof(ev->title);
			else if (!strcmp(key, "message")) text = ev->message, size = sizeof(ev->message);
			if (text && *j.p == '"') {
				if (!ntfy_json_string(&j, text, size)) return false;
			} else if (!strcmp(key, "time") && (*j.p == '-' || (*j.p >= '0' && *j.p <= '9'))) {
				long long v;
				bool is_int;
				if (!ntfy_json_number(&j, &v, &is_int)) return false;
				if (is_int && v > 0) ev->time = v;
			} else if (!strcmp(key, "priority") && (*j.p == '-' || (*j.p >= '0' && *j.p <= '9'))) {
				long long v;
				bool is_int;
				if (!ntfy_json_number(&j, &v, &is_int)) return false;
				ev->priority = is_int && v >= 1 && v <= 5 ? (int)v : 3;
			} else if (!strcmp(key, "tags")) {
				if (!ntfy_json_tags(ev, &j)) return false;
			} else if (!strcmp(key, "attachment")) {
				if (!ntfy_json_attachment(&j, ev->attachment, sizeof(ev->attachment))) return false;
			} else if (!ntfy_json_skip(&j, 1)) {
				return false;
			}
			ntfy_json_ws(&j);
			if (j.p >= j.end) return false;
			if (*j.p == '}') {
				j.p++;
				break;
			}
			if (*j.p != ',') return false;
			j.p++;
		}
	}
	ntfy_json_ws(&j);
	return j.p == j.end;
}

/* Bytes as they come, in lines. A line too long to keep is thrown away whole,
 * up to its newline, and counted; the next line is read as usual. */
struct ntfy_lines {
	char *buf;		/* NTFY_LINE_MAX bytes, the owner's */
	size_t len;
	bool skipping;
	unsigned long oversized;
};

static inline void ntfy_lines_feed(struct ntfy_lines *l, const char *data, size_t n,
		void (*line)(const char *s, size_t n, void *ctx), void *ctx) {
	while (n) {
		const char *nl = memchr(data, '\n', n);
		size_t take = nl ? (size_t)(nl - data) : n;
		if (!l->skipping) {
			if (l->len + take > NTFY_LINE_MAX) {
				l->skipping = true;
				l->oversized++;
				l->len = 0;
			} else {
				memcpy(l->buf + l->len, data, take);
				l->len += take;
			}
		}
		data += take;
		n -= take;
		if (nl) {
			if (!l->skipping && l->len) line(l->buf, l->len, ctx);
			l->len = 0;
			l->skipping = false;
			data++;
			n--;
		}
	}
}

/* ---- names and addresses -------------------------------------------------- */

static inline bool ntfy_name_valid(const char *s, size_t max) {
	size_t n = strlen(s);
	if (!n || n > max) return false;
	for (; *s; s++) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
	}
	return true;
}
static inline bool ntfy_topic_valid(const char *s) { return ntfy_name_valid(s, NTFY_TOPIC_MAX); }
/* ntfy's ids are 12 letters and digits. What is saved goes into an address, so
 * nothing else gets near it. */
static inline bool ntfy_id_valid(const char *s) { return ntfy_name_valid(s, NTFY_ID_MAX); }

/* What a person typed, as "scheme://host[:port][/path]" with no slash at the
 * end. Without a scheme it means https. http only when it says http://, and any
 * other scheme, a name and password in the address, a query or anything odd is
 * refused. */
static inline bool ntfy_server_normalise(const char *in, char *out, size_t size) {
	while (*in == ' ') in++;
	size_t n = strlen(in);
	while (n && in[n - 1] == ' ') n--;
	while (n && in[n - 1] == '/') n--;
	const char *scheme = "https";
	const char *rest = in;
	const char *sep = NULL;
	for (size_t i = 0; i + 2 < n; i++) {
		if (in[i] == ':' && in[i + 1] == '/' && in[i + 2] == '/') {
			sep = in + i;
			break;
		}
	}
	if (sep) {
		size_t sl = (size_t)(sep - in);
		if (sl == 5 && !strncasecmp(in, "https", 5)) scheme = "https";
		else if (sl == 4 && !strncasecmp(in, "http", 4)) scheme = "http";
		else return false;
		rest = sep + 3;
		n -= sl + 3;
	}
	/* the host, up to a port or a path */
	size_t i = 0;
	if (n && rest[0] == '[') {
		/* an IPv6 address */
		i = 1;
		while (i < n && rest[i] != ']') {
			char c = rest[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return false;
			i++;
		}
		if (i >= n || i == 1) return false;
		i++;
	} else {
		while (i < n && rest[i] != ':' && rest[i] != '/') {
			char c = rest[i];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
			i++;
		}
		if (!i || rest[0] == '-' || rest[0] == '.') return false;
	}
	if (i < n && rest[i] == ':') {
		size_t p = i + 1;
		long port = 0;
		while (p < n && rest[p] >= '0' && rest[p] <= '9' && p - i <= 5) port = port * 10 + (rest[p++] - '0');
		if (p == i + 1 || port < 1 || port > 65535 || (p < n && rest[p] != '/')) return false;
		i = p;
	}
	for (size_t k = i; k < n; k++) {
		char c = rest[k];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')) return false;
		if (c == '.' && k + 1 < n && rest[k + 1] == '.') return false;
	}
	int w = snprintf(out, size, "%s://%.*s", scheme, (int)n, rest);
	return w > 0 && (size_t)w < size && (size_t)w <= NTFY_SERVER_MAX;
}

static inline bool ntfy_server_is_https(const char *server) { return !strncmp(server, "https://", 8); }

/* Just the host (and port) of a checked server, for showing. */
static inline void ntfy_server_host(const char *server, char *out, size_t size) {
	const char *p = strstr(server, "://");
	p = p ? p + 3 : server;
	size_t n = strcspn(p, "/");
	snprintf(out, size, "%.*s", (int)n, p);
}

/* `since`: a message's id, or a time as seconds. Both have been checked. */
static inline bool ntfy_stream_url(char *out, size_t size, const char *server, const char *topic, const char *since) {
	int w = snprintf(out, size, "%s/%s/json%s%s", server, topic, since && since[0] ? "?since=" : "", since ? since : "");
	return w > 0 && (size_t)w < size;
}

/* ---- the subscriptions file ----------------------------------------------- */

struct ntfy_sub_conf {
	char id[NTFY_SUB_ID_LEN + 1];
	long long added;
	char server[NTFY_SERVER_MAX + 1];
	char topic[NTFY_TOPIC_MAX + 1];
	char user[NTFY_CRED_MAX + 1];
	char pass[NTFY_CRED_MAX + 1];
	char token[NTFY_CRED_MAX + 1];
};

/* Anything but what is plainly safe goes as %XX, so that a password may hold a
 * space or a percent sign. False when it would not fit. */
static inline bool ntfy_pct_encode(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.';
		if (o + (safe ? 1 : 3) >= size) return false;
		if (safe) {
			out[o++] = (char)*p;
		} else {
			static const char HEX[] = "0123456789ABCDEF";
			out[o++] = '%';
			out[o++] = HEX[*p >> 4];
			out[o++] = HEX[*p & 15];
		}
	}
	out[o] = 0;
	return true;
}

/* False for bad escapes, for what won't fit, and for control characters: these
 * end up in an HTTP header. */
static inline bool ntfy_pct_decode(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (const char *p = in; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '%') {
			static const char H[] = "0123456789ABCDEF";
			const char *a = p[1] ? strchr(H, p[1]) : NULL;
			const char *b = a && p[2] ? strchr(H, p[2]) : NULL;
			if (!a || !b) return false;
			c = (unsigned char)((a - H) << 4 | (b - H));
			p += 2;
		}
		if (c < 0x20 || c == 0x7f || o + 1 >= size) return false;
		out[o++] = (char)c;
	}
	out[o] = 0;
	return true;
}

/* A token goes in a header as it is: no spaces in it. */
static inline bool ntfy_token_valid(const char *s) {
	if (strlen(s) > NTFY_CRED_MAX) return false;
	for (; *s; s++) {
		if ((unsigned char)*s <= 0x20 || (unsigned char)*s >= 0x7f) return false;
	}
	return true;
}

/* A user and a password are joined by ":" in HTTP's basic sign-in. */
static inline bool ntfy_user_valid(const char *s) { return strlen(s) <= NTFY_CRED_MAX && !strchr(s, ':'); }

/* "id added server topic user pass token", with tabs between them. */
static inline bool ntfy_conf_format(char *out, size_t size, const struct ntfy_sub_conf *c) {
	char user[NTFY_CRED_MAX * 3 + 1], pass[NTFY_CRED_MAX * 3 + 1], token[NTFY_CRED_MAX * 3 + 1];
	if (!ntfy_pct_encode(user, sizeof(user), c->user) || !ntfy_pct_encode(pass, sizeof(pass), c->pass) ||
			!ntfy_pct_encode(token, sizeof(token), c->token)) return false;
	int w = snprintf(out, size, "%s\t%lld\t%s\t%s\t%s\t%s\t%s\n", c->id, c->added, c->server, c->topic, user, pass, token);
	return w > 0 && (size_t)w < size;
}

#define NTFY_CONF_LINE_MAX (NTFY_SUB_ID_LEN + NTFY_SERVER_MAX + NTFY_TOPIC_MAX + 3 * 3 * NTFY_CRED_MAX + 48)

/* Takes `line` apart in place. False for anything that isn't quite right; the
 * line is then ignored, and says nothing of what was in it. */
static inline bool ntfy_conf_parse(char *line, struct ntfy_sub_conf *c) {
	char *f[7];
	size_t n = strcspn(line, "\r\n");
	line[n] = 0;
	memset(c, 0, sizeof(*c));
	if (!line[0] || line[0] == '#') return false;
	f[0] = line;
	for (int i = 1; i < 7; i++) {
		char *tab = strchr(f[i - 1], '\t');
		if (!tab) return false;
		*tab = 0;
		f[i] = tab + 1;
	}
	if (strchr(f[6], '\t')) return false;
	if (!ntfy_name_valid(f[0], NTFY_SUB_ID_LEN) || !ntfy_topic_valid(f[3])) return false;
	char *end;
	long long added = strtoll(f[1], &end, 10);
	if (!f[1][0] || *end || added < 0) return false;
	if (!strstr(f[2], "://") || !ntfy_server_normalise(f[2], c->server, sizeof(c->server))) return false;
	if (!ntfy_pct_decode(c->user, sizeof(c->user), f[4]) || !ntfy_pct_decode(c->pass, sizeof(c->pass), f[5]) ||
			!ntfy_pct_decode(c->token, sizeof(c->token), f[6])) return false;
	if (!ntfy_token_valid(c->token) || !ntfy_user_valid(c->user)) return false;
	snprintf(c->id, sizeof(c->id), "%s", f[0]);
	snprintf(c->topic, sizeof(c->topic), "%s", f[3]);
	c->added = added;
	return true;
}

static inline bool ntfy_conf_same(const struct ntfy_sub_conf *a, const struct ntfy_sub_conf *b) {
	return !strcmp(a->id, b->id) && a->added == b->added && !strcmp(a->server, b->server) && !strcmp(a->topic, b->topic) &&
		!strcmp(a->user, b->user) && !strcmp(a->pass, b->pass) && !strcmp(a->token, b->token);
}

/* ---- what has been seen ---------------------------------------------------- */

#define NTFY_SEEN_MAX	32
#define NTFY_SEEN_SAVED	8	/* of them, the newest kept across a restart */

struct ntfy_seen {
	char ids[NTFY_SEEN_MAX][NTFY_ID_MAX + 1];
	int n, next;
};

static inline bool ntfy_seen_has(const struct ntfy_seen *s, const char *id) {
	for (int i = 0; i < s->n; i++) {
		if (!strcmp(s->ids[i], id)) return true;
	}
	return false;
}

static inline void ntfy_seen_add(struct ntfy_seen *s, const char *id) {
	if (ntfy_seen_has(s, id)) return;
	snprintf(s->ids[s->next], sizeof(s->ids[0]), "%s", id);
	s->next = (s->next + 1) % NTFY_SEEN_MAX;
	if (s->n < NTFY_SEEN_MAX) s->n++;
}

/* A replay of what was seen long ago, past the end of what's remembered, with
 * some room because times are in whole seconds and publishers race. */
#define NTFY_OLD_SLACK_S 60
static inline bool ntfy_is_old(long long time, long long last_time) {
	return last_time > 0 && time > 0 && time + NTFY_OLD_SLACK_S < last_time;
}

/* The saved file: "subscription-id last-id last-time last-shown seen,seen,..."
 * Back in: false, and nothing used, unless every id in it is a proper one. */
static inline bool ntfy_state_format(char *out, size_t size, const char *sub, const char *last_id, long long last_time,
		long long shown, const struct ntfy_seen *s) {
	int w = snprintf(out, size, "%s\t%s\t%lld\t%lld\t", sub, last_id[0] ? last_id : "-", last_time, shown);
	if (w <= 0 || (size_t)w >= size) return false;
	size_t o = (size_t)w;
	int count = s->n < NTFY_SEEN_SAVED ? s->n : NTFY_SEEN_SAVED;
	for (int k = count; k >= 1; k--) {
		const char *id = s->ids[(s->next - k + NTFY_SEEN_MAX) % NTFY_SEEN_MAX];
		w = snprintf(out + o, size - o, "%s%s", id, k > 1 ? "," : "");
		if (w <= 0 || (size_t)w >= size - o) return false;
		o += (size_t)w;
	}
	if (o + 2 > size) return false;
	out[o++] = '\n';
	out[o] = 0;
	return true;
}

static inline bool ntfy_state_parse(char *line, char *sub, size_t sub_size, char *last_id, size_t id_size,
		long long *last_time, long long *shown, struct ntfy_seen *s) {
	char *f[5];
	line[strcspn(line, "\r\n")] = 0;
	f[0] = line;
	for (int i = 1; i < 5; i++) {
		char *tab = strchr(f[i - 1], '\t');
		if (!tab) return false;
		*tab = 0;
		f[i] = tab + 1;
	}
	if (!ntfy_name_valid(f[0], NTFY_SUB_ID_LEN)) return false;
	if (strcmp(f[1], "-") && !ntfy_id_valid(f[1])) return false;
	char *end;
	long long t = strtoll(f[2], &end, 10), when;
	if (!f[2][0] || *end || t < 0) return false;
	when = strtoll(f[3], &end, 10);
	if (!f[3][0] || *end || when < 0) return false;
	memset(s, 0, sizeof(*s));
	for (char *id = f[4]; *id;) {
		char *comma = strchr(id, ',');
		if (comma) *comma = 0;
		if (!ntfy_id_valid(id)) return false;
		ntfy_seen_add(s, id);
		if (!comma) break;
		id = comma + 1;
	}
	snprintf(sub, sub_size, "%s", f[0]);
	snprintf(last_id, id_size, "%s", strcmp(f[1], "-") ? f[1] : "");
	*last_time = t;
	*shown = when;
	return true;
}

/* ---- from a message to a notification -------------------------------------- */

struct ntfy_note {
	char app[NTFY_NOTE_APP], title[NTFY_NOTE_TITLE], body[NTFY_NOTE_BODY];
	bool quiet;
	int priority;
};

/* The tags ntfy's own apps turn into emoji that come up most. An exact match
 * only; anything else stays a word. */
static const struct { const char *tag, *emoji; } NTFY_EMOJI[] = {
	{ "+1", "\xf0\x9f\x91\x8d" }, { "-1", "\xf0\x9f\x91\x8e" }, { "warning", "\xe2\x9a\xa0\xef\xb8\x8f" },
	{ "rotating_light", "\xf0\x9f\x9a\xa8" }, { "skull", "\xf0\x9f\x92\x80" }, { "tada", "\xf0\x9f\x8e\x89" },
	{ "white_check_mark", "\xe2\x9c\x85" }, { "heavy_check_mark", "\xe2\x9c\x94\xef\xb8\x8f" }, { "x", "\xe2\x9d\x8c" },
	{ "no_entry", "\xe2\x9b\x94" }, { "no_entry_sign", "\xf0\x9f\x9a\xab" }, { "fire", "\xf0\x9f\x94\xa5" },
	{ "bell", "\xf0\x9f\x94\x94" }, { "rocket", "\xf0\x9f\x9a\x80" }, { "loudspeaker", "\xf0\x9f\x93\xa2" },
	{ "mega", "\xf0\x9f\x93\xa3" }, { "computer", "\xf0\x9f\x92\xbb" }, { "floppy_disk", "\xf0\x9f\x92\xbe" },
	{ "package", "\xf0\x9f\x93\xa6" }, { "robot", "\xf0\x9f\xa4\x96" }, { "bug", "\xf0\x9f\x90\x9b" },
	{ "lock", "\xf0\x9f\x94\x92" }, { "key", "\xf0\x9f\x94\x91" }, { "zap", "\xe2\x9a\xa1" },
	{ "information_source", "\xe2\x84\xb9\xef\xb8\x8f" }, { "question", "\xe2\x9d\x93" },
	{ "exclamation", "\xe2\x9d\x97" }, { "email", "\xf0\x9f\x93\xa7" }, { "moneybag", "\xf0\x9f\x92\xb0" },
	{ "calendar", "\xf0\x9f\x93\x85" }, { "clock", "\xf0\x9f\x95\x90" }, { "partying_face", "\xf0\x9f\xa5\xb3" },
};

static inline const char *ntfy_tag_emoji(const char *tag) {
	for (size_t i = 0; i < sizeof(NTFY_EMOJI) / sizeof(NTFY_EMOJI[0]); i++) {
		if (!strcmp(tag, NTFY_EMOJI[i].tag)) return NTFY_EMOJI[i].emoji;
	}
	return NULL;
}

/* The app's line is the topic. The title is what the message says or, like
 * ntfy's own default, where it came from. Tags come in front of the title as
 * emoji when a font can draw them, and otherwise as words at the end of the
 * text, as do tags nothing is known about. ntfy's priorities 1 and 2 don't
 * alert. Addresses (click, attachment, actions) were never read: only the
 * attachment's name shows, as a word. */
static inline void ntfy_note_make(struct ntfy_note *note, const struct ntfy_event *ev, const char *topic, const char *host, bool emoji) {
	char title[NTFY_RAW_TITLE_MAX + NTFY_TAGS_MAX * 16], body[NTFY_RAW_MESSAGE_MAX + 512];
	size_t t = 0, b = 0;
	char words[NTFY_TAGS_MAX * (NTFY_TAG_MAX + 2) + 1];
	size_t w = 0;
	for (int i = 0; i < ev->n_tags; i++) {
		if (!ev->tags[i][0]) continue;
		const char *e = emoji ? ntfy_tag_emoji(ev->tags[i]) : NULL;
		if (e) {
			t += (size_t)snprintf(title + t, sizeof(title) - t, "%s", e);
		} else {
			w += (size_t)snprintf(words + w, sizeof(words) - w, "%s%s", w ? ", " : "", ev->tags[i]);
		}
	}
	if (t) title[t++] = ' ';
	snprintf(title + t, sizeof(title) - t, "%s", ev->title[0] ? ev->title : host);
	b = (size_t)snprintf(body, sizeof(body), "%s", ev->message);
	if (b >= sizeof(body)) b = sizeof(body) - 1;
	if (ev->attachment[0]) {
		b += (size_t)snprintf(body + b, sizeof(body) - b, " [attachment: %s]", ev->attachment);
		if (b >= sizeof(body)) b = sizeof(body) - 1;
	}
	if (w) {
		snprintf(body + b, sizeof(body) - b, " [%s]", words);
	}
	ntfy_clean(note->app, sizeof(note->app), topic, strlen(topic), emoji);
	ntfy_clean(note->title, sizeof(note->title), title, strlen(title), emoji);
	ntfy_clean(note->body, sizeof(note->body), body, strlen(body), emoji);
	/* (a message of nothing but what was taken out still arrived) */
	if (!note->title[0]) ntfy_clean(note->title, sizeof(note->title), host, strlen(host), emoji);
	if (!note->body[0]) snprintf(note->body, sizeof(note->body), "(no text)");
	note->priority = ev->priority;
	note->quiet = ev->priority <= 2;
}

/* ---- waiting to be shown ---------------------------------------------------- *
 *
 * What came and hasn't been shown yet. Not all of it is kept: a long time away
 * comes to the newest few, a count, and the most urgent. Anything not shown can
 * be asked for again (the saved id only moves on once it has been), so losing
 * this costs nothing. */
#define NTFY_HELD_KEEP 3	/* this many or fewer are shown one by one; more are one summary */

struct ntfy_item {
	struct ntfy_note note;
	char id[NTFY_ID_MAX + 1];
	long long time;
};

struct ntfy_held {
	struct ntfy_item items[NTFY_HELD_KEEP];	/* oldest first */
	struct ntfy_item top;			/* the most urgent; of those, the newest */
	int n;
	unsigned long total;
	bool loud;				/* any of them alerts */
};

static inline void ntfy_held_add(struct ntfy_held *h, const struct ntfy_item *it) {
	if (h->n == NTFY_HELD_KEEP) {
		memmove(&h->items[0], &h->items[1], sizeof(h->items[0]) * (NTFY_HELD_KEEP - 1));
		h->n--;
	}
	h->items[h->n++] = *it;
	if (!h->total || it->note.priority >= h->top.note.priority) h->top = *it;
	h->total++;
	h->loud = h->loud || !it->note.quiet;
}

/* What to show next, and the id that's then been dealt with. False: nothing. */
static inline bool ntfy_held_next(const struct ntfy_held *h, struct ntfy_note *out, struct ntfy_item *last) {
	if (!h->total) return false;
	if (h->total <= NTFY_HELD_KEEP) {
		*out = h->items[0].note;
		*last = h->items[0];
		return true;
	}
	char title[NTFY_NOTE_TITLE];
	/* (the text of the most urgent, from the title it came with) */
	snprintf(title, sizeof(title), "%lu new messages", h->total);
	*out = h->top.note;
	memcpy(out->title, title, sizeof(title));
	out->quiet = !h->loud;
	*last = h->items[h->n - 1];
	return true;
}

/* That was shown. */
static inline void ntfy_held_done(struct ntfy_held *h) {
	if (h->total <= NTFY_HELD_KEEP && h->total > 0) {
		memmove(&h->items[0], &h->items[1], sizeof(h->items[0]) * (NTFY_HELD_KEEP - 1));
		h->n--;
		h->total--;
		h->loud = false;
		for (int i = 0; i < h->n; i++) h->loud = h->loud || !h->items[i].note.quiet;
		if (h->n) {
			h->top = h->items[0];
			for (int i = 1; i < h->n; i++) {
				if (h->items[i].note.priority >= h->top.note.priority) h->top = h->items[i];
			}
		}
	} else {
		memset(h, 0, sizeof(*h));
	}
}

/* So many alerts at most, however fast they are published: a few at once, then
 * one each so often, and what is waiting by then becomes a summary. */
#define NTFY_BUCKET_BURST 10

struct ntfy_bucket { int tokens; long long at; };

static inline void ntfy_bucket_fill(struct ntfy_bucket *b, long long now, long long every_ms) {
	if (!b->at) {
		b->tokens = NTFY_BUCKET_BURST;
		b->at = now;
		return;
	}
	if (b->tokens >= NTFY_BUCKET_BURST) {
		b->at = now;
		return;
	}
	long long gained = (now - b->at) / every_ms;
	if (gained > 0) {
		b->tokens = gained >= NTFY_BUCKET_BURST - b->tokens ? NTFY_BUCKET_BURST : b->tokens + (int)gained;
		b->at += gained * every_ms;
	}
}

/* ---- waiting between tries ------------------------------------------------- */

/* The wait before try number `attempt` (from 0): doubling up to the cap, each a
 * random amount between half of that and all of it, so that phones and topics
 * don't come back all at once. `random` is any random number. */
static inline long long ntfy_backoff_ms(int attempt, long long base, long long cap, uint32_t random) {
	long long wait = base;
	for (int i = 0; i < attempt && wait < cap; i++) wait *= 2;
	if (wait > cap) wait = cap;
	long long half = wait / 2;
	return half + (long long)(random % (uint64_t)(half + 1));
}

#endif
