/* What ztos-audiod says about the outputs, for the action centre's audio tile and
 * its modal: reading the state file, naming an output's type, and the slider's
 * rules. Kept apart from the shell so the host can test it (tests/audio-state.c).
 * The file comes from outside the shell, so nothing here trusts it: anything
 * malformed or cut short is refused whole and the last good state stands. */
#ifndef ZT_AUDIO_STATE_H
#define ZT_AUDIO_STATE_H
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio-sync.h"

#define AUDIO_MAX_OUTPUTS 8
#define AUDIO_STATE_MAX_BYTES 65536	/* bigger is not a file this daemon wrote */
#define AUDIO_JSON_DEPTH 16
#define AUDIO_SLIDER_GAP_MS 120		/* the daemon runs wpctl for each volume command */
#define AUDIO_SWITCH_WAIT_MS 8000	/* a swap to a device can take a few seconds */

struct audio_output {
	char name[160], desc[96], kind[24], detail[24];
	int percent;		/* 0..100, -1 not known */
	bool mute, current;
};

struct audio_state {
	int n;
	struct audio_output out[AUDIO_MAX_OUTPUTS];
};

/* An output's type: the daemon's kind, and its "detail" when it comes to send one.
 * The first match wins, so an entry with a detail goes above the same kind without.
 * A new type is one line here. icon is a name for draw_icon. */
struct audio_type { const char *kind, *detail, *label, *icon; };
static const struct audio_type AUDIO_TYPES[] = {
	{ "speaker", NULL, "Phone Speaker", "speaker" },
	{ "bluetooth", NULL, "A2DP", "bluetooth" },
	/* Nothing reports which headphones yet, so this says no more than the daemon
	 * does. 3.5mm AUX, 3.5mm Headset, USB AUX and USB Headset go above it, once
	 * the daemon can tell them apart, as { "headphones", "DETAIL", ... }. */
	{ "headphones", NULL, "Headphones", "music" },
	{ NULL, NULL, "Audio Output", "music" },	/* a kind not known here: keep this last */
};

static inline const struct audio_type *audio_type_of(const struct audio_output *o) {
	const struct audio_type *t = AUDIO_TYPES;
	for (; t->kind; t++) {
		if (strcmp(t->kind, o->kind) == 0 && (!t->detail || strcmp(t->detail, o->detail) == 0)) break;
	}
	return t;
}

/* ---- reading JSON: only what this file needs ------------------------------ */
struct audio_json { const char *p, *end; };

static inline void aj_ws(struct audio_json *j) {
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static inline bool aj_eat(struct audio_json *j, char c) {
	aj_ws(j);
	if (j->p >= j->end || *j->p != c) return false;
	j->p++;
	return true;
}

static inline bool aj_peek(struct audio_json *j, char c) {
	aj_ws(j);
	return j->p < j->end && *j->p == c;
}

static inline bool aj_word(struct audio_json *j, const char *word) {
	size_t n = strlen(word);
	aj_ws(j);
	if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return false;
	j->p += n;
	return true;
}

static inline int aj_hex4(const char *p) {
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
			c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) return -1;
		v = v * 16 + d;
	}
	return v;
}

/* The length of the valid UTF-8 character at s, or 0. */
static inline int aj_utf8_len(const unsigned char *s, const unsigned char *end) {
	unsigned c = s[0];
	int n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
	if (!n || end - s < n) return 0;
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80) return 0;
	}
	if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] > 0x9f) ||
	    (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] > 0x8f)) return 0;
	return n;
}

