/* The Wi-Fi page's rows (settings/wifi-list.h): one per name, and the one the
 * phone is on reads "connected" when nmcli lists a stronger access point of
 * the same name above it (ROADMAP 8.51). */
#include "../settings/wifi-list.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
	struct network list[4];
	int n = 0;

	/* 2.4 GHz first (stronger), the 5 GHz one the phone is on second */
	wifi_list_add(list, &n, 4, " ", "Wireless Hotspot", "82", "WPA2");
	wifi_list_add(list, &n, 4, "*", "Wireless Hotspot", "60", "WPA2");
	wifi_list_add(list, &n, 4, " ", "sdn", "40", "WPA2");
	wifi_list_add(list, &n, 4, " ", "", "90", "WPA2");	/* hidden: left out */
	wifi_list_add(list, &n, 4, " ", "sdn", "55", "WPA2");
	CHECK(n == 2);
	CHECK(strcmp(list[0].ssid, "Wireless Hotspot") == 0);
	CHECK(list[0].in_use);
	CHECK(list[0].signal == 60);	/* the one it is on */
	CHECK(list[0].secured && strcmp(list[0].security, "WPA2") == 0);
	CHECK(!list[1].in_use);
	CHECK(list[1].signal == 55);	/* not in use: the strongest */

	/* in use first: a weaker line after it changes nothing */
	n = 0;
	wifi_list_add(list, &n, 4, "*", "home", "70", "--");
	wifi_list_add(list, &n, 4, " ", "home", "90", "--");
	CHECK(n == 1 && list[0].in_use && list[0].signal == 70);
	CHECK(!list[0].secured && strcmp(list[0].security, "open") == 0);

	/* the list's end: new names stop, a listed name still merges */
	n = 0;
	wifi_list_add(list, &n, 1, " ", "a", "10", "WPA2");
	wifi_list_add(list, &n, 1, " ", "b", "10", "WPA2");
	wifi_list_add(list, &n, 1, "*", "a", "20", "WPA2");
	CHECK(n == 1 && list[0].in_use && list[0].signal == 20);

	if (failures) return 1;
	printf("wifi-list: ok\n");
	return 0;
}
