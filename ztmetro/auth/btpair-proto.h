/* What ztos-bt-agent says to the shell, checked without a compositor, so the
 * host tests can run it (tests/bt-pair-proto.c).
 *
 * Over the control pipe, one line each (ctl_buf is 256 bytes):
 *     bt-pair ID|MODE|SECONDS|ADDRESS|VALUE|NAME
 *     bt-pair-update ID|ENTERED
 *     bt-pair-close ID
 * Back over $XDG_RUNTIME_DIR/ztos-bt-agent.sock, on a connection of its own:
 *     ID\tOK\tVALUE\n   or   ID\tCANCEL\n
 * or, for a PIN or passkey with "Use this ... for similar devices" ticked:
 *     ID\tOK\tVALUE\tremember\n
 *
 * A device's name comes over the air and its owner chooses it: it is cleaned
 * here too, even though the agent has already cleaned it. */
#ifndef ZTOS_BTPAIR_PROTO_H
#define ZTOS_BTPAIR_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum bt_pair_mode {
	BT_PAIR_NONE,
	BT_PAIR_PIN,		/* type a PIN: 1 to 16 letters and digits */
	BT_PAIR_PASSKEY,	/* type a passkey: 6 digits */
	BT_PAIR_SHOW_PIN,	/* show it, for typing on the other device */
	BT_PAIR_SHOW_PASSKEY,
	BT_PAIR_CONFIRM,	/* does the other device show this number? */
	BT_PAIR_AUTHORIZE,	/* pair with no code at all? */
	BT_PAIR_SERVICE,	/* may a paired device use a service? */
};

#define BT_PAIR_PIN_MAX 16
#define BT_PAIR_PASSKEY_LEN 6

struct bt_pair_request {
	char id[12];
	enum bt_pair_mode mode;
	int seconds;
	char addr[18];
	char value[33];		/* the number shown, or the service's name */
	char name[97];
};

static inline bool bt_pair_is_digit(char c) { return c >= '0' && c <= '9'; }
static inline bool bt_pair_is_alnum(char c) {
	return bt_pair_is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool bt_pair_all(const char *s, bool (*ok)(char)) {
	if (!*s) return false;
	for (; *s; s++) if (!ok(*s)) return false;
	return true;
}

/* How many bytes the character at s has, or 0 if it is not valid UTF-8 (overlong
 * forms, surrogates and past U+10FFFF included). */
static inline size_t bt_pair_utf8_len(const unsigned char *s) {
	if (s[0] < 0x80) return 1;
	if (s[0] < 0xc2) return 0;
	if (s[0] < 0xe0) return (s[1] & 0xc0) == 0x80 ? 2 : 0;
	if (s[0] < 0xf0) {
		if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) return 0;
		if (s[0] == 0xe0 && s[1] < 0xa0) return 0;
		if (s[0] == 0xed && s[1] >= 0xa0) return 0;
		return 3;
	}
	if (s[0] < 0xf5) {
		if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return 0;
		if (s[0] == 0xf0 && s[1] < 0x90) return 0;
		if (s[0] == 0xf4 && s[1] >= 0x90) return 0;
		return 4;
	}
	return 0;
}

/* Text that can pretend to be something else on the screen, or breaks the line
 * it is drawn in: C0, DEL and C1 controls, zero-width and bidirectional
 * marks, line and paragraph separators. */
static inline bool bt_pair_hidden_char(const unsigned char *s, size_t n) {
	if (n == 1) return s[0] < 0x20 || s[0] == 0x7f;
	if (n == 2) return s[0] == 0xc2 && s[1] < 0xa0;
	if (n == 3 && s[0] == 0xe2 && s[1] == 0x80) return (s[2] >= 0x8b && s[2] <= 0x8f) || (s[2] >= 0xa8 && s[2] <= 0xae);
	if (n == 3 && s[0] == 0xe2 && s[1] == 0x81) return s[2] >= 0xa0 && s[2] <= 0xaf;
	if (n == 3 && s[0] == 0xef && s[1] == 0xbb) return s[2] == 0xbf;
	return false;
}

/* src into dst, whole characters only: nothing invalid or hidden, and no field
 * separator. */
static inline void bt_pair_clean(char *dst, size_t size, const char *src) {
	size_t used = 0;
	const unsigned char *s = (const unsigned char *)src;
	while (*s) {
		size_t n = bt_pair_utf8_len(s);
		if (!n) { s++; continue; }
		if (!bt_pair_hidden_char(s, n)) {
			if (used + n >= size) break;
			if (*s == '|') dst[used++] = '/';
			else { memcpy(dst + used, s, n); used += n; }
		}
		s += n;
	}
	dst[used] = 0;
}

