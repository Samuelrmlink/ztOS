// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Every advert in Apple's manufacturer data, whatever its type or address, for a while:
 *
 *   pods-adv-capture [--seconds N] [--label TEXT]     (default 30 s)
 *
 * One line each, on stdout, tab-separated:
 *   TIME_MS  ADDRESS  ADDRESS_TYPE  RSSI  raw=HEX  [what it is]
 * A line with raw=same is the same bytes heard again from that address (an RSSI change).
 *
 * It runs beside ztos-podsd as a discovery client of its own. It asks BlueZ for discovery and
 * nothing else: it never powers Bluetooth, connects or pairs. It refuses to start while
 * Bluetooth is off, or while A2DP is playing, and stops if A2DP starts.
 *
 * With the daemon's keys file it also tries each key on what it hears, printing what opens
 * (the plaintext). The keys themselves are never printed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "pods-proto.h"

#define MAX_PAIRS 6
#define MAX_ADDRS 256

static const char BLUEZ[] = "org.bluez", DEVICE1[] = "org.bluez.Device1", ADAPTER1[] = "org.bluez.Adapter1",
	TRANSPORT1[] = "org.bluez.MediaTransport1";

static struct { char mac[18]; uint8_t mac_b[6], irk[16], enc[16]; } pairs[MAX_PAIRS];
static int n_pairs;

/* addresses Apple's data has come from: for the RSSI-only lines, and the summary */
static struct { char mac[18]; int count, changes; uint8_t last[64]; size_t last_len; char type[8]; } addrs[MAX_ADDRS];
static int n_addrs;

static sd_bus *bus;
static sd_event *event;
static char adapter[64];
static bool powered, started;
static int exit_code;

static void load_keys(void) {
	const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char path[300], line[200], a[33], b[33], mac[18];
	if (data) snprintf(path, sizeof(path), "%s/ztos/pods/keys", data);
	else snprintf(path, sizeof(path), "%s/.local/share/ztos/pods/keys", home ? home : "");
	FILE *f = fopen(path, "r");
	while (f && n_pairs < MAX_PAIRS && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%17s %32s %32s", mac, a, b) != 3 || !pods_mac_parse(mac, pairs[n_pairs].mac_b)) continue;
		bool ok = true;
		for (int i = 0; i < 16 && ok; i++) {
			unsigned x, y;
			ok = sscanf(a + i * 2, "%2x", &x) == 1 && sscanf(b + i * 2, "%2x", &y) == 1;
			if (ok) {
				pairs[n_pairs].irk[i] = (uint8_t)x;
				pairs[n_pairs].enc[i] = (uint8_t)y;
			}
		}
		if (ok) snprintf(pairs[n_pairs++].mac, 18, "%s", mac);
	}
	if (f) fclose(f);
	fprintf(stderr, "pods-adv-capture: %d pair(s) of keys to try\n", n_pairs);
}

