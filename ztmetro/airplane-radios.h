/* What the airplane tile does to Wi-Fi and Bluetooth when the "all radios" option
 * is on (Settings > cellular + SIM; off by default). No wlroots and nothing run
 * here, so tests/airplane-radios.c can check it on any machine.
 *
 * Going in, it turns off what is on and remembers that. Coming out, it turns back
 * on only those, so a radio that was already off stays off. Switching one by hand
 * in between clears its took_ flag (the shell's tiles do that): it's the person's
 * again, and coming out leaves it as they set it. The memory is the shell's only:
 * after a shell restart during airplane mode, the radios stay as they are. */
#ifndef ZTOS_AIRPLANE_RADIOS_H
#define ZTOS_AIRPLANE_RADIOS_H

#include <stdbool.h>

enum { AIRPLANE_WIFI_ON = 1, AIRPLANE_WIFI_OFF = 2, AIRPLANE_BT_ON = 4, AIRPLANE_BT_OFF = 8 };

struct airplane_radios {
	bool option;		/* the setting: airplane_radios = on */
	bool took_wifi, took_bt;	/* turned off by airplane mode */
};

/* The tile was just switched: which radios to switch now. */
static inline int airplane_radios_step(struct airplane_radios *a, bool airplane, bool wifi, bool bt) {
	int todo = 0;
	if (airplane) {
		if (!a->option)
			return 0;
		a->took_wifi = wifi;
		a->took_bt = bt;
		if (wifi)
			todo |= AIRPLANE_WIFI_OFF;
		if (bt)
			todo |= AIRPLANE_BT_OFF;
		return todo;
	}
	/* (whatever the option says now: what was taken goes back) */
	if (a->took_wifi && !wifi)
		todo |= AIRPLANE_WIFI_ON;
	if (a->took_bt && !bt)
		todo |= AIRPLANE_BT_ON;
	a->took_wifi = a->took_bt = false;
	return todo;
}

#endif