static inline int aj_utf8_put(unsigned cp, char *b) {
	if (cp < 0x80) {
		b[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		b[0] = (char)(0xc0 | cp >> 6);
		b[1] = (char)(0x80 | (cp & 63));
		return 2;
	}
	if (cp < 0x10000) {
		b[0] = (char)(0xe0 | cp >> 12);
		b[1] = (char)(0x80 | (cp >> 6 & 63));
		b[2] = (char)(0x80 | (cp & 63));
		return 3;
	}
	b[0] = (char)(0xf0 | cp >> 18);
	b[1] = (char)(0x80 | (cp >> 12 & 63));
	b[2] = (char)(0x80 | (cp >> 6 & 63));
	b[3] = (char)(0x80 | (cp & 63));
	return 4;
}

/* A string into out (NUL-terminated, whole characters only), or skipped if out is
 * NULL. *cut says it didn't fit. Control characters become spaces and broken
 * surrogates U+FFFD, since this ends up drawn on the screen. */
static inline bool aj_string(struct audio_json *j, char *out, size_t cap, bool *cut) {
	if (!aj_eat(j, '"')) return false;
	size_t at = 0;
	bool full = false;
	while (j->p < j->end && *j->p != '"') {
		char b[4];
		int n;
		unsigned char c = (unsigned char)*j->p;
		if (c < 0x20) return false;
		if (c == '\\') {
			if (j->end - j->p < 2) return false;
			char e = j->p[1];
			j->p += 2;
			if (e == 'u') {
				if (j->end - j->p < 4) return false;
				int u = aj_hex4(j->p);
				if (u < 0) return false;
				j->p += 4;
				unsigned cp = (unsigned)u;
				if (cp >= 0xd800 && cp <= 0xdbff && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
					int low = aj_hex4(j->p + 2);
					if (low >= 0xdc00 && low <= 0xdfff) {
						cp = 0x10000 + ((cp - 0xd800) << 10) + ((unsigned)low - 0xdc00);
						j->p += 6;
					}
				}
				if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
				if (cp < 0x20) cp = ' ';
				n = aj_utf8_put(cp, b);
			} else {
				const char *from = "\"\\/bfnrt", *to = "\"\\/     ";
				const char *hit = e ? strchr(from, e) : NULL;
				if (!hit) return false;
				b[0] = to[hit - from];
				n = 1;
			}
		} else {
			n = aj_utf8_len((const unsigned char *)j->p, (const unsigned char *)j->end);
			if (!n) return false;
			memcpy(b, j->p, (size_t)n);
			j->p += n;
		}
		if (out && !full) {
			if (at + (size_t)n < cap) {
				memcpy(out + at, b, (size_t)n);
				at += (size_t)n;
			} else {
				full = true;
			}
		}
	}
	if (j->p >= j->end) return false;	/* cut short */
	j->p++;
	if (out) out[at] = 0;
	if (cut) *cut = full;
	return true;
}

/* A JSON number. *value is it rounded to a whole number and held at +-1000000,
 * or AJ_NOT_WHOLE for an exponent, which this daemon never writes. */
#define AJ_NOT_WHOLE (-2000000)
static inline bool aj_number(struct audio_json *j, int *value) {
	aj_ws(j);
	const char *p = j->p, *end = j->end;
	bool negative = p < end && *p == '-';
	if (negative) p++;
	if (p >= end || *p < '0' || *p > '9') return false;
	bool leading_zero = *p == '0';
	long v = 0;
	const char *first = p;
	for (; p < end && *p >= '0' && *p <= '9'; p++) {
		if (v < 1000000) v = v * 10 + (*p - '0');
	}
	if (leading_zero && p - first > 1) return false;
	if (p < end && *p == '.') {
		p++;
		if (p >= end || *p < '0' || *p > '9') return false;
		if (*p >= '5') v++;
		while (p < end && *p >= '0' && *p <= '9') p++;
	}
	bool exponent = false;
	if (p < end && (*p == 'e' || *p == 'E')) {
		exponent = true;
		p++;
		if (p < end && (*p == '+' || *p == '-')) p++;
		if (p >= end || *p < '0' || *p > '9') return false;
		while (p < end && *p >= '0' && *p <= '9') p++;
	}
	j->p = p;
	if (v > 1000000) v = 1000000;
	if (value) *value = exponent ? AJ_NOT_WHOLE : (int)(negative ? -v : v);
	return true;
}

/* Any value: checked, and passed over. */
static inline bool aj_skip(struct audio_json *j, int depth) {
	if (depth <= 0) return false;
	aj_ws(j);
	if (j->p >= j->end) return false;
	char c = *j->p;
	if (c == '"') return aj_string(j, NULL, 0, NULL);
	if (c == '{' || c == '[') {
		char close = c == '{' ? '}' : ']';
		j->p++;
		if (aj_eat(j, close)) return true;
		do {
			if (c == '{' && (!aj_string(j, NULL, 0, NULL) || !aj_eat(j, ':'))) return false;
			if (!aj_skip(j, depth - 1)) return false;
		} while (aj_eat(j, ','));
		return aj_eat(j, close);
	}
	if (c == 't') return aj_word(j, "true");
	if (c == 'f') return aj_word(j, "false");
	if (c == 'n') return aj_word(j, "null");
	return aj_number(j, NULL);
}

/* A value that should be a string, into out; one of another type leaves it empty. */
static inline bool aj_text_field(struct audio_json *j, char *out, size_t cap, bool *cut) {
	out[0] = 0;
	*cut = false;
	return aj_peek(j, '"') ? aj_string(j, out, cap, cut) : aj_skip(j, AUDIO_JSON_DEPTH);
}

static inline bool aj_bool_field(struct audio_json *j, bool *out) {
	*out = false;
	if (aj_word(j, "true")) {
		*out = true;
		return true;
	}
	return aj_skip(j, AUDIO_JSON_DEPTH);
}

static inline bool aj_percent_field(struct audio_json *j, int *out) {
	*out = -1;	/* null, or anything but a number: not known yet */
	aj_ws(j);
	if (j->p >= j->end || (*j->p != '-' && (*j->p < '0' || *j->p > '9'))) return aj_skip(j, AUDIO_JSON_DEPTH);
	int v = AJ_NOT_WHOLE;
	if (!aj_number(j, &v)) return false;
	if (v != AJ_NOT_WHOLE) *out = v < 0 ? 0 : v > 100 ? 100 : v;
	return true;
}

/* One of "outputs". *usable is false without a whole name, because an output is
 * addressed by its name and a shortened one would be some other output, or none. */
static inline bool aj_output(struct audio_json *j, struct audio_output *o, bool *usable) {
	memset(o, 0, sizeof(*o));
	o->percent = -1;
	*usable = false;
	if (!aj_eat(j, '{')) return false;
	if (aj_eat(j, '}')) return true;
	bool name_cut = false, cut;
	do {
		char key[16];
		if (!aj_string(j, key, sizeof(key), &cut) || !aj_eat(j, ':')) return false;
		bool ok;
		if (cut) ok = aj_skip(j, AUDIO_JSON_DEPTH);
		else if (!strcmp(key, "name")) ok = aj_text_field(j, o->name, sizeof(o->name), &name_cut);
		else if (!strcmp(key, "desc")) ok = aj_text_field(j, o->desc, sizeof(o->desc), &cut);
		else if (!strcmp(key, "kind")) ok = aj_text_field(j, o->kind, sizeof(o->kind), &cut);
		else if (!strcmp(key, "detail")) ok = aj_text_field(j, o->detail, sizeof(o->detail), &cut);
		else if (!strcmp(key, "mute")) ok = aj_bool_field(j, &o->mute);
		else if (!strcmp(key, "current")) ok = aj_bool_field(j, &o->current);
		else if (!strcmp(key, "percent")) ok = aj_percent_field(j, &o->percent);
		else ok = aj_skip(j, AUDIO_JSON_DEPTH);
		if (!ok) return false;
	} while (aj_eat(j, ','));
	if (!aj_eat(j, '}')) return false;
	*usable = o->name[0] && !name_cut;
	return true;
}

/* The state file's text: st is replaced only if all of it is good. */
static inline bool audio_state_parse(const char *text, size_t len, struct audio_state *st) {
	struct audio_json j = { text, text + len };
	struct audio_state fresh;
	bool saw_outputs = false;
	fresh.n = 0;
	if (len > AUDIO_STATE_MAX_BYTES || !aj_eat(&j, '{')) return false;
	do {
		char key[16];
		bool cut;
		if (!aj_string(&j, key, sizeof(key), &cut) || !aj_eat(&j, ':')) return false;
		if (!cut && !strcmp(key, "version")) {
			int v = 0;
			if (!aj_number(&j, &v) || v != 1) return false;	/* a later format isn't this one */
		} else if (!cut && !strcmp(key, "outputs")) {
			saw_outputs = true;
			fresh.n = 0;
			if (!aj_eat(&j, '[')) return false;
			if (aj_eat(&j, ']')) continue;
			do {
				struct audio_output o;
				bool usable;
				if (!aj_output(&j, &o, &usable)) return false;
				if (!usable || fresh.n == AUDIO_MAX_OUTPUTS) continue;
				for (int i = 0; i < fresh.n; i++) {
					if (fresh.out[i].current) o.current = false;	/* only one is */
				}
				fresh.out[fresh.n++] = o;
			} while (aj_eat(&j, ','));
			if (!aj_eat(&j, ']')) return false;
		} else if (!aj_skip(&j, AUDIO_JSON_DEPTH)) {
			return false;
		}
	} while (aj_eat(&j, ','));
	if (!aj_eat(&j, '}')) return false;
	aj_ws(&j);
	if (j.p != j.end || !saw_outputs) return false;
	*st = fresh;
	return true;
}

/* ---- the outputs --------------------------------------------------------- */
static inline int audio_find(const struct audio_state *st, const char *name) {
	for (int i = 0; i < st->n; i++) {
		if (strcmp(st->out[i].name, name) == 0) return i;
	}
	return -1;
}

static inline int audio_current(const struct audio_state *st) {
	for (int i = 0; i < st->n; i++) {
		if (st->out[i].current) return i;
	}
	return -1;
}

/* The one the modal shows selected: the output on its way in, else the current. */
static inline int audio_selected(const struct audio_state *st, const char *pending) {
	int i = pending[0] ? audio_find(st, pending) : -1;
	return i >= 0 ? i : audio_current(st);
}

/* A swap is over when its output is the current one, or has gone. */
static inline bool audio_pending_over(const struct audio_state *st, const char *pending) {
	int i = audio_find(st, pending);
	return i < 0 || st->out[i].current;
}

/* The tile's label: the action centre's are in capitals. */
static inline void audio_label_caps(char *out, size_t cap, const char *label) {
	size_t i = 0;
	for (; label[i] && i + 1 < cap; i++) out[i] = (char)toupper((unsigned char)label[i]);
	out[i] = 0;
}

/* The daemon's commands, which split words as a shell does. Node names have no
 * quotes in them; one that does isn't sent, as with the banner's "change". */
static inline bool audio_name_quotable(const char *name) {
	return name[0] && !strpbrk(name, "'\n");
}

static inline bool audio_cmd_output(char *line, size_t cap, const char *name) {
	if (!audio_name_quotable(name)) return false;
	int n = snprintf(line, cap, "output '%s'", name);
	return n > 0 && (size_t)n < cap;
}

static inline bool audio_cmd_volume(char *line, size_t cap, int percent, const char *name) {
	if (!audio_name_quotable(name)) return false;
	int n = snprintf(line, cap, "volume %d '%s'", percent, name);
	return n > 0 && (size_t)n < cap;
}

/* ---- the slider ---------------------------------------------------------- */
struct audio_slider {
	bool dragging;
	int percent, sent;	/* where it is, and what the daemon was last told; -1 nothing yet */
	uint32_t sent_ms;	/* now_ms() of that; 0 none */
	char name[sizeof(((struct audio_output *)0)->name)];	/* always by name: a Bluetooth node's id changes */
};

static inline int audio_slider_percent(double x, double left, double width) {
	if (width <= 0) return 0;
	double p = (x - left) * 100.0 / width;
	return p <= 0 ? 0 : p >= 100 ? 100 : (int)(p + 0.5);
}

static inline void audio_slider_begin(struct audio_slider *s, const char *name, int percent) {
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->dragging = true;
	s->percent = percent;
	s->sent = -1;
	s->sent_ms = 0;
}

/* -1: nothing to send. 0: send now. Otherwise, the ms until it may be sent. */
static inline int audio_slider_due(const struct audio_slider *s, uint32_t now) {
	if (s->percent == s->sent) return -1;
	if (!s->sent_ms) return 0;
	uint32_t since = now - s->sent_ms;
	return since >= AUDIO_SLIDER_GAP_MS ? 0 : (int)(AUDIO_SLIDER_GAP_MS - since);
}

static inline void audio_slider_mark_sent(struct audio_slider *s, uint32_t now) {
	s->sent = s->percent;
	s->sent_ms = now ? now : 1;
}

/* While it is held, and for a moment after, the device's reports are behind it. */
static inline bool audio_slider_quiet(const struct audio_slider *s, uint32_t now) {
	return s->dragging || audio_sync_quiet(now, s->sent_ms);
}

/* A state as read, with the slider's output where the slider says. */
static inline void audio_state_hold(struct audio_state *st, const struct audio_slider *s, uint32_t now) {
	if (!s->name[0] || !audio_slider_quiet(s, now)) return;
	int i = audio_find(st, s->name);
	if (i < 0) return;
	st->out[i].percent = s->percent;
	if (s->percent > 0) st->out[i].mute = false;	/* the daemon unmutes along with it */
}
#endif
