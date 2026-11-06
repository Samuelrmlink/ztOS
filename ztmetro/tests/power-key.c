#include <assert.h>
#include <stdio.h>
#include "../power/power-key.h"

int main(void) {
	struct power_key k = {0};

	/* screen on, short press: sleeps on release, not before */
	assert(power_key_pressed(&k, true) == POWER_KEY_NOTHING);
	assert(power_key_released(&k, false) == POWER_KEY_SLEEP);

	/* screen off, short press: wakes on press, release does nothing */
	assert(power_key_pressed(&k, false) == POWER_KEY_WAKE);
	assert(power_key_released(&k, false) == POWER_KEY_NOTHING);

	/* screen on, hold: menu once, and the release doesn't sleep or close it */
	assert(power_key_pressed(&k, true) == POWER_KEY_NOTHING);
	assert(power_key_hold_elapsed(&k) == POWER_KEY_OPEN_MENU);
	assert(power_key_hold_elapsed(&k) == POWER_KEY_NOTHING);
	assert(power_key_released(&k, true) == POWER_KEY_NOTHING);

	/* screen off, hold: wakes, then the menu */
	assert(power_key_pressed(&k, false) == POWER_KEY_WAKE);
	assert(power_key_hold_elapsed(&k) == POWER_KEY_OPEN_MENU);
	assert(power_key_released(&k, true) == POWER_KEY_NOTHING);

	/* menu open, short press: closes it and leaves the screen on */
	assert(power_key_pressed(&k, true) == POWER_KEY_NOTHING);
	assert(power_key_released(&k, true) == POWER_KEY_CLOSE_MENU);

	/* a timer that fires after the release, or a stray release, does nothing */
	assert(power_key_hold_elapsed(&k) == POWER_KEY_NOTHING);
	assert(power_key_released(&k, false) == POWER_KEY_NOTHING);

	puts("PASS: power key short press, hold, wake and menu states");
	return 0;
}
