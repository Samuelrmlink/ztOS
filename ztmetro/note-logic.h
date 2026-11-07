/* The action centre's notifications: what a sideways swipe decides, and which
 * lines of the control pipe change a notification in place. No wlroots or
 * drawing here, so that tests/note-logic.c can check it on any machine.
 *
 * The distances are starting values, to be tuned by hand on the phone. */
#ifndef ZTOS_NOTE_LOGIC_H
#define ZTOS_NOTE_LOGIC_H

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_KEY_LEN		72
#define NOTE_MUTED_MAX		8

/* A drag this much more sideways than up or down is a swipe. Anything less is
 * left to the list and the panel, which were here first: a sloppy scroll that
 * leans a little must go on being a scroll. */
#define NOTE_SWIPE_RATIO	1.5
#define NOTE_SWIPE_PAST		0.35	/* of the width: let go here and it goes */
#define NOTE_SWIPE_THROW_V	0.5	/* points per ms: a throw counts from here */
#define NOTE_SWIPE_THROW_MIN	24.0	/* and has to have gone this far (points) */

/* Whether a drag that has just passed the slop becomes a swipe. `on_note`: it
 * went down on a notification. `panel_open`: the panel is all the way down.
 * `free_now`: no other finger is already swiping one. */
static inline bool note_swipe_wants(double dx, double dy, bool on_note, bool panel_open, bool free_now) {
	return on_note && panel_open && free_now && fabs(dx) > fabs(dy) * NOTE_SWIPE_RATIO;
}

enum note_swipe_end { NOTE_SWIPE_SETTLE, NOTE_SWIPE_FLY_LEFT, NOTE_SWIPE_FLY_RIGHT };

/* The finger let go at `x` (points from where the notification rests), going
 * at `vx`; a finger that had stopped first passes 0. A touch that was cancelled
 * never dismisses. Past the line it goes, unless thrown back the other way;
 * short of the line, only a throw outward takes it. */
static inline enum note_swipe_end note_swipe_release(double x, double vx, double width, bool cancelled) {
	if (cancelled || x == 0) {
		return NOTE_SWIPE_SETTLE;
	}
	bool past = fabs(x) >= width * NOTE_SWIPE_PAST;
	bool fast = fabs(vx) >= NOTE_SWIPE_THROW_V;
	bool outward = (vx > 0) == (x > 0);
	if (past ? (fast && !outward) : !(fast && outward && fabs(x) >= NOTE_SWIPE_THROW_MIN)) {
		return NOTE_SWIPE_SETTLE;
	}
	return x > 0 ? NOTE_SWIPE_FLY_RIGHT : NOTE_SWIPE_FLY_LEFT;
}

/* `dt` ms of moving `x` to `to`: quickly, slowing as it nears, and never less
 * than `min_v` points per ms so that it does arrive. Returns whether it has. */
static inline bool note_swipe_step(double *x, double to, double dt, double tau, double min_v) {
	double left = to - *x;
	double step = fabs(left) * (1.0 - exp(-dt / tau));
	if (step < min_v * dt) {
		step = min_v * dt;
	}
	if (step >= fabs(left)) {
		*x = to;
		return true;
	}
	*x += left > 0 ? step : -step;
	return false;
}

/* It fades as it goes, and is still faintly there at the edge. */
static inline float note_swipe_alpha(double x, double width) {
	double t = width > 0 ? fabs(x) / width : 0;
	return (float)(t >= 1 ? 0.3 : 1.0 - 0.7 * t);
}

/* ---- notifications that change in place ------------------------------------
 *
 *	notekey KEY|FLAGS|PERCENT|WHO|APP|TITLE|TEXT
 *	noteclose KEY
 *
 * KEY names the notification for as long as its sender keeps it. FLAGS is "q"
 * for an update that should not alert, or "-". PERCENT is 0..100 for a bar,
 * NOTE_PLAIN for an ordinary notification, NOTE_BUSY for one that belongs to
 * something under way but has no number to show just now. */
#define NOTE_PLAIN	(-1)
#define NOTE_BUSY	(-2)

struct note_line {
	char *key, *who, *app, *title, *text;
	bool quiet;
	int percent;
};

/* Takes `line` (what follows "notekey ") apart in place. Text may hold anything
 * but the line's end, so it comes last. Returns false for a line it can't read:
 * no key, too few fields, a key too long to keep, or a PERCENT that is not one. */
static inline bool note_line_parse(char *line, struct note_line *out) {
	char *f[7];
	f[0] = line;
	for (int i = 1; i < 7; i++) {
		char *bar = strchr(f[i - 1], '|');
		if (!bar) {
			return false;
		}
		*bar = 0;
		f[i] = bar + 1;
	}
	char *end;
	long percent = strtol(f[2], &end, 10);
	if (!f[0][0] || strlen(f[0]) >= NOTE_KEY_LEN || !f[2][0] || *end ||
			percent < NOTE_BUSY || percent > 100) {
		return false;
	}
	out->key = f[0];
	out->quiet = f[1][0] == 'q';
	out->percent = (int)percent;
	out->who = f[3];
	out->app = f[4];
	out->title = f[5];
	out->text = f[6];
	return true;
}

/* A download the user has swiped away should stay away: its next update comes
 * seconds later and must not bring it back. So its key is remembered until its
 * sender closes it, or says something that is news rather than progress. Only
 * a few are kept; forgetting the oldest at worst brings one back. */
struct note_muted {
	char key[NOTE_MUTED_MAX][NOTE_KEY_LEN];
	int n;
};

static inline bool note_muted_has(const struct note_muted *m, const char *key) {
	for (int i = 0; i < m->n; i++) {
		if (!strcmp(m->key[i], key)) {
			return true;
		}
	}
	return false;
}

static inline void note_muted_forget(struct note_muted *m, const char *key) {
	for (int i = 0; i < m->n; i++) {
		if (!strcmp(m->key[i], key)) {
			memmove(m->key[i], m->key[i + 1], sizeof(m->key[0]) * (size_t)(m->n - i - 1));
			m->n--;
			return;
		}
	}
}

static inline void note_muted_add(struct note_muted *m, const char *key) {
	if (!key[0] || strlen(key) >= NOTE_KEY_LEN || note_muted_has(m, key)) {
		return;
	}
	if (m->n == NOTE_MUTED_MAX) {
		memmove(m->key[0], m->key[1], sizeof(m->key[0]) * (NOTE_MUTED_MAX - 1));
		m->n--;
	}
	memcpy(m->key[m->n], key, strlen(key) + 1);
	m->n++;
}

/* Whether an update for `key` is dropped because the user dismissed it. An
 * ordinary notification is news: it gets through, and ends the muting. */
static inline bool note_muted_drops(struct note_muted *m, const char *key, int percent) {
	if (!note_muted_has(m, key)) {
		return false;
	}
	if (percent == NOTE_PLAIN) {
		note_muted_forget(m, key);
		return false;
	}
	return true;
}

#endif
