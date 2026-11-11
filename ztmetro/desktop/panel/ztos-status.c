// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztos-status: the phone's state in XFCE's panel, on the desktop (DESIGN.md,
 * "The panel"). Cellular, Wi-Fi, VPN (WireGuard, Tailscale), Bluetooth (read
 * only) and the battery, as icons with tooltips. An internal panel plugin: it
 * runs in xfce4-panel's own process.
 *
 * Everything is told, nothing is polled faster than once a minute:
 *   cellular, Wi-Fi bars   /run/ztoscorenetworking/status (ztoscorenetworkingd
 *                          rewrites it when it changes): inotify
 *   Wi-Fi network          NetworkManager, D-Bus: its PropertiesChanged
 *   WireGuard              rtnetlink link messages; /sys/class/net/X/uevent
 *                          says DEVTYPE=wireguard
 *   Tailscale              tailscaled's LocalAPI watch-ipn-bus: a line per
 *                          change of state (userspace networking has no tun)
 *   Bluetooth              BlueZ, D-Bus: PropertiesChanged and InterfacesAdded
 *                          or Removed; never a call that changes anything
 *   battery                sysfs, on a power_supply uevent (a charger plugged
 *                          or pulled) and once a minute for the percentage
 *
 * While nobody sees the desktop ($XDG_RUNTIME_DIR/ztmetro-desktop/viewed is
 * 0: ztmetro writes it, see desk_view_sync) all of that is closed: no watch,
 * no subscription, no socket, no timer. Only the inotify on that one file is
 * left, so the plugin costs no wakeup at all. Seen again, everything is read
 * afresh and watched again.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#define BATTERY "/sys/class/power_supply/max170xx_battery"
#define MODEM_DIR "/run/ztoscorenetworking"
#define TAILSCALE_SOCK "/run/tailscale/tailscaled.sock"
#define MINUTE_S 60

enum { I_CELL, I_WIFI, I_VPN, I_BT, I_BATT, I_COUNT };

typedef struct {
	XfcePanelPlugin *plugin;
	GtkWidget *box, *icon[I_COUNT], *ebox[I_COUNT], *batt_label;
	char *view_dir;
	int view_fd;			/* inotify on view_dir: always open */
	guint view_src;
	gboolean viewed, live;		/* seen; the watches below are open */

	/* the watches, open only while seen */
	int modem_fd;			/* inotify on MODEM_DIR */
	guint modem_src;
	int rtnl_fd, uevent_fd;
	guint rtnl_src, uevent_src;
	guint minute_src;
	GDBusConnection *sys;
	guint nm_sub, bz_sub, bz_add_sub, bz_rm_sub;
	GCancellable *cancel;		/* D-Bus calls and the Tailscale stream */
	GSocketConnection *ts_conn;
	gboolean ts_connecting;
	GString *ts_buf;

	/* what is shown */
	char cell_sim[16], cell_op[64], cell_tech[8], cell_reg[16];
	int cell_bars, cell_airplane, cell_data, wifi_bars;
	char wifi_ssid[64], wifi_ip[64];
	int wifi_state;			/* 0 off (radio), 1 not connected, 2 connected */
	int wifi_strength;		/* % or -1 */
	char wg[128];			/* WireGuard interfaces up, space separated */
	int ts_state;			/* tailscaled's ipn.State; -1 not there */
	int bt_on;			/* -1 no adapter */
	char bt_dev[64];
	int bt_batt;
	int batt_pct, batt_charging, batt_full, batt_ma;
} Zs;

/* ---- small things ------------------------------------------------------------- */

static gboolean read_line(const char *path, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) return FALSE;
	gboolean ok = fgets(out, size, f) != NULL;
	fclose(f);
	if (ok) out[strcspn(out, "\n")] = 0;
	return ok;
}

static int read_int(const char *path, int dflt) {
	char b[32];
	return read_line(path, b, sizeof(b)) ? atoi(b) : dflt;
}