static inline enum bt_pair_mode bt_pair_mode_from(const char *s) {
	static const struct { const char *name; enum bt_pair_mode mode; } modes[] = {
		{ "pin", BT_PAIR_PIN }, { "passkey", BT_PAIR_PASSKEY },
		{ "show-pin", BT_PAIR_SHOW_PIN }, { "show-passkey", BT_PAIR_SHOW_PASSKEY },
		{ "confirm", BT_PAIR_CONFIRM }, { "authorize", BT_PAIR_AUTHORIZE },
		{ "service", BT_PAIR_SERVICE },
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (!strcmp(s, modes[i].name)) return modes[i].mode;
	return BT_PAIR_NONE;
}

static inline bool bt_pair_addr_ok(const char *s) {
	if (!*s) return true;	/* not known */
	if (strlen(s) != 17) return false;
	for (int i = 0; i < 17; i++) {
		char c = s[i];
		bool hex = bt_pair_is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
		if (i % 3 == 2 ? c != ':' : !hex) return false;
	}
	return true;
}

/* Whether this mode's VALUE is what it should be. */
static inline bool bt_pair_value_ok(enum bt_pair_mode mode, const char *v) {
	switch (mode) {
	case BT_PAIR_SHOW_PASSKEY:
	case BT_PAIR_CONFIRM:
		if (strlen(v) != BT_PAIR_PASSKEY_LEN) return false;
		for (; *v; v++) if (!bt_pair_is_digit(*v)) return false;
		return true;
	case BT_PAIR_SHOW_PIN:
		if (!*v || strlen(v) > BT_PAIR_PIN_MAX) return false;
		for (; *v; v++) if (!bt_pair_is_alnum(*v)) return false;
		return true;
	case BT_PAIR_SERVICE:
		return strlen(v) < 33;	/* a label, cleaned like a name */
	default:
		return !*v;
	}
}

/* Takes the line apart (what follows "bt-pair "), which it writes over. */
static inline bool bt_pair_parse(char *line, struct bt_pair_request *r) {
	char *f[6] = { line };
	for (int i = 1; i < 6; i++) {
		char *bar = strchr(f[i - 1], '|');
		if (!bar) return false;
		*bar = 0;
		f[i] = bar + 1;	/* the last field, the name, may hold more bars: cleaned below */
	}
	if (!*f[0] || strlen(f[0]) >= sizeof(r->id)) return false;
	for (char *c = f[0]; *c; c++) if (!bt_pair_is_alnum(*c)) return false;
	enum bt_pair_mode mode = bt_pair_mode_from(f[1]);
	if (mode == BT_PAIR_NONE) return false;
	char *end;
	long seconds = strtol(f[2], &end, 10);
	if (*end || end == f[2] || seconds < 1 || seconds > 300) return false;
	if (!bt_pair_addr_ok(f[3])) return false;
	char value[33];
	if (strlen(f[4]) >= sizeof(value)) return false;
	bt_pair_clean(value, sizeof(value), f[4]);
	if (!bt_pair_value_ok(mode, value)) return false;
	memset(r, 0, sizeof(*r));
	snprintf(r->id, sizeof(r->id), "%s", f[0]);
	r->mode = mode;
	r->seconds = (int)seconds;
	snprintf(r->addr, sizeof(r->addr), "%s", f[3]);
	snprintf(r->value, sizeof(r->value), "%s", value);
	bt_pair_clean(r->name, sizeof(r->name), f[5]);
	if (!r->name[0]) snprintf(r->name, sizeof(r->name), "%s", r->addr[0] ? r->addr : "A device");
	return true;
}

/* May this text go on the end of what has been typed? One character at a time,
 * of the alphabet the mode takes. The numeric keyboard has only digits, but a
 * real keyboard may be plugged in. */
static inline bool bt_pair_entry_takes(enum bt_pair_mode mode, size_t len, const char *text) {
	if (!text || !text[0] || text[1]) return false;
	if (mode == BT_PAIR_PIN) return len < BT_PAIR_PIN_MAX && bt_pair_is_alnum(text[0]);
	if (mode == BT_PAIR_PASSKEY) return len < BT_PAIR_PASSKEY_LEN && bt_pair_is_digit(text[0]);
	return false;
}

/* May what has been typed be sent? */
static inline bool bt_pair_entry_done(enum bt_pair_mode mode, size_t len) {
	return mode == BT_PAIR_PIN ? len >= 1 && len <= BT_PAIR_PIN_MAX :
		mode == BT_PAIR_PASSKEY ? len == BT_PAIR_PASSKEY_LEN : false;
}

/* The line back: its length, or -1 if it does not fit. */
static inline int bt_pair_reply_line(char *out, size_t size, const char *id, bool ok, const char *value,
		bool remember) {
	int n = ok ? snprintf(out, size, "%s\tOK\t%s%s\n", id, value ? value : "", remember ? "\tremember" : "") :
		snprintf(out, size, "%s\tCANCEL\n", id);
	return n < 0 || (size_t)n >= size ? -1 : n;
}
#endif
