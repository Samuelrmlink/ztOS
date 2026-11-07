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
};

static inline int bt_rank(const struct bt_device *d) {
	return d->connected * 2 + d->paired;
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
static inline int bt_list_merge(struct bt_device *list, int n, const struct bt_device *fresh, int n_fresh) {
	struct bt_device out[MAX_BT];
	int m = 0;
	for (int i = 0; i < n; i++) {
		const struct bt_device *now = bt_find(fresh, n_fresh, list[i].mac);
		if (now) {
			out[m] = *now;
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
		out[m++].gone = false;
	}
	for (int i = 1; i < m; i++) {	/* stable: equal ranks keep their rows */
		struct bt_device d = out[i];
		int j = i;
		while (j > 0 && bt_rank(&out[j - 1]) < bt_rank(&d)) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = d;
	}
	memcpy(list, out, (size_t)m * sizeof(out[0]));
	return m;
}

#endif