static void set_icon(Zs *z, int i, const char *name, const char *tip) {
	if (!name) {
		gtk_widget_hide(z->ebox[i]);
		return;
	}
	gtk_image_set_from_icon_name(GTK_IMAGE(z->icon[i]), name, GTK_ICON_SIZE_BUTTON);
	gtk_image_set_pixel_size(GTK_IMAGE(z->icon[i]), xfce_panel_plugin_get_icon_size(z->plugin));
	gtk_widget_set_tooltip_text(z->ebox[i], tip);
	gtk_widget_show(z->ebox[i]);
}

static const char *level5(int bars) {
	static const char *const n[] = { "none", "weak", "ok", "good", "excellent" };
	return n[bars < 0 ? 0 : bars > 4 ? 4 : bars];
}

/* ---- drawing (only ever while seen) ------------------------------------------- */

static void show_cell(Zs *z) {
	char tip[256], name[64];
	if (!z->cell_sim[0] || !strcmp(z->cell_sim, "absent")) {
		set_icon(z, I_CELL, NULL, NULL);
		return;
	}
	if (z->cell_airplane) {
		set_icon(z, I_CELL, "network-cellular-offline-symbolic", "Mobile: airplane mode");
		return;
	}
	gboolean reg = !strcmp(z->cell_reg, "home") || !strcmp(z->cell_reg, "roaming");
	if (!reg) {
		snprintf(tip, sizeof(tip), "Mobile: no service (SIM %s)", z->cell_sim);
		set_icon(z, I_CELL, "network-cellular-no-route-symbolic", tip);
		return;
	}
	snprintf(name, sizeof(name), "network-cellular-signal-%s-symbolic", level5(z->cell_bars));
	snprintf(tip, sizeof(tip), "Mobile: %s%s%s, %d of 4 bars%s\nData %s",
		z->cell_op[0] ? z->cell_op : "registered", z->cell_tech[0] ? " " : "", z->cell_tech,
		z->cell_bars, !strcmp(z->cell_reg, "roaming") ? ", roaming" : "",
		z->cell_data ? "connected" : "off");
	set_icon(z, I_CELL, name, tip);
}

static void show_wifi(Zs *z) {
	char tip[256], name[64];
	if (z->wifi_state == 0) {
		set_icon(z, I_WIFI, "network-wireless-disabled-symbolic", "Wi-Fi: off");
		return;
	}
	if (z->wifi_state == 1) {
		set_icon(z, I_WIFI, "network-wireless-offline-symbolic", "Wi-Fi: not connected");
		return;
	}
	/* ztoscorenetworkingd's 0..3 bars when it has them, else NM's strength */
	int bars = z->wifi_bars >= 0 ? z->wifi_bars + 1 :
		z->wifi_strength >= 0 ? (z->wifi_strength + 19) / 20 - 1 : 4;
	if (bars < 1) bars = 1;
	snprintf(name, sizeof(name), "network-wireless-signal-%s-symbolic", level5(bars));
	char strength[32] = "";
	if (z->wifi_strength >= 0) snprintf(strength, sizeof(strength), ", %d%%", z->wifi_strength);
	snprintf(tip, sizeof(tip), "Wi-Fi: %s%s%s%s", z->wifi_ssid[0] ? z->wifi_ssid : "connected",
		strength, z->wifi_ip[0] ? "\n" : "", z->wifi_ip);
	set_icon(z, I_WIFI, name, tip);
}

static void show_vpn(Zs *z) {
	static const char *const ts_names[] = { "starting", "in use by another user", "needs login",
		"needs machine authorization", "stopped", "starting", "connected" };
	char tip[256] = "";
	gboolean up = z->wg[0] || z->ts_state == 6;
	if (z->ts_state >= 0 && z->ts_state <= 6) {
		snprintf(tip, sizeof(tip), "Tailscale: %s", ts_names[z->ts_state]);
	}
	if (z->wg[0]) {
		size_t n = strlen(tip);
		snprintf(tip + n, sizeof(tip) - n, "%sWireGuard: %s", n ? "\n" : "", z->wg);
	}
	set_icon(z, I_VPN, up ? "network-vpn-symbolic" : tip[0] ? "network-vpn-disabled-symbolic" : NULL, tip);
}

