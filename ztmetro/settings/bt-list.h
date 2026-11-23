/* The Bluetooth page's list: rows stay where they are while the page is open.
 * BlueZ's own order, and which temporary devices it still lists, change with every
 * scan, and a row that moves under a finger can pair with a stranger's device. */
#ifndef ZTOS_BT_LIST_H
#define ZTOS_BT_LIST_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef MAX_BT
#define MAX_BT 32
#endif

struct bt_device {
	char mac[18], name[64], icon[32];
	bool paired, connected;
	bool le, bredr;	/* which link is up: BlueZ 5.8x says per bearer (older: neither) */
	bool gone;	/* BlueZ no longer lists it: kept so the rows below stay put */
	float shown;	/* the row it is drawn at, easing towards its place in the list */
};

/* What the page asks BlueZ: "yes|no" (powered), "name=" what others see this
 * phone as, then "@devices" and one record each, "MAC|name|paired|connected|
 * icon|le|bredr". BlueZ 5.8x's `bluetoothctl info` adds per-bearer lines
 * ("LE.Connected: yes", "BREDR.Paired: yes") below the device's own. Each
 * field is the first line that is exactly that key, so the bearer lines
 * neither land in "Connected" (they split every record over several lines,
 * and every device read as not connected: ROADMAP 8.50) nor go unread. */
#define BT_QUERY "s=$(bluetoothctl show 2>/dev/null); " \
	"p=$(echo \"$s\" | sed -n 's/^[[:space:]]*Powered: //p' | head -n 1); echo \"${p:-no}\"; " \
	"echo \"name=$(echo \"$s\" | sed -n 's/^[[:space:]]*Alias: //p' | head -n 1)\"; " \
	"echo @devices; bluetoothctl devices 2>/dev/null | while read -r _ mac name; do " \
	"i=$(bluetoothctl info \"$mac\" 2>/dev/null); " \
	"f() { echo \"$i\" | sed -n \"s/^[[:space:]]*$1: //p\" | head -n 1; }; " \
	"echo \"$mac|$name|$(f Paired)|$(f Connected)|$(f Icon)|$(f 'LE[.]Connected')|$(f 'BREDR[.]Connected')\"; done"

/* A MAC address as bluetoothctl prints it (and so safe in a command). */
static inline bool bt_is_mac(const char *s) {
	if (strlen(s) != 17) return false;
	for (int i = 0; i < 17; i++) {
		char c = s[i];
		bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (i % 3 == 2 ? c != ':' : !hex) return false;
	}
	return true;
}

/* One BT_QUERY record into d (line is cut up). False if it is not one: a
 * line that does not start with a MAC is left out, not listed as a device. */
static inline bool bt_record_parse(char *line, struct bt_device *d) {
	char *f[7] = { line, "", "", "", "", "", "" };
	for (int i = 1; i < 7; i++) {
		char *bar = strchr(f[i - 1], '|');
		if (!bar) break;
		*bar = 0;
		f[i] = bar + 1;
	}
	if (!bt_is_mac(f[0])) return false;
	memset(d, 0, sizeof(*d));
	snprintf(d->mac, sizeof(d->mac), "%s", f[0]);
	snprintf(d->name, sizeof(d->name), "%s", f[1][0] ? f[1] : f[0]);
	d->paired = strncmp(f[2], "yes", 3) == 0;
	d->connected = strncmp(f[3], "yes", 3) == 0;
	snprintf(d->icon, sizeof(d->icon), "%s", f[4]);
	d->le = d->connected && strncmp(f[5], "yes", 3) == 0;
	d->bredr = d->connected && strncmp(f[6], "yes", 3) == 0;
	return true;
}

/* A connected row's line: "Connected (BLE)" for a link that is only LE (a
 * GATT tool, a sensor), so it is not taken for audio; plain otherwise. */
static inline const char *bt_connected_label(const struct bt_device *d) {
	return d->le && !d->bredr ? "Connected (BLE)" : "Connected";
}

/* Connected on top, then the one being connected, then paired, then the rest. */
static inline int bt_rank(const struct bt_device *d, const char *busy) {
	if (d->connected) return 3;
	if (busy && busy[0] && strcmp(d->mac, busy) == 0) return 2;
	return d->paired;
}

/* Stable: equal ranks keep their rows. */
static inline void bt_list_sort(struct bt_device *list, int n, const char *busy) {
	for (int i = 1; i < n; i++) {
		struct bt_device d = list[i];
		int j = i;
		while (j > 0 && bt_rank(&list[j - 1], busy) < bt_rank(&d, busy)) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = d;
	}
}

static inline const struct bt_device *bt_find(const struct bt_device *list, int n, const char *mac) {
	for (int i = 0; i < n; i++) {
		if (strcmp(list[i].mac, mac) == 0) return &list[i];
	}
	return NULL;
}

/* list becomes fresh's devices in the order list already had them, new ones after
 * them. A device that was paired and has left was deleted, so its row goes too.
 * Connected first, then paired: that move is the user's own doing. Returns the count. */
static inline int bt_list_merge(struct bt_device *list, int n, const struct bt_device *fresh, int n_fresh,
		const char *busy) {
	struct bt_device out[MAX_BT];
	int m = 0;
	for (int i = 0; i < n; i++) {
		const struct bt_device *now = bt_find(fresh, n_fresh, list[i].mac);
		if (now) {
			out[m] = *now;
			out[m].shown = list[i].shown;
			out[m++].gone = false;
		} else if (!list[i].paired) {
			out[m] = list[i];
			out[m].connected = false;
			out[m++].gone = true;
		}
	}
	for (int i = 0; i < n_fresh; i++) {
		if (bt_find(list, n, fresh[i].mac)) continue;
		if (m == MAX_BT) {	/* full: a device in range outranks one that has left */
			int j = m - 1;
			while (j >= 0 && !out[j].gone) j--;
			if (j < 0) break;
			memmove(&out[j], &out[j + 1], (size_t)(m - 1 - j) * sizeof(out[0]));
			m--;
		}
		out[m] = fresh[i];
		out[m].shown = (float)m;
		out[m++].gone = false;
	}
	bt_list_sort(out, m, busy);
	memcpy(list, out, (size_t)m * sizeof(out[0]));
	return m;
}

#endif
