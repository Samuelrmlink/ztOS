/* make check-modem-sim: no SIM reads as no SIM, and no network as no signal. */
#define main modemd_main
#include "../modemd/ztoscorenetworkingd.c"
#undef main
#include <assert.h>

int main(void) {
	assert(!strcmp(sim_from_cpin("READY"), "ready"));
	assert(!strcmp(sim_from_cpin("SIM PIN"), "pin"));
	assert(!strcmp(sim_from_cpin("SIM PUK"), "puk"));
	assert(!strcmp(sim_from_cpin("SIM PIN2"), "pin"));
	assert(!strcmp(sim_from_cpin("NOT INSERTED"), "absent"));
	assert(!strcmp(sim_from_cpin("SIM NOT INSERTED"), "absent"));
	assert(!strcmp(sim_from_cpin("ABSENT"), "absent"));
	assert(!strcmp(sim_from_cpin("SOMETHING NEW"), "error"));

	assert(!strcmp(sim_from_cme("+CME ERROR: SIM not inserted"), "absent"));
	assert(!strcmp(sim_from_cme("+CME ERROR: 10"), "absent"));
	assert(!strcmp(sim_from_cme("+CME ERROR: SIM failure"), "error"));
	assert(!sim_from_cme("+CME ERROR: SIM busy"));
	assert(!sim_from_cme("+CME ERROR: 14"));
	assert(!sim_from_cme("+CME ERROR: unknown"));

	assert(status_bars(5, 0) == 0);		/* the phone's report with no SIM */
	assert(status_bars(4, 2) == 0);		/* searching */
	assert(status_bars(4, 3) == 0);		/* refused */
	assert(status_bars(-1, 0) == 0);
	assert(status_bars(4, 1) == 4);
	assert(status_bars(2, 5) == 2);
	assert(status_bars(0, 1) == 3);		/* registered with nothing measured */
	assert(status_bars(-1, 5) == 3);
	puts("PASS: SIM state and signal");
	return 0;
}