static void show_bt(Zs *z) {
	char tip[160];
	if (z->bt_on < 0) {
		set_icon(z, I_BT, NULL, NULL);
	} else if (!z->bt_on) {
		set_icon(z, I_BT, "bluetooth-disabled-symbolic", "Bluetooth: off");
	} else if (z->bt_dev[0]) {
		if (z->bt_batt >= 0) snprintf(tip, sizeof(tip), "Bluetooth: %s, battery %d%%", z->bt_dev, z->bt_batt);
		else snprintf(tip, sizeof(tip), "Bluetooth: %s", z->bt_dev);
		set_icon(z, I_BT, "bluetooth-active-symbolic", tip);
	} else {
		set_icon(z, I_BT, "bluetooth-disconnected-symbolic", "Bluetooth: on, nothing connected");
	}
}

static void show_batt(Zs *z) {
	char name[64], tip[128], label[8];
	if (z->batt_pct < 0) {
		set_icon(z, I_BATT, NULL, NULL);
		gtk_widget_hide(z->batt_label);
		return;
	}
	int lvl = (z->batt_pct + 5) / 10 * 10;
	if (lvl > 100) lvl = 100;
	snprintf(name, sizeof(name), "battery-level-%d%s-symbolic", lvl,
		z->batt_charging && lvl < 100 ? "-charging" : "");
	snprintf(tip, sizeof(tip), "Battery: %d%%, %s (%d mA)", z->batt_pct,
		z->batt_full ? "full" : z->batt_charging ? "charging" : "on battery", z->batt_ma);
	set_icon(z, I_BATT, name, tip);
	snprintf(label, sizeof(label), "%d%%", z->batt_pct);
	gtk_label_set_text(GTK_LABEL(z->batt_label), label);
	gtk_widget_set_tooltip_text(z->batt_label, tip);
	gtk_widget_show(z->batt_label);
}

/* ---- the battery: sysfs ------------------------------------------------------ */

static void read_batt(Zs *z) {
	char st[32] = "";
	z->batt_pct = read_int(BATTERY "/capacity", -1);
	read_line(BATTERY "/status", st, sizeof(st));
	z->batt_full = !strcmp(st, "Full");
	z->batt_charging = !strcmp(st, "Charging") || z->batt_full;
	z->batt_ma = read_int(BATTERY "/current_now", 0) / 1000;
	show_batt(z);
}

static void ts_open(Zs *z);

static gboolean on_minute(gpointer data) {
	Zs *z = data;
	read_batt(z);
	if (!z->ts_conn && !z->ts_connecting) ts_open(z);	/* tailscaled (re)started */
	return G_SOURCE_CONTINUE;
}

/* a uevent: only power_supply ones count (a charger in or out) */
static gboolean on_uevent(gint fd, GIOCondition c, gpointer data) {
	char buf[4096];
	gboolean batt = FALSE;
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strcmp(p, "SUBSYSTEM=power_supply")) batt = TRUE;
		}
	}
	if (batt) read_batt(data);
	return G_SOURCE_CONTINUE;
}

/* ---- cellular: ztoscorenetworkingd's status file ----------------------------- */