static void hex(const uint8_t *b, size_t n) {
	for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

/* What it is, as far as anyone knows. Every message's type; then whatever opens. */
static void annotate(const uint8_t addr[6], const uint8_t *m, size_t n) {
	printf("\ttypes=");
	for (size_t at = 0; at + 2 <= n;) {
		size_t len = m[at + 1] & 0x1f;
		printf("%02x(%zu)%s", m[at], len, at + 2 + len < n ? "," : "");
		if (at + 2 + len > n) break;
		at += 2 + len;
	}
	const uint8_t *prox = ble_find_proximity(m, n), *kase = ble_find_case(m, n);
	for (int i = 0; i < n_pairs; i++) {
		if (ble_rpa_resolves(pairs[i].irk, addr)) printf("\tirk=pair%d", i);
		uint8_t pt[16];
		if (prox) {
			pods_aes_decrypt(pairs[i].enc, prox + 9, pt);
			if (ble_rpa_resolves(pairs[i].irk, addr)) {
				printf("\tprox_plain=");
				hex(pt, 16);
			}
		}
		/* any block of 16 under any type: the format may not be the one we think */
		for (size_t at = 0; at + 2 <= n;) {
			size_t len = m[at + 1] & 0x1f;
			if (at + 2 + len > n) break;
			if (len >= 16) {
				pods_aes_decrypt(pairs[i].enc, m + at + 2 + len - 16, pt);
				if (pt[1] == 0x20 || (pt[7] == pairs[i].mac_b[3] && pt[8] == pairs[i].mac_b[4] && pt[9] == pairs[i].mac_b[5])) {
					printf("\ttype%02x_tail_plain_pair%d=", m[at], i);
					hex(pt, 16);
				}
			}
			at += 2 + len;
		}
		struct ble_exact e;
		if (kase && ble_decrypt_case(pairs[i].enc, kase, &e)) {
			printf("\tCASE_pair%d case=%d/%d left=%d/%d right=%d/%d", i, e.level[POD_CASE], e.charging[POD_CASE],
				e.level[POD_LEFT], e.charging[POD_LEFT], e.level[POD_RIGHT], e.charging[POD_RIGHT]);
		}
	}
	if (prox) {
		struct ble_clear c;
		if (ble_parse_clear(prox, &c)) {
			printf("\tclear product=%04x sender=%s st=%02x left=%d right=%d case=%d chg=%d%d%d lid=%d conn=%02x",
				c.product, c.sender_left ? "L" : "R", prox[3], c.level[POD_LEFT], c.level[POD_RIGHT], c.level[POD_CASE],
				c.charging[POD_LEFT], c.charging[POD_RIGHT], c.charging[POD_CASE], c.lid, c.connection);
		}
	}
}

static void heard(const char *path, const char *address, const char *type, int rssi, const uint8_t *m, size_t n) {
	char mac[18];
	if (!pods_path_mac(path, mac)) return;
	int at = -1;
	for (int i = 0; i < n_addrs; i++) {
		if (!strcmp(addrs[i].mac, mac)) at = i;
	}
	if (at < 0) {
		/* only Apple's: a device with no Apple data yet is no line */
		if (!m || n_addrs >= MAX_ADDRS) return;
		at = n_addrs++;
		snprintf(addrs[at].mac, 18, "%s", mac);
	}
	if (type) snprintf(addrs[at].type, sizeof(addrs[at].type), "%.7s", type);
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	addrs[at].count++;
	bool same = !m || (n == addrs[at].last_len && !memcmp(m, addrs[at].last, n < 64 ? n : 64));
	printf("%lld\t%s\t%s\t%d\t", (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000, mac, addrs[at].type[0] ? addrs[at].type : "?", rssi);
	if (same && addrs[at].last_len) {
		printf("raw=same");
	} else {
		uint8_t b[6];
		addrs[at].changes++;
		addrs[at].last_len = n < 64 ? n : 64;
		memcpy(addrs[at].last, m, addrs[at].last_len);
		printf("raw=");
		hex(m, n);
		if (pods_mac_parse(mac, b)) annotate(b, m, n);
	}
	putchar('\n');
	fflush(stdout);
	(void)address;
}

/* one a{sv} of an interface; what's wanted is printed or noted */
static int read_props(const char *path, const char *iface, sd_bus_message *m) {
	const uint8_t *mfr = NULL;
	size_t mfr_len = 0;
	const char *address = NULL, *type = NULL, *state = NULL;
	int16_t rssi = 0;
	bool has_rssi = false;
	int pow = -1;
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) return r;
		if (!strcmp(iface, ADAPTER1) && !strcmp(key, "Powered")) {
			sd_bus_message_read(m, "v", "b", &pow);
		} else if (!strcmp(iface, TRANSPORT1) && !strcmp(key, "State")) {
			sd_bus_message_read(m, "v", "s", &state);
		} else if (strcmp(iface, DEVICE1)) {
			sd_bus_message_skip(m, "v");
		} else if (!strcmp(key, "Address")) {
			sd_bus_message_read(m, "v", "s", &address);
		} else if (!strcmp(key, "AddressType")) {
			sd_bus_message_read(m, "v", "s", &type);
		} else if (!strcmp(key, "RSSI")) {
			has_rssi = sd_bus_message_read(m, "v", "n", &rssi) >= 0;
		} else if (!strcmp(key, "ManufacturerData")) {
			if ((r = sd_bus_message_enter_container(m, 'v', "a{qv}")) < 0) return r;
			if ((r = sd_bus_message_enter_container(m, 'a', "{qv}")) < 0) return r;
			while (sd_bus_message_enter_container(m, 'e', "qv") > 0) {
				uint16_t company;
				sd_bus_message_read(m, "q", &company);
				if (company == APPLE_COMPANY_ID && sd_bus_message_enter_container(m, 'v', "ay") > 0) {
					const void *bytes;
					size_t len;
					if (sd_bus_message_read_array(m, 'y', &bytes, &len) >= 0) {
						mfr = bytes;
						mfr_len = len;
					}
					sd_bus_message_exit_container(m);
				} else {
					sd_bus_message_skip(m, "v");
				}
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		if ((r = sd_bus_message_exit_container(m)) < 0) return r;
	}
	if (r < 0) return r;
	r = sd_bus_message_exit_container(m);
	if (!strcmp(iface, ADAPTER1)) {
		if (!adapter[0]) snprintf(adapter, sizeof(adapter), "%s", path);
		if (pow >= 0) powered = pow;
	} else if (!strcmp(iface, TRANSPORT1) && state && !strcmp(state, "active")) {
		fprintf(stderr, "pods-adv-capture: A2DP is playing: stopping\n");
		exit_code = 3;
		sd_event_exit(event, 3);
	} else if (!strcmp(iface, DEVICE1) && started && (mfr || has_rssi)) {
		heard(path, address, type, rssi, mfr, mfr_len);
	}
	return r;
}

static int read_interfaces(const char *path, sd_bus_message *m) {
	int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface;
		if ((r = sd_bus_message_read(m, "s", &iface)) < 0) return r;
		if ((r = read_props(path, iface, m)) < 0) return r;
		if ((r = sd_bus_message_exit_container(m)) < 0) return r;
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

static int on_added(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *path;
	if (sd_bus_message_read(m, "o", &path) >= 0) read_interfaces(path, m);
	return 0;
}

static int on_changed(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *iface;
	if (sd_bus_message_read(m, "s", &iface) >= 0) read_props(sd_bus_message_get_path(m), iface, m);
	return 0;
}

static int on_end(sd_event_source *s, uint64_t usec, void *data) {
	sd_event_exit(event, 0);
	return 0;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *data) {
	sd_event_exit(event, 0);
	return 0;
}

static int fail(const char *what, int r) {
	fprintf(stderr, "pods-adv-capture: %s: %s\n", what, strerror(r < 0 ? -r : r));
	return 1;
}

int main(int argc, char **argv) {
	int seconds = 30;
	const char *label = "";
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--label")) label = argv[i + 1];
	}
	if (seconds <= 0) seconds = 30;
	load_keys();

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int r = sd_event_default(&event);
	if (r < 0) return fail("event", r);
	sd_event_add_signal(event, NULL, SIGTERM, on_signal, NULL);
	sd_event_add_signal(event, NULL, SIGINT, on_signal, NULL);
	if ((r = sd_bus_open_system(&bus)) < 0) return fail("system bus", r);
	sd_bus_set_exit_on_disconnect(bus, 1);
	sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);

	/* what is there: without starting bluetoothd, were it down */
	sd_bus_message *call = NULL, *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message_new_method_call(bus, &call, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
	sd_bus_message_set_auto_start(call, 0);
	if ((r = sd_bus_call(bus, call, 5000000, &err, &reply)) < 0) return fail("BlueZ isn't there", r);
	sd_bus_message_unref(call);
	if ((r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}")) < 0) return fail("objects", r);
	while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
		const char *path;
		sd_bus_message_read(reply, "o", &path);
		read_interfaces(path, reply);
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_unref(reply);
	if (exit_code) return exit_code;
	if (!adapter[0] || !powered) {
		fprintf(stderr, "pods-adv-capture: Bluetooth is off; leaving it so\n");
		return 2;
	}

	const char *om = "org.freedesktop.DBus.ObjectManager";
	sd_bus_match_signal(bus, NULL, BLUEZ, NULL, om, "InterfacesAdded", on_added, NULL);
	sd_bus_match_signal(bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", on_changed, NULL);

	sd_bus_message_new_method_call(bus, &call, BLUEZ, adapter, ADAPTER1, "SetDiscoveryFilter");
	sd_bus_message_set_auto_start(call, 0);
	sd_bus_message_append(call, "a{sv}", 2, "Transport", "s", "le", "DuplicateData", "b", 1);
	if ((r = sd_bus_call(bus, call, 5000000, &err, NULL)) < 0) return fail("SetDiscoveryFilter", r);
	sd_bus_message_unref(call);
	sd_bus_message_new_method_call(bus, &call, BLUEZ, adapter, ADAPTER1, "StartDiscovery");
	sd_bus_message_set_auto_start(call, 0);
	if ((r = sd_bus_call(bus, call, 5000000, &err, NULL)) < 0) return fail("StartDiscovery", r);
	sd_bus_message_unref(call);
	started = true;
	printf("# label=%s seconds=%d start=%ld\n", label, seconds, (long)time(NULL));
	fflush(stdout);

	uint64_t now;
	sd_event_now(event, CLOCK_MONOTONIC, &now);
	sd_event_add_time(event, NULL, CLOCK_MONOTONIC, now + (uint64_t)seconds * 1000000, 100000, on_end, NULL);
	r = sd_event_loop(event);

	sd_bus_message_new_method_call(bus, &call, BLUEZ, adapter, ADAPTER1, "StopDiscovery");
	sd_bus_message_set_auto_start(call, 0);
	sd_bus_call(bus, call, 5000000, NULL, NULL);
	sd_bus_message_unref(call);

	printf("# end=%ld addresses=%d\n", (long)time(NULL), n_addrs);
	for (int i = 0; i < n_addrs; i++) printf("# %s heard=%d changes=%d\n", addrs[i].mac, addrs[i].count, addrs[i].changes);
	sd_bus_flush_close_unref(bus);
	return exit_code;
}
