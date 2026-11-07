/* make check-airplane-radios */
#include <assert.h>
#include <stdio.h>
#include "../airplane-radios.h"

int main(void) {
	struct airplane_radios a = { 0 };

	/* off by default: the modem's radio only */
	assert(airplane_radios_step(&a, true, true, true) == 0);
	assert(airplane_radios_step(&a, false, true, true) == 0);

	a.option = true;
	/* both on: both off, both back */
	assert(airplane_radios_step(&a, true, true, true) == (AIRPLANE_WIFI_OFF | AIRPLANE_BT_OFF));
	assert(airplane_radios_step(&a, false, false, false) == (AIRPLANE_WIFI_ON | AIRPLANE_BT_ON));

	/* Bluetooth already off: stays off */
	assert(airplane_radios_step(&a, true, true, false) == AIRPLANE_WIFI_OFF);
	assert(airplane_radios_step(&a, false, false, false) == AIRPLANE_WIFI_ON);

	/* Wi-Fi switched back on by hand during the flight: not switched again */
	assert(airplane_radios_step(&a, true, true, true) == (AIRPLANE_WIFI_OFF | AIRPLANE_BT_OFF));
	assert(airplane_radios_step(&a, false, true, false) == AIRPLANE_BT_ON);

	/* on by hand, then off by hand: the tile says it's theirs now */
	assert(airplane_radios_step(&a, true, true, false) == AIRPLANE_WIFI_OFF);
	a.took_wifi = false;
	assert(airplane_radios_step(&a, false, false, false) == 0);

	/* the option turned off during the flight: what was taken still goes back */
	assert(airplane_radios_step(&a, true, true, true) == (AIRPLANE_WIFI_OFF | AIRPLANE_BT_OFF));
	a.option = false;
	assert(airplane_radios_step(&a, false, false, false) == (AIRPLANE_WIFI_ON | AIRPLANE_BT_ON));

	puts("PASS: airplane mode and the other radios");
	return 0;
}
