// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What keeps the phone awake, as the lock screen shows it (ROADMAP 8.30):
 * the output of `zrn-sleep-why --shell` read into a struct. Plain C, no
 * wlroots or cairo, so tests/lock-awake.c checks it on the PC.
 *
 * The lines, tab-separated (devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleep-why):
 *
 *	line	Awake: Brave playing · 2 more	(empty: nothing to say)
 *	item	KIND	LABEL	DETAIL		each blocker, most important first
 *	woke	TEXT				the last hour's wakes from sleep
 *	window	TEXT				what the wakers are measured over
 *	waker	NAME	RATE			a busy process (--wakers)
 *	irq	NAME	RATE			a busy interrupt (--wakers)
 *
 * Unknown lines are skipped; long fields are cut at a character boundary.
 */
#ifndef ZT_LOCK_AWAKE_PARSE_H
#define ZT_LOCK_AWAKE_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define LA_ITEMS	12
#define LA_WAKERS	8

struct la_item {
	char kind[16];
	char label[80];
	char detail[160];
};

struct la_waker {
	char name[48];
	char rate[40];
	bool irq;
};

struct la_data {
	bool valid;		/* a reply came (else nothing is shown) */
	char line[128];
	int n_items;
	struct la_item items[LA_ITEMS];
	char woke[160];
	char window[64];
	bool wakers_known;	/* a --wakers reply: the busy list is real, even if empty */
	int n_wakers;
	struct la_waker wakers[LA_WAKERS];
};

/* Copy at most `n` bytes of `src` into `dst` (size `size`), cut back to a
 * UTF-8 character boundary, NUL-terminated. */
static inline void la_copy(char *dst, size_t size, const char *src, size_t n) {
	if (size == 0) return;
	if (n > size - 1) {
		n = size - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

/* The `i`th tab-separated field of the line [s, e) into dst; false if absent. */
static inline bool la_field(const char *s, const char *e, int i, char *dst, size_t size) {
	for (; i > 0; i--) {
		const char *tab = memchr(s, '\t', (size_t)(e - s));
		if (!tab) {
			if (size) dst[0] = 0;
			return false;
		}
		s = tab + 1;
	}
	const char *tab = memchr(s, '\t', (size_t)(e - s));
	la_copy(dst, size, s, (size_t)((tab ? tab : e) - s));
	return true;
}

static inline void la_parse(struct la_data *d, const char *text) {
	memset(d, 0, sizeof(*d));
	if (!text) return;
	d->valid = true;
	for (const char *s = text; *s;) {
		const char *e = strchr(s, '\n');
		if (!e) e = s + strlen(s);
		const char *next = *e ? e + 1 : e;
		const char *end = e > s && e[-1] == '\r' ? e - 1 : e;
		char tag[8];
		la_field(s, end, 0, tag, sizeof(tag));
		if (!strcmp(tag, "line")) {
			la_field(s, end, 1, d->line, sizeof(d->line));
		} else if (!strcmp(tag, "item") && d->n_items < LA_ITEMS) {
			struct la_item *it = &d->items[d->n_items];
			if (la_field(s, end, 1, it->kind, sizeof(it->kind)) &&
					la_field(s, end, 2, it->label, sizeof(it->label)) && it->label[0]) {
				la_field(s, end, 3, it->detail, sizeof(it->detail));
				d->n_items++;
			} else {
				memset(it, 0, sizeof(*it));
			}
		} else if (!strcmp(tag, "woke")) {
			la_field(s, end, 1, d->woke, sizeof(d->woke));
		} else if (!strcmp(tag, "window")) {
			la_field(s, end, 1, d->window, sizeof(d->window));
			d->wakers_known = true;
		} else if ((!strcmp(tag, "waker") || !strcmp(tag, "irq")) && d->n_wakers < LA_WAKERS) {
			struct la_waker *w = &d->wakers[d->n_wakers];
			if (la_field(s, end, 1, w->name, sizeof(w->name)) && w->name[0]) {
				la_field(s, end, 2, w->rate, sizeof(w->rate));
				w->irq = !strcmp(tag, "irq");
				d->n_wakers++;
			} else {
				memset(w, 0, sizeof(*w));
			}
		}
		s = next;
	}
}

/* Does the lock screen show the line? Only with something in it: the
 * reader leaves it empty when only the lock screen keeps the phone up
 * (unless asked --always). */
static inline bool la_line_shown(const struct la_data *d) {
	return d->valid && d->line[0];
}

#endif
