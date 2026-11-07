#include <assert.h>
#include <stdio.h>
#include "../settings/bt-list.h"

static struct bt_device dev(const char *mac, bool paired, bool connected) {
	struct bt_device d = {0};
	strcpy(d.mac, mac);
	strcpy(d.name, mac);
	d.paired = paired;
	d.connected = connected;
	return d;
}

#define ORDER(a, b, c) assert(!strcmp(l[0].mac, a) && !strcmp(l[1].mac, b) && !strcmp(l[2].mac, c))

int main(void) {
	struct bt_device l[MAX_BT], f[MAX_BT];
	int n = 0;

	/* BlueZ reorders them: rows stay */
	f[0] = dev("A", 0, 0); f[1] = dev("B", 0, 0); f[2] = dev("C", 0, 0);
	n = bt_list_merge(l, n, f, 3, NULL);
	ORDER("A", "B", "C");
	f[0] = dev("C", 0, 0); f[1] = dev("A", 0, 0); f[2] = dev("B", 0, 0);
	n = bt_list_merge(l, n, f, 3, NULL);
	assert(n == 3);
	ORDER("A", "B", "C");

	/* a new one goes at the bottom, moving nothing */
	f[3] = dev("D", 0, 0);
	n = bt_list_merge(l, n, f, 4, NULL);
	assert(n == 4 && !strcmp(l[3].mac, "D"));
	ORDER("A", "B", "C");

	/* one leaves BlueZ's list: its row stays, marked, and comes back unmarked */
	f[0] = dev("A", 0, 0); f[1] = dev("C", 0, 0); f[2] = dev("D", 0, 0);
	n = bt_list_merge(l, n, f, 3, NULL);
	assert(n == 4 && l[1].gone && !strcmp(l[1].mac, "B") && !strcmp(l[2].mac, "C"));
	f[3] = dev("B", 0, 0);
	n = bt_list_merge(l, n, f, 4, NULL);
	assert(n == 4 && !l[1].gone);
	ORDER("A", "B", "C");

	/* pairing and connecting moves that row up, and the rest keep their order */
	f[1] = dev("C", 1, 1);
	n = bt_list_merge(l, n, f, 4, NULL);
	assert(!strcmp(l[0].mac, "C") && !strcmp(l[1].mac, "A") && !strcmp(l[2].mac, "B") && !strcmp(l[3].mac, "D"));

	/* a paired device that has left was deleted: its row goes */
	f[0] = dev("A", 0, 0); f[1] = dev("B", 0, 0); f[2] = dev("D", 0, 0);
	n = bt_list_merge(l, n, f, 3, NULL);
	assert(n == 3);
	ORDER("A", "B", "D");

	/* full: rows that have left give way to devices in range */
	n = 0;
	for (int i = 0; i < MAX_BT; i++) {
		char mac[18];
		snprintf(mac, sizeof(mac), "M%d", i);
		f[i] = dev(mac, 0, 0);
	}
	n = bt_list_merge(l, n, f, MAX_BT, NULL);
	struct bt_device g = dev("NEW", 0, 0);
	n = bt_list_merge(l, n, &g, 1, NULL);
	assert(n == MAX_BT && !strcmp(l[MAX_BT - 1].mac, "NEW") && bt_find(l, n, "M0") != NULL);

	/* connected, then the one connecting, then paired, then the rest */
	n = 0;
	f[0] = dev("A", 0, 0); f[1] = dev("B", 1, 0); f[2] = dev("C", 0, 0); f[3] = dev("D", 1, 1);
	n = bt_list_merge(l, n, f, 4, NULL);
	assert(!strcmp(l[0].mac, "D") && !strcmp(l[1].mac, "B") && !strcmp(l[2].mac, "A") && !strcmp(l[3].mac, "C"));
	bt_list_sort(l, n, "C");	/* tapped: it goes up, past paired B, below connected D */
	assert(!strcmp(l[0].mac, "D") && !strcmp(l[1].mac, "C") && !strcmp(l[2].mac, "B") && !strcmp(l[3].mac, "A"));
	bt_list_sort(l, n, "D");	/* connected stays first while busy */
	assert(!strcmp(l[0].mac, "D"));
	bt_list_sort(l, n, "");	/* failed: back among the rest, in stable order */
	assert(!strcmp(l[0].mac, "D") && !strcmp(l[1].mac, "B"));
	/* a row's drawn position survives a merge; a new row starts at its own slot */
	l[0].shown = 2.5f;
	f[0] = dev("D", 1, 1); f[1] = dev("B", 1, 0); f[2] = dev("C", 0, 0); f[3] = dev("A", 0, 0); f[4] = dev("Z", 0, 0);
	n = bt_list_merge(l, n, f, 5, NULL);
	assert(l[0].shown == 2.5f && !strcmp(l[n - 1].mac, "Z") && l[n - 1].shown == (float)(n - 1));

	puts("PASS: Bluetooth rows keep their places");
	return 0;
}
