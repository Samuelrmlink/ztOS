/* The Bluetooth page's list: rows stay where they are while the page is open.
 * BlueZ's own order, and which temporary devices it still lists, change with every
 * scan, and a row that moves under a finger can pair with a stranger's device. */
#ifndef ZTOS_BT_LIST_H
#define ZTOS_BT_LIST_H

#include <stdbool.h>
#include <string.h>

#ifndef MAX_BT
#define MAX_BT 32
#endif

struct bt_device {
	char mac[18], name[64], icon[32];
	bool paired, connected;
	bool gone;	/* BlueZ no longer lists it: kept so the rows below stay put */
	float shown;	/* the row it is drawn at, easing towards its place in the list */
};

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
