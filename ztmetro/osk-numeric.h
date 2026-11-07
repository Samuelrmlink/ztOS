/* The on-screen keyboard's number pads: which one a field wants, and where its
 * keys go. No wlroots or drawing here, so that tests/osk-numeric.c can check
 * it on any machine; ztmetro.c turns these keys into its own osk_key table.
 *
 * All pads are four rows of the user's key height, the same as the letters, so
 * going between them does not move what is kept clear of the keyboard. */
#ifndef ZTOS_OSK_NUMERIC_H
#define ZTOS_OSK_NUMERIC_H

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum osk_mode {
	OSK_MODE_FULL,		/* the letters, and the pages behind them */
	OSK_MODE_DIGITS,	/* 0-9 and nothing else: PINs */
	OSK_MODE_NUMBER,	/* 0-9, decimal point, minus */
	OSK_MODE_PHONE,		/* 0-9, * # + */
};

/* zwp_text_input_v3 content_purpose values that matter here. They are the
 * protocol's wire values, which cannot change. wlroots does not export them. */
#define OSK_PURPOSE_DIGITS	2
#define OSK_PURPOSE_NUMBER	3
#define OSK_PURPOSE_PHONE	4
#define OSK_PURPOSE_PIN		9

/* A field's text-input purpose. Everything else, passwords and dates included,
 * has the full keyboard. Nothing is masked by the keyboard for any of them. */
static inline enum osk_mode osk_mode_for_purpose(uint32_t purpose) {
	switch (purpose) {
	case OSK_PURPOSE_DIGITS:
	case OSK_PURPOSE_PIN:
		return OSK_MODE_DIGITS;
	case OSK_PURPOSE_NUMBER:
		return OSK_MODE_NUMBER;
	case OSK_PURPOSE_PHONE:
		return OSK_MODE_PHONE;
	default:
		return OSK_MODE_FULL;
	}
}

/* The names used down the control pipe ("osk numeric digits"), and in the log. */
static inline const char *osk_mode_name(enum osk_mode mode) {
	switch (mode) {
	case OSK_MODE_DIGITS: return "digits";
	case OSK_MODE_NUMBER: return "number";
	case OSK_MODE_PHONE: return "phone";
	default: return "full";
	}
}

static inline enum osk_mode osk_mode_from_name(const char *name) {
	if (name && !strcmp(name, "digits")) return OSK_MODE_DIGITS;
	if (name && !strcmp(name, "number")) return OSK_MODE_NUMBER;
	if (name && !strcmp(name, "phone")) return OSK_MODE_PHONE;
	return OSK_MODE_FULL;
}

/* Which keyboard is wanted. A field of the shell's own (the search box, a
 * password prompt) takes the keys whatever the app behind it has focused, so it
 * gets what the shell asked for, which is the full keyboard unless it asked
 * otherwise. Then the app: its text-input field, then what it asked for down
 * the control pipe. */
static inline enum osk_mode osk_mode_choose(bool shell_field, enum osk_mode shell,
		enum osk_mode field, enum osk_mode app) {
	if (shell_field || shell != OSK_MODE_FULL) return shell;
	return field != OSK_MODE_FULL ? field : app;
}

/* A digits-only field has no use for letters, so its pad has no way to them. */
static inline enum osk_mode osk_mode_shown(enum osk_mode wanted, bool letters) {
	return letters && wanted != OSK_MODE_DIGITS ? OSK_MODE_FULL : wanted;
}

#define OSK_PAD_ROWS 4

struct osk_pad_key {
	const char *label;	/* NULL: draw the icon */
	const char *icon;
	uint32_t keycode;
	bool shift;		/* needs shift on a US layout */
	bool page;		/* no key press: goes to the letters */
	int col, row, cols, rows;
};

#define PAD_K(label, code, c, r) { label, NULL, code, false, false, c, r, 1, 1 }
#define PAD_S(label, code, c, r) { label, NULL, code, true, false, c, r, 1, 1 }
#define PAD_DIGITS \
	PAD_K("1", KEY_1, 0, 0), PAD_K("2", KEY_2, 1, 0), PAD_K("3", KEY_3, 2, 0), \
	PAD_K("4", KEY_4, 0, 1), PAD_K("5", KEY_5, 1, 1), PAD_K("6", KEY_6, 2, 1), \
	PAD_K("7", KEY_7, 0, 2), PAD_K("8", KEY_8, 1, 2), PAD_K("9", KEY_9, 2, 2), \
	PAD_K("0", KEY_0, 1, 3)
#define PAD_BACKSPACE(c, r) { NULL, "backspace", KEY_BACKSPACE, false, false, c, r, 1, 1 }
#define PAD_ENTER(c, r, n) { NULL, "enter", KEY_ENTER, false, false, c, r, 1, n }
#define PAD_LETTERS(c, r) { "abc", NULL, 0, false, true, c, r, 1, 1 }

/* The keys of a pad, and how many columns it has; NULL for the full keyboard. */
static inline const struct osk_pad_key *osk_pad_keys(enum osk_mode mode, int *n, int *columns) {
	static const struct osk_pad_key digits[] = {
		PAD_DIGITS, PAD_BACKSPACE(0, 3), PAD_ENTER(2, 3, 1),
	};
	static const struct osk_pad_key number[] = {
		PAD_DIGITS, PAD_K("-", KEY_MINUS, 0, 3), PAD_K(".", KEY_DOT, 2, 3),
		PAD_BACKSPACE(3, 0), PAD_LETTERS(3, 1), PAD_ENTER(3, 2, 2),
	};
	static const struct osk_pad_key phone[] = {
		PAD_DIGITS, PAD_S("*", KEY_8, 0, 3), PAD_S("#", KEY_3, 2, 3),
		PAD_BACKSPACE(3, 0), PAD_S("+", KEY_EQUAL, 3, 1), PAD_LETTERS(3, 2), PAD_ENTER(3, 3, 1),
	};
	switch (mode) {
	case OSK_MODE_DIGITS:
		*n = sizeof(digits) / sizeof(digits[0]);
		*columns = 3;
		return digits;
	case OSK_MODE_NUMBER:
		*n = sizeof(number) / sizeof(number[0]);
		*columns = 4;
		return number;
	case OSK_MODE_PHONE:
		*n = sizeof(phone) / sizeof(phone[0]);
		*columns = 4;
		return phone;
	default:
		*n = *columns = 0;
		return NULL;
	}
}

/* Where a key goes, with the gaps and rows of the rest of the keyboard: `top`
 * is where the first row starts. */
static inline void osk_pad_place(const struct osk_pad_key *key, int columns, double width,
		double key_height, double gap, double top, double *x, double *y, double *w, double *h) {
	double unit = (width - (columns + 1) * gap) / columns;
	*x = gap + key->col * (unit + gap);
	*y = top + key->row * key_height;
	*w = unit * key->cols + gap * (key->cols - 1);
	*h = key_height * key->rows - gap;
}

#endif
