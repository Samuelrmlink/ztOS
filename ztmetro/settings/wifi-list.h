/* The Wi-Fi page's list: one row per network name. NetworkManager lists one
 * line per access point (`nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi
 * list`), strongest first, so a router with a 2.4 GHz and a 5 GHz radio under
 * one name is listed twice, and the one the phone is on is often not the
 * first: the 2.4 GHz one is usually stronger. Keeping only the first line of
 * each name showed the network the phone was on as "saved, secured", not
 * "connected", and a tap on it started the connection again (ROADMAP 8.51).
 * So the lines of one name are merged: in use if any of them is, with the
 * signal of the one in use, else the strongest. */
#ifndef ZTOS_WIFI_LIST_H
#define ZTOS_WIFI_LIST_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct network {
	char ssid[64];
	int signal;
	bool secured, in_use, known;
	char security[24];
};

/* Add one nmcli line's fields to the list of `*count` (at most `max`): a
 * hidden network (no name) is left out, and a name already listed is merged
 * into its row. */
static inline void wifi_list_add(struct network *list, int *count, int max, const char *in_use,
		const char *ssid, const char *signal, const char *security) {
	if (!ssid[0]) return;
	bool use = in_use[0] == '*';
	int strength = atoi(signal);
	for (int i = 0; i < *count; i++) {
		struct network *n = &list[i];
		if (strcmp(n->ssid, ssid) != 0) continue;
		if (use && !n->in_use) {
			n->in_use = true;
			n->signal = strength;	/* the fan shows the one it is on */
		} else if (!n->in_use && strength > n->signal) {
			n->signal = strength;
		}
		return;
	}
	if (*count >= max) return;
	struct network *n = &list[(*count)++];
	snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
	n->signal = strength;
	n->secured = security[0] && strcmp(security, "--") != 0;
	snprintf(n->security, sizeof(n->security), "%s", n->secured ? security : "open");
	n->in_use = use;
	n->known = false;
}

#endif
