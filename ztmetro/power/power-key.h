/* The power key: a short press sleeps or wakes the screen, a hold opens the
 * power menu. The screen wakes on the press, so waking never waits for the
 * key to come up; it sleeps on the release, once it can't be a hold. */
#ifndef ZTOS_POWER_KEY_H
#define ZTOS_POWER_KEY_H

#include <stdbool.h>

#define POWER_HOLD_MS 800

enum power_key_action {
	POWER_KEY_NOTHING,
	POWER_KEY_WAKE,
	POWER_KEY_SLEEP,
	POWER_KEY_OPEN_MENU,
	POWER_KEY_CLOSE_MENU,
};

struct power_key {
	bool held;
	bool held_long;	/* the hold has already opened the menu */
	bool woke;	/* this press woke the screen, so its release does nothing */
};

static inline enum power_key_action power_key_pressed(struct power_key *k, bool screen_on) {
	k->held = true;
	k->held_long = false;
	k->woke = !screen_on;
	return k->woke ? POWER_KEY_WAKE : POWER_KEY_NOTHING;
}

static inline enum power_key_action power_key_hold_elapsed(struct power_key *k) {
	if (!k->held || k->held_long) return POWER_KEY_NOTHING;
	k->held_long = true;
	return POWER_KEY_OPEN_MENU;
}

static inline enum power_key_action power_key_released(struct power_key *k, bool menu_open) {
	bool was_held = k->held;
	k->held = false;
	if (!was_held || k->held_long || k->woke) return POWER_KEY_NOTHING;
	return menu_open ? POWER_KEY_CLOSE_MENU : POWER_KEY_SLEEP;
}

#endif