static void read_modem(Zs *z) {
	FILE *f = fopen(MODEM_DIR "/status", "r");
	char line[160];
	z->cell_sim[0] = z->cell_op[0] = z->cell_tech[0] = z->cell_reg[0] = 0;
	z->cell_bars = 0, z->cell_airplane = 0, z->cell_data = 0, z->wifi_bars = -1;
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *v = strchr(line, '=');
		if (!v) continue;
		*v++ = 0;
		if (!strcmp(line, "sim")) g_strlcpy(z->cell_sim, v, sizeof(z->cell_sim));
		else if (!strcmp(line, "operator")) g_strlcpy(z->cell_op, v, sizeof(z->cell_op));
		else if (!strcmp(line, "tech")) g_strlcpy(z->cell_tech, v, sizeof(z->cell_tech));
		else if (!strcmp(line, "registered")) g_strlcpy(z->cell_reg, v, sizeof(z->cell_reg));
		else if (!strcmp(line, "bars")) z->cell_bars = atoi(v);
		else if (!strcmp(line, "airplane")) z->cell_airplane = atoi(v);
		else if (!strcmp(line, "data")) z->cell_data = atoi(v);
		else if (!strcmp(line, "wifi_bars")) z->wifi_bars = atoi(v);
	}
	if (f) fclose(f);
	show_cell(z);
	show_wifi(z);
}

static gboolean on_modem(gint fd, GIOCondition c, gpointer data) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	gboolean hit = FALSE;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->len && !strcmp(e->name, "status")) hit = TRUE;
			p += sizeof(*e) + e->len;
		}
	}
	if (hit) read_modem(data);
	return G_SOURCE_CONTINUE;
}

/* ---- WireGuard: rtnetlink tells, sysfs says which ---------------------------- */

static void read_wg(Zs *z) {
	GDir *d = g_dir_open("/sys/class/net", 0, NULL);
	const char *name;
	z->wg[0] = 0;
	while (d && (name = g_dir_read_name(d))) {
		char path[300], line[128], state[16] = "";
		gboolean wg = FALSE;
		snprintf(path, sizeof(path), "/sys/class/net/%s/uevent", name);
		FILE *f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f)) {
			if (!strcmp(line, "DEVTYPE=wireguard\n")) wg = TRUE;
		}
		if (f) fclose(f);
		snprintf(path, sizeof(path), "/sys/class/net/%s/flags", name);
		read_line(path, state, sizeof(state));
		if (wg && (strtol(state, NULL, 16) & 1)) {	/* IFF_UP */
			size_t n = strlen(z->wg);
			snprintf(z->wg + n, sizeof(z->wg) - n, "%s%s", n ? " " : "", name);
		}
	}
	if (d) g_dir_close(d);
	show_vpn(z);
}

static gboolean on_rtnl(gint fd, GIOCondition c, gpointer data) {
	char buf[8192];
	while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
	}
	read_wg(data);
	return G_SOURCE_CONTINUE;
}

/* ---- Tailscale: the LocalAPI's bus, one JSON line per change ------------------ */

/* tailscaled went: on_minute looks for it again (only while seen) */
static void ts_lost(Zs *z) {
	g_clear_object(&z->ts_conn);
	if (z->ts_state != -1) {
		z->ts_state = -1;
		show_vpn(z);
	}
}

static void ts_read_more(Zs *z);

static void ts_got(GObject *src, GAsyncResult *res, gpointer data) {
	GError *err = NULL;
	GBytes *b = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
	if (!b) {
		if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) ts_lost(data);
		g_error_free(err);
		return;
	}
	Zs *z = data;
	gsize n;
	const char *p = g_bytes_get_data(b, &n);
	if (n == 0) {
		g_bytes_unref(b);
		ts_lost(z);
		return;
	}
	g_string_append_len(z->ts_buf, p, n);
	g_bytes_unref(b);
	/* the last "State":N in what came (chunked framing is left in; the
	 * key cannot be split by it once the line is whole) */
	const char *s, *last = NULL;
	for (s = z->ts_buf->str; (s = strstr(s, "\"State\":")); s++) last = s;
	if (last && last[8] >= '0' && last[8] <= '9') {
		int st = atoi(last + 8);
		if (st != z->ts_state) {
			z->ts_state = st;
			show_vpn(z);
		}
	}
	/* keep only the tail: a line in progress */
	const char *nl = strrchr(z->ts_buf->str, '\n');
	if (nl) g_string_erase(z->ts_buf, 0, nl + 1 - z->ts_buf->str);
	if (z->ts_buf->len > 65536) g_string_truncate(z->ts_buf, 0);
	ts_read_more(z);
}

