#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

	/* records: the page's own fields, and which link is up */
	struct bt_device d;
	char rec[160];
	strcpy(rec, "E3:19:D9:61:F8:3B|ChameleonUltra|yes|yes||yes|");
	assert(bt_record_parse(rec, &d) && d.paired && d.connected && d.le && !d.bredr);
	assert(!strcmp(bt_connected_label(&d), "Connected (BLE)"));
	strcpy(rec, "40:DA:5C:62:CA:1B|AirPods Pro|yes|yes|audio-headphones|no|yes");
	assert(bt_record_parse(rec, &d) && d.connected && !d.le && d.bredr && !strcmp(d.icon, "audio-headphones"));
	assert(!strcmp(bt_connected_label(&d), "Connected"));
	strcpy(rec, "40:DA:5C:62:CA:1B|AirPods Pro|yes|yes|audio-headphones|yes|yes");	/* both links */
	assert(bt_record_parse(rec, &d) && !strcmp(bt_connected_label(&d), "Connected"));
	strcpy(rec, "00:0D:44:EB:50:54|UE MINI BOOM|yes|yes|audio-headset");	/* older BlueZ: no bearer fields */
	assert(bt_record_parse(rec, &d) && d.connected && !d.le && !strcmp(bt_connected_label(&d), "Connected"));
	strcpy(rec, "31:9D:68:1F:38:A4|PM220|yes|no|printer|yes|no");	/* not connected: no link */
	assert(bt_record_parse(rec, &d) && !d.connected && !d.le);
	/* what the old query's split records left: not devices */
	strcpy(rec, "yes|yes");
	assert(!bt_record_parse(rec, &d));
	strcpy(rec, "no|audio-headphones");
	assert(!bt_record_parse(rec, &d));
	strcpy(rec, "yes");
	assert(!bt_record_parse(rec, &d));
	puts("PASS: Bluetooth records, LE links included");

	/* the query itself, against what BlueZ 5.88's bluetoothctl prints (tests/bt-fake) */
	const char *fake = getenv("FAKE_BT");
	if (!fake) {
		puts("SKIP: BT_QUERY (no FAKE_BT)");
		return 0;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/bin:%s", fake, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	setenv("PATH", path, 1);
	FILE *p = popen(BT_QUERY, "r");
	assert(p);
	char line[256];
	int records = 0, junk = 0;
	bool powered = false, named = false, cham = false, pods = false, boom = false;
	bool devices = false;
	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\n")] = 0;
		if (!strcmp(line, "@devices")) { devices = true; continue; }
		if (!devices) {
			if (!strcmp(line, "yes")) powered = true;
			if (!strcmp(line, "name=ztos")) named = true;
			continue;
		}
		if (!bt_record_parse(line, &d)) { junk++; continue; }
		records++;
		if (!strcmp(d.mac, "E3:19:D9:61:F8:3B")) {
			cham = d.connected && d.paired && d.le && !d.bredr &&
				!strcmp(bt_connected_label(&d), "Connected (BLE)");
		} else if (!strcmp(d.mac, "40:DA:5C:62:CA:1B")) {
			pods = d.paired && !d.connected && !strcmp(d.name, "AirPods Pro") &&
				!strcmp(d.icon, "audio-headphones");
		} else if (!strcmp(d.mac, "00:0D:44:EB:50:54")) {
			boom = d.connected && !d.le && !strcmp(bt_connected_label(&d), "Connected");
		}
	}
	assert(pclose(p) == 0);
	assert(powered && named);
	assert(records == 5 && junk == 0);	/* one line per device: none split */
	assert(cham && pods && boom);
	puts("PASS: BT_QUERY reads BlueZ 5.88's LE and BR/EDR devices");
	return 0;
}