static void ts_read_more(Zs *z) {
	if (!z->ts_conn) return;
	g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(z->ts_conn)),
		8192, G_PRIORITY_DEFAULT, z->cancel, ts_got, z);
}

static void ts_connected(GObject *src, GAsyncResult *res, gpointer data) {
	GError *err = NULL;
	GSocketConnection *c = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &err);
	g_object_unref(src);
	if (!c) {
		if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			Zs *z = data;
			z->ts_connecting = FALSE;
			z->ts_state = -1;
			show_vpn(z);
		}
		g_error_free(err);
		return;
	}
	Zs *z = data;
	z->ts_connecting = FALSE;
	if (!z->live) {
		/* connected just as the desktop went unseen: not kept */
		g_io_stream_close(G_IO_STREAM(c), NULL, NULL);
		g_object_unref(c);
		return;
	}
	z->ts_conn = c;
	/* mask 2|16: the state now, then on every change; no private keys */
	static const char req[] = "GET /localapi/v0/watch-ipn-bus?mask=18 HTTP/1.1\r\n"
		"Host: local-tailscaled.sock\r\nConnection: keep-alive\r\n\r\n";
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c));
	if (!g_output_stream_write_all(out, req, sizeof(req) - 1, NULL, NULL, NULL)) {
		ts_lost(z);
		return;
	}
	g_string_truncate(z->ts_buf, 0);
	ts_read_more(z);
}

static void ts_open(Zs *z) {
	if (!g_file_test(TAILSCALE_SOCK, G_FILE_TEST_EXISTS)) {
		z->ts_state = -1;
		show_vpn(z);
		return;
	}
	z->ts_connecting = TRUE;
	GSocketClient *cl = g_socket_client_new();
	GSocketAddress *a = g_unix_socket_address_new(TAILSCALE_SOCK);
	g_socket_client_connect_async(cl, G_SOCKET_CONNECTABLE(a), z->cancel, ts_connected, z);
	g_object_unref(a);
}

/* ---- NetworkManager: the Wi-Fi network ---------------------------------------- */

static GVariant *nm_get(Zs *z, const char *path, const char *iface, const char *prop) {
	GVariant *r = g_dbus_connection_call_sync(z->sys, "org.freedesktop.NetworkManager", path,
		"org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, prop),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (!r) return NULL;
	GVariant *v;
	g_variant_get(r, "(v)", &v);
	g_variant_unref(r);
	return v;
}

static void read_nm(Zs *z) {
	z->wifi_state = 1, z->wifi_ssid[0] = 0, z->wifi_ip[0] = 0, z->wifi_strength = -1;
	GVariant *en = nm_get(z, "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", "WirelessEnabled");
	if (en) {
		if (!g_variant_get_boolean(en)) z->wifi_state = 0;
		g_variant_unref(en);
	}
	GVariant *acs = z->wifi_state ? nm_get(z, "/org/freedesktop/NetworkManager",
		"org.freedesktop.NetworkManager", "ActiveConnections") : NULL;
	GVariantIter it;
	const char *ac;
	if (acs) g_variant_iter_init(&it, acs);
	while (acs && g_variant_iter_next(&it, "&o", &ac)) {
		GVariant *type = nm_get(z, ac, "org.freedesktop.NetworkManager.Connection.Active", "Type");
		gboolean wifi = type && !strcmp(g_variant_get_string(type, NULL), "802-11-wireless");
		if (type) g_variant_unref(type);
		if (!wifi) continue;
		GVariant *st = nm_get(z, ac, "org.freedesktop.NetworkManager.Connection.Active", "State");
		gboolean up = st && g_variant_get_uint32(st) == 2;	/* ACTIVATED */
		if (st) g_variant_unref(st);
		if (!up) continue;
		z->wifi_state = 2;
		GVariant *devs = nm_get(z, ac, "org.freedesktop.NetworkManager.Connection.Active", "Devices");
		const char *dev = NULL;
		if (devs && g_variant_n_children(devs)) g_variant_get_child(devs, 0, "&o", &dev);
		GVariant *ap = dev ? nm_get(z, dev, "org.freedesktop.NetworkManager.Device.Wireless", "ActiveAccessPoint") : NULL;
		const char *app = ap ? g_variant_get_string(ap, NULL) : "/";
		if (strcmp(app, "/")) {
			GVariant *ssid = nm_get(z, app, "org.freedesktop.NetworkManager.AccessPoint", "Ssid");
			if (ssid) {
				gsize n;
				const char *b = g_variant_get_fixed_array(ssid, &n, 1);
				snprintf(z->wifi_ssid, sizeof(z->wifi_ssid), "%.*s", (int)MIN(n, 32), b);
				g_variant_unref(ssid);
			}
			GVariant *s = nm_get(z, app, "org.freedesktop.NetworkManager.AccessPoint", "Strength");
			if (s) {
				z->wifi_strength = g_variant_get_byte(s);
				g_variant_unref(s);
			}
		}
		GVariant *ip4 = dev ? nm_get(z, dev, "org.freedesktop.NetworkManager.Device", "Ip4Config") : NULL;
		const char *ip4p = ip4 ? g_variant_get_string(ip4, NULL) : "/";
		GVariant *ad = strcmp(ip4p, "/") ? nm_get(z, ip4p, "org.freedesktop.NetworkManager.IP4Config", "AddressData") : NULL;
		if (ad && g_variant_n_children(ad)) {
			GVariant *first = g_variant_get_child_value(ad, 0);
			const char *addr;
			if (g_variant_lookup(first, "address", "&s", &addr))
				snprintf(z->wifi_ip, sizeof(z->wifi_ip), "%s", addr);
			g_variant_unref(first);
		}
		if (ad) g_variant_unref(ad);
		if (ip4) g_variant_unref(ip4);
		if (ap) g_variant_unref(ap);
		if (devs) g_variant_unref(devs);
		break;
	}
	if (acs) g_variant_unref(acs);
	show_wifi(z);
}

static void on_nm(GDBusConnection *c, const char *sender, const char *path, const char *iface,
		const char *sig, GVariant *params, gpointer data) {
	read_nm(data);
}

/* ---- BlueZ: read only --------------------------------------------------------- */

static void read_bt(Zs *z) {
	z->bt_on = -1, z->bt_dev[0] = 0, z->bt_batt = -1;
	GVariant *r = g_dbus_connection_call_sync(z->sys, "org.bluez", "/",
		"org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
		G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);
	if (r) {
		GVariant *objs = g_variant_get_child_value(r, 0);
		GVariantIter oi;
		const char *path;
		GVariant *ifaces;
		g_variant_iter_init(&oi, objs);
		while (g_variant_iter_next(&oi, "{&o@a{sa{sv}}}", &path, &ifaces)) {
			gboolean on, conn;
			if (g_variant_lookup(ifaces, "org.bluez.Adapter1", "@a{sv}", NULL)) {
				GVariant *ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", NULL);
				if (g_variant_lookup(ad, "Powered", "b", &on) && z->bt_on < 1) z->bt_on = on;
				g_variant_unref(ad);
			}
			GVariant *dv = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
			if (dv && g_variant_lookup(dv, "Connected", "b", &conn) && conn && !z->bt_dev[0]) {
				const char *alias;
				if (g_variant_lookup(dv, "Alias", "&s", &alias))
					g_strlcpy(z->bt_dev, alias, sizeof(z->bt_dev));
				GVariant *bv = g_variant_lookup_value(ifaces, "org.bluez.Battery1", NULL);
				guchar pct;
				if (bv && g_variant_lookup(bv, "Percentage", "y", &pct)) z->bt_batt = pct;
				if (bv) g_variant_unref(bv);
			}
			if (dv) g_variant_unref(dv);
			g_variant_unref(ifaces);
		}
		g_variant_unref(objs);
		g_variant_unref(r);
	}
	show_bt(z);
}

static void on_bz(GDBusConnection *c, const char *sender, const char *path, const char *iface,
		const char *sig, GVariant *params, gpointer data) {
	/* only what the icon shows: an adapter's, a device's or a battery's change */
	if (!strcmp(sig, "PropertiesChanged")) {
		const char *which;
		g_variant_get_child(params, 0, "&s", &which);
		if (strcmp(which, "org.bluez.Adapter1") && strcmp(which, "org.bluez.Device1") &&
		    strcmp(which, "org.bluez.Battery1")) {
			return;
		}
	}
	read_bt(data);
}

/* ---- seen or not ---------------------------------------------------------------- */

static int inotify_dir(const char *dir, uint32_t mask) {
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd >= 0 && inotify_add_watch(fd, dir, mask) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int netlink_open(int proto, unsigned groups) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = groups };
	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void close_src(guint *src, int *fd) {
	if (*src) g_source_remove(*src);
	*src = 0;
	if (fd && *fd >= 0) close(*fd);
	if (fd) *fd = -1;
}

static void unsub(Zs *z, guint *id) {
	if (*id) g_dbus_connection_signal_unsubscribe(z->sys, *id);
	*id = 0;
}

/* everything open and read afresh */
static void go_live(Zs *z) {
	if (z->live) return;
	z->live = TRUE;
	z->cancel = g_cancellable_new();
	z->modem_fd = inotify_dir(MODEM_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);
	if (z->modem_fd >= 0) z->modem_src = g_unix_fd_add(z->modem_fd, G_IO_IN, on_modem, z);
	z->rtnl_fd = netlink_open(NETLINK_ROUTE, RTMGRP_LINK);
	if (z->rtnl_fd >= 0) z->rtnl_src = g_unix_fd_add(z->rtnl_fd, G_IO_IN, on_rtnl, z);
	z->uevent_fd = netlink_open(NETLINK_KOBJECT_UEVENT, 1);
	if (z->uevent_fd >= 0) z->uevent_src = g_unix_fd_add(z->uevent_fd, G_IO_IN, on_uevent, z);
	z->minute_src = g_timeout_add_seconds(MINUTE_S, on_minute, z);
	if (!z->sys) z->sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (z->sys) {
		z->nm_sub = g_dbus_connection_signal_subscribe(z->sys, "org.freedesktop.NetworkManager",
			"org.freedesktop.DBus.Properties", "PropertiesChanged", "/org/freedesktop/NetworkManager",
			NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_nm, z, NULL);
		z->bz_sub = g_dbus_connection_signal_subscribe(z->sys, "org.bluez",
			"org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, on_bz, z, NULL);
		z->bz_add_sub = g_dbus_connection_signal_subscribe(z->sys, "org.bluez",
			"org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "/", NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, on_bz, z, NULL);
		z->bz_rm_sub = g_dbus_connection_signal_subscribe(z->sys, "org.bluez",
			"org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "/", NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, on_bz, z, NULL);
		read_nm(z);
		read_bt(z);
	}
	read_modem(z);
	read_wg(z);
	read_batt(z);
	ts_open(z);
}

/* everything closed: no wakeup until seen again */
static void go_dark(Zs *z) {
	if (!z->live) return;
	z->live = FALSE;
	close_src(&z->modem_src, &z->modem_fd);
	close_src(&z->rtnl_src, &z->rtnl_fd);
	close_src(&z->uevent_src, &z->uevent_fd);
	close_src(&z->minute_src, NULL);
	if (z->sys) {
		unsub(z, &z->nm_sub);
		unsub(z, &z->bz_sub);
		unsub(z, &z->bz_add_sub);
		unsub(z, &z->bz_rm_sub);
		/* a D-Bus connection idles in poll: kept, it costs no wakeup */
	}
	g_cancellable_cancel(z->cancel);
	g_clear_object(&z->cancel);
	z->ts_connecting = FALSE;
	if (z->ts_conn) {
		g_io_stream_close(G_IO_STREAM(z->ts_conn), NULL, NULL);
		g_clear_object(&z->ts_conn);
	}
}

static void read_viewed(Zs *z) {
	char path[300], v[8] = "1";
	snprintf(path, sizeof(path), "%s/viewed", z->view_dir);
	/* no file: a shell that doesn't say, so always seen */
	read_line(path, v, sizeof(v));
	z->viewed = v[0] != '0';
	if (z->viewed) go_live(z);
	else go_dark(z);
}

static gboolean on_view(gint fd, GIOCondition c, gpointer data) {
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
	gboolean hit = FALSE;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->len && !strcmp(e->name, "viewed")) hit = TRUE;
			p += sizeof(*e) + e->len;
		}
	}
	if (hit) read_viewed(data);
	return G_SOURCE_CONTINUE;
}

/* ---- the plugin --------------------------------------------------------------- */

static gboolean on_size(XfcePanelPlugin *plugin, gint size, Zs *z) {
	int px = xfce_panel_plugin_get_icon_size(plugin);
	for (int i = 0; i < I_COUNT; i++) gtk_image_set_pixel_size(GTK_IMAGE(z->icon[i]), px);
	return TRUE;
}

static void on_mode(XfcePanelPlugin *plugin, XfcePanelPluginMode mode, Zs *z) {
	gtk_orientable_set_orientation(GTK_ORIENTABLE(z->box),
		mode == XFCE_PANEL_PLUGIN_MODE_VERTICAL ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
}

static void on_free(XfcePanelPlugin *plugin, Zs *z) {
	go_dark(z);
	close_src(&z->view_src, &z->view_fd);
	g_clear_object(&z->sys);
	g_string_free(z->ts_buf, TRUE);
	g_free(z->view_dir);
	g_free(z);
}

static void construct(XfcePanelPlugin *plugin) {
	Zs *z = g_new0(Zs, 1);
	z->plugin = plugin;
	z->modem_fd = z->rtnl_fd = z->uevent_fd = z->view_fd = -1;
	z->ts_state = -1, z->bt_on = -1, z->batt_pct = -1, z->wifi_bars = -1, z->wifi_strength = -1;
	z->ts_buf = g_string_new(NULL);
	z->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	for (int i = 0; i < I_COUNT; i++) {
		z->ebox[i] = gtk_event_box_new();
		z->icon[i] = gtk_image_new();
		gtk_container_add(GTK_CONTAINER(z->ebox[i]), z->icon[i]);
		gtk_box_pack_start(GTK_BOX(z->box), z->ebox[i], FALSE, FALSE, 0);
		gtk_widget_show(z->icon[i]);
		gtk_widget_set_no_show_all(z->ebox[i], TRUE);
	}
	z->batt_label = gtk_label_new(NULL);
	gtk_widget_set_no_show_all(z->batt_label, TRUE);
	gtk_box_pack_start(GTK_BOX(z->box), z->batt_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(plugin), z->box);
	gtk_widget_show(z->box);
	xfce_panel_plugin_set_small(plugin, FALSE);
	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size), z);
	g_signal_connect(plugin, "mode-changed", G_CALLBACK(on_mode), z);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), z);
	on_mode(plugin, xfce_panel_plugin_get_mode(plugin), z);

	const char *rt = g_getenv("XDG_RUNTIME_DIR");
	z->view_dir = g_strdup_printf("%s/ztmetro-desktop", rt ? rt : "/tmp");
	mkdir(z->view_dir, 0700);	/* (the shell's, made early: the watch needs it) */
	z->view_fd = inotify_dir(z->view_dir, IN_MOVED_TO | IN_CLOSE_WRITE);
	if (z->view_fd >= 0) z->view_src = g_unix_fd_add(z->view_fd, G_IO_IN, on_view, z);
	read_viewed(z);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
