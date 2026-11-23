// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USBView: what is on the phone's USB, as a tree. Controllers, then their
 * root hubs, hubs and devices by port, then each device's configurations,
 * interfaces (every alternate setting) and endpoints, with the names from
 * usb.ids. Whether the USB-C port is a host or a device is at the top, and
 * the gadget controller (the port as a device) is in the tree too.
 *
 * Everything comes from sysfs (usb-tree.h) and is only read: no root, and
 * nothing written (no authorize, unbind or power control). The kernel's
 * uevents (netlink) say when something is plugged in or out, a driver binds
 * or the port changes role; the tree is read again then, and never polled.
 *
 * A row's arrow, or a configuration or interface, opens and closes it;
 * anything else opens a page with all of it, as GTK USBView's right pane.
 *
 *	ztusbview
 *	ztusbview --dump			the tree as text
 *	ztusbview --watch			and again at every change
 *	ztusbview --render tree|detail PNG [ID]	ZT_USB_SYSFS: another sysfs
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

#include "zrnmetrocore.h"
#include "usb-ids.h"

enum page { PAGE_TREE, PAGE_DETAIL };
enum action { ACT_TOGGLE = 1, ACT_OPEN };

#define ROW_H		62
#define INDENT		18
#define MAX_DEPTH	8
#define ARROW_W		26
#define GREY		0.6
#define RED		0xe51400
#define USB_IDS		"/usr/share/hwdata/usb.ids"
#define USB_IDS_OLD	"/usr/share/misc/usb.ids"
#define SETTLE_MS	250	/* after a uevent: the rest of a plug's burst */

static struct usbt tree, fresh;
static struct usb_ids ids;
static bool have_ids;
static const char *sys_root = "/sys";

/* the rows shown: tree nodes whose parents are all open */
static int rows[USBT_MAX], n_rows;
static uint64_t rows_rev;
static struct { char id[64]; bool open; } toggles[256];
static int n_toggles;

/* the node on the detail page, kept if it goes */
static char shown_id[64];
static struct usbt_node shown;
static bool shown_gone;
static int nl_fd = -1;

static bool default_open(const struct usbt_node *n)
{
	return n->kind == UT_CTRL || n->kind == UT_UDC || (n->kind == UT_DEV && n->cls == 9);
}

static bool is_open(int i)
{
	const struct usbt_node *n = &tree.nodes[i];
	for (int k = 0; k < n_toggles; k++)
		if (!strcmp(toggles[k].id, n->id))
			return toggles[k].open;
	return default_open(n);
}

static void set_open(int i, bool open)
{
	const struct usbt_node *n = &tree.nodes[i];
	int k = 0;
	while (k < n_toggles && strcmp(toggles[k].id, n->id))
		k++;
	if (k == n_toggles) {
		if (n_toggles == (int)(sizeof(toggles) / sizeof(toggles[0])))
			k = --n_toggles;	/* the last one makes room */
		n_toggles++;
		snprintf(toggles[k].id, sizeof(toggles[k].id), "%s", n->id);
	}
	toggles[k].open = open;
}

static void build_rows(void)
{
	static bool shown_row[USBT_MAX];
	n_rows = 0;
	for (int i = 0; i < tree.n; i++) {
		int p = tree.nodes[i].parent;
		shown_row[i] = p < 0 || (shown_row[p] && is_open(p));
		if (shown_row[i])
			rows[n_rows++] = i;
	}
	rows_rev++;
}

/* ---- words ---- */

static void cat(char *out, size_t size, const char *part)
{
	if (!part || !part[0])
		return;
	size_t l = strlen(out);
	snprintf(out + l, size - l, "%s%s", l ? " · " : "", part);
}

/* the drivers bound to a device's interfaces in use: "snd-usb-audio, usbhid" */
static void device_drivers(const struct usbt *t, int dev, char *out, size_t size)
{
	out[0] = 0;
	for (int i = dev + 1; i < t->n && t->nodes[i].depth > t->nodes[dev].depth; i++) {
		const struct usbt_node *n = &t->nodes[i];
		if (n->kind != UT_IFACE || t->nodes[n->parent].parent != dev || !n->active || !n->driver[0])
			continue;
		if (strstr(out, n->driver))
			continue;
		size_t l = strlen(out);
		snprintf(out + l, size - l, "%s%s", l ? ", " : "", n->driver);
	}
}

/* an active interface with no driver: drawn red, as GTK USBView does */
static bool device_unbound(const struct usbt *t, int dev)
{
	for (int i = dev + 1; i < t->n && t->nodes[i].depth > t->nodes[dev].depth; i++) {
		const struct usbt_node *n = &t->nodes[i];
		if (n->kind == UT_IFACE && t->nodes[n->parent].parent == dev && n->active && !n->driver[0])
			return true;
	}
	return false;
}

static const char *short_version(const char *v)
{
	/* "2.00" -> "2.0", "3.20" -> "3.2" */
	static char s[8];
	snprintf(s, sizeof(s), "%s", v);
	size_t l = strlen(s);
	if (l == 4 && s[3] == '0')
		s[3] = 0;
	return s;
}

static const char *device_name(const struct usbt_node *n, char *out, size_t size)
{
	if (n->root_hub)
		snprintf(out, size, "bus %d root hub", n->busnum);
	else if (n->product[0])
		snprintf(out, size, "%s", n->product);
	else if (n->product_name[0])
		snprintf(out, size, "%s", n->product_name);
	else if (n->vendor_name[0])
		snprintf(out, size, "%s device", n->vendor_name);
	else
		snprintf(out, size, "%04x:%04x", n->vid, n->pid);
	return out;
}

/* the speed node of an endpoint or interface: its device */
static const struct usbt_node *owner_device(const struct usbt *t, int i)
{
	while (i >= 0 && t->nodes[i].kind != UT_DEV)
		i = t->nodes[i].parent;
	return i >= 0 ? &t->nodes[i] : NULL;
}

/* A row's two lines; *warn: the detail in red. */
static void node_text(const struct usbt *t, int i, char *title, size_t ts, char *detail, size_t ds, bool *warn)
{
	const struct usbt_node *n = &t->nodes[i];
	char a[96], b[64];
	title[0] = detail[0] = 0;
	*warn = false;
	switch (n->kind) {
	case UT_CTRL: {
		int buses = 0;
		for (int k = i + 1; k < t->n && t->nodes[k].depth > 0; k++)
			buses += t->nodes[k].parent == i;
		snprintf(title, ts, "%s", n->name);
		cat(detail, ds, "host controller");
		cat(detail, ds, n->driver);
		snprintf(a, sizeof(a), "%d bus%s", buses, buses == 1 ? "" : "es");
		cat(detail, ds, a);
		break;
	}
	case UT_UDC:
		snprintf(title, ts, "device controller");
		cat(detail, ds, n->name);
		cat(detail, ds, n->udc_state);
		if (n->udc_function[0])
			cat(detail, ds, n->udc_function);
		break;
	case UT_DEV:
		/* what matters first, as the line is cut at the edge: the IDs, the
		 * drivers (or that one is missing), then speed, version and port */
		device_name(n, title, ts);
		snprintf(a, sizeof(a), "%04x:%04x", n->vid, n->pid);
		cat(detail, ds, a);
		device_drivers(t, i, a, sizeof(a));
		if (device_unbound(t, i)) {
			*warn = true;
			cat(detail, ds, a[0] ? "an interface without a driver" : "no driver");
		}
		cat(detail, ds, a);
		cat(detail, ds, usbt_speed_name(n->speed));
		if (n->version[0]) {
			snprintf(a, sizeof(a), "USB %s", short_version(n->version));
			cat(detail, ds, a);
		}
		if (n->cls == 9) {
			snprintf(a, sizeof(a), "%d port%s", n->maxchild, n->maxchild == 1 ? "" : "s");
			cat(detail, ds, a);
		}
		if (!n->root_hub) {
			snprintf(a, sizeof(a), "port %s", n->name);
			cat(detail, ds, a);
		}
		if (!n->root_hub && n->max_power[0] && strcmp(n->max_power, "0mA"))
			cat(detail, ds, n->max_power);
		break;
	case UT_CONF:
		snprintf(title, ts, "configuration %d", n->conf_value);
		if (n->active)
			cat(detail, ds, "in use");
		snprintf(a, sizeof(a), "%d interface%s", n->n_ifaces, n->n_ifaces == 1 ? "" : "s");
		cat(detail, ds, a);
		usbt_conf_attrs(n->conf_attr, b, sizeof(b));
		cat(detail, ds, b);
		snprintf(a, sizeof(a), "%d mA", n->conf_power_ma);
		cat(detail, ds, a);
		cat(detail, ds, n->str);
		break;
	case UT_IFACE:
		if (n->if_alt)
			snprintf(title, ts, "interface %d, setting %d", n->if_num, n->if_alt);
		else
			snprintf(title, ts, "interface %d", n->if_num);
		cat(detail, ds, n->class_name[0] ? n->class_name : "class");
		if (n->active) {
			if (n->driver[0]) {
				cat(detail, ds, n->driver);
			} else {
				cat(detail, ds, "no driver");
				*warn = true;
			}
		} else if (t->nodes[n->parent].active) {
			cat(detail, ds, "not in use");
		}
		snprintf(a, sizeof(a), "%d endpoint%s", n->n_eps, n->n_eps == 1 ? "" : "s");
		cat(detail, ds, a);
		cat(detail, ds, n->str);
		break;
	case UT_EP: {
		const struct usbt_node *d = owner_device(t, i);
		snprintf(title, ts, "endpoint 0x%02x %s", n->ep_addr, n->ep_addr & 0x80 ? "in" : "out");
		cat(detail, ds, usbt_ep_type(n->ep_attr));
		usbt_ep_packet(n, a, sizeof(a));
		cat(detail, ds, a);
		usbt_ep_interval(n, d ? d->speed : "", a, sizeof(a));
		cat(detail, ds, a);
		break;
	}
	}
}

/* "Host, supplying power", from typec or else the role switch */
static void port_role(char *out, size_t size)
{
	const char *data = tree.typec ? tree.data_role : tree.usb_role;
	out[0] = 0;
	if (!data[0]) {
		snprintf(out, size, "Not known: this kernel has no USB-C port class");
		return;
	}
	bool host = !strcmp(data, "host");
	snprintf(out, size, "%s", host ? "Host: the phone runs the bus" :
		!strcmp(data, "device") ? "Device: a computer can run the phone" : data);
	if (tree.typec && tree.power_role[0])
		snprintf(out + strlen(out), size - strlen(out), ", %s",
			!strcmp(tree.power_role, "source") ? "giving power" : "taking power");
	snprintf(out + strlen(out), size - strlen(out), "%s",
		tree.partner ? ". Something is plugged in." : tree.typec ? ". Nothing is plugged in." : ".");
}

/* ---- reading ---- */

static void reread(void)
{
	usbt_read(&fresh, sys_root);
	usbt_names(&fresh, have_ids ? &ids : NULL);
	if (!memcmp(&fresh, &tree, sizeof(tree)))
		return;
	zmc_list_lock(true);
	memcpy(&tree, &fresh, sizeof(tree));
	build_rows();
	zmc_list_lock(false);
	if (shown_id[0]) {
		int i = usbt_find(&tree, shown_id);
		shown_gone = i < 0;
		if (i >= 0)
			shown = tree.nodes[i];
	}
	zmc_redraw();
}

static void settled(void)
{
	zmc_every(0, NULL);
	reread();
}

/* The kernel's uevents: USB, the port's role, the gadget, and the charger
 * (a cable in or out changes the gadget's state without a USB event). */
static bool uevents_wanted(int fd)
{
	char buf[8192];
	bool want = false;
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) != 0) {
		if (n < 0) {
			if (errno == ENOBUFS)
				want = true;	/* some were lost: read it all again */
			if (errno == EINTR || errno == ENOBUFS)
				continue;
			break;
		}
		buf[n] = 0;
		bool power = false, battery = false;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strncmp(p, "SUBSYSTEM=", 10)) {
				const char *s = p + 10;
				want = want || !strcmp(s, "usb") || !strcmp(s, "typec") || !strcmp(s, "usb_role") ||
					!strcmp(s, "udc");
				power = !strcmp(s, "power_supply");
			} else if (!strncmp(p, "POWER_SUPPLY_NAME=", 18)) {
				battery = strstr(p, "battery") != NULL;
			}
		}
		want = want || (power && !battery);
	}
	return want;
}

static void uevent(int fd, void *data)
{
	if (uevents_wanted(fd))
		zmc_every(SETTLE_MS, settled);
}

static bool open_uevents(void)
{
	nl_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (nl_fd < 0)
		return false;
	int size = 256 * 1024;
	setsockopt(nl_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };	/* the kernel's own */
	if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(nl_fd);
		nl_fd = -1;
		return false;
	}
	return true;
}

/* --watch: the tree as text again at every change, with no window */
static int watch_text(void)
{
	if (!open_uevents()) {
		perror("ztusbview: uevents");
		return 1;
	}
	usbt_dump(&tree, stdout);
	fflush(stdout);
	for (;;) {
		struct pollfd p = { .fd = nl_fd, .events = POLLIN };
		if (poll(&p, 1, -1) < 0 && errno != EINTR)
			return 1;
		if (!uevents_wanted(nl_fd))
			continue;
		/* the rest of the burst */
		while (poll(&p, 1, SETTLE_MS) > 0)
			uevents_wanted(nl_fd);
		usbt_read(&fresh, sys_root);
		usbt_names(&fresh, have_ids ? &ids : NULL);
		if (!memcmp(&fresh, &tree, sizeof(tree)))
			continue;
		memcpy(&tree, &fresh, sizeof(tree));
		time_t now = time(NULL);
		printf("--- changed %s", ctime(&now));
		usbt_dump(&tree, stdout);
		fflush(stdout);
	}
}

/* ---- the tree ---- */

static double row_x(int i)
{
	int d = tree.nodes[i].depth;
	return ZMC_MARGIN + (d > MAX_DEPTH ? MAX_DEPTH : d) * INDENT;
}

static void arrow(cairo_t *cr, double x, double y, bool open)
{
	zmc_fg(cr);
	if (open) {
		cairo_move_to(cr, x + 4, y + 9);
		cairo_line_to(cr, x + 18, y + 9);
		cairo_line_to(cr, x + 11, y + 17);
	} else {
		cairo_move_to(cr, x + 7, y + 6);
		cairo_line_to(cr, x + 15, y + 13);
		cairo_line_to(cr, x + 7, y + 20);
	}
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void draw_row(cairo_t *cr, int r)
{
	if (r >= n_rows)
		return;
	int i = rows[r];
	char title[128], detail[256];
	bool warn;
	node_text(&tree, i, title, sizeof(title), detail, sizeof(detail), &warn);
	double x = row_x(i), w = zmc.width - x - ARROW_W - ZMC_MARGIN;
	if (usbt_has_children(&tree, i))
		arrow(cr, x, 6, is_open(i));
	const struct usbt_node *n = &tree.nodes[i];
	bool dim = (n->kind == UT_IFACE || n->kind == UT_EP || n->kind == UT_CONF) && !n->active;
	if (dim)
		cairo_set_source_rgb(cr, GREY, GREY, GREY);
	else
		zmc_fg(cr);
	double size = n->kind == UT_CTRL || n->kind == UT_UDC ? 21 : n->kind == UT_DEV ? 20 : 18;
	zmc_text_line(cr, title, FONT_SEMILIGHT, size, x + ARROW_W, 2, w);
	if (warn)
		zmc_set_rgb(cr, RED);
	else
		cairo_set_source_rgb(cr, GREY, GREY, GREY);
	zmc_text_line(cr, detail, FONT_REGULAR, 12.5, x + ARROW_W, 32, w);
}

static bool row_hit(int r, double x, double y, struct zmc_hit *hit)
{
	if (r >= n_rows)
		return false;
	int i = rows[r];
	const struct usbt_node *n = &tree.nodes[i];
	bool kids = usbt_has_children(&tree, i);
	double x0 = row_x(i);
	if (kids && (x < x0 + ARROW_W + 14 || n->kind == UT_CONF || n->kind == UT_IFACE)) {
		/* the arrow: pressed alone, so the row does not tilt as if it opened */
		*hit = (struct zmc_hit){ .x = x < x0 + ARROW_W + 14 ? x0 - 8 : 0, .y = 0,
			.w = x < x0 + ARROW_W + 14 ? ARROW_W + 22 : zmc.width, .h = ROW_H - 4,
			.action = ACT_TOGGLE, .arg = i };
		return true;
	}
	*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = zmc.width, .h = ROW_H - 4, .action = ACT_OPEN, .arg = i };
	return true;
}

static double note(cairo_t *cr, double y, const char *text)
{
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
	return y + zmc_text(cr, text, FONT_REGULAR, 13, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 12;
}

static double render_tree(cairo_t *cr)
{
	double y = zmc_header(cr, "USB devices");
	char role[160];
	port_role(role, sizeof(role));
	y = zmc_field(cr, y, "USB-C port", role);
	bool host = false;
	for (int i = 0; i < tree.n; i++)
		host = host || tree.nodes[i].kind == UT_CTRL;
	if (!host)
		y = note(cr, y, "No host controller is running, so no devices are listed. Plug in a hub, "
			"a USB-C adapter or EarPods and the port turns host.");
	y += 4;
	struct zmc_list list = { .count = n_rows, .row_h = ROW_H, .revision = rows_rev,
		.draw_row = draw_row, .row_hit = row_hit };
	y = zmc_list(cr, y, &list);
	if (!have_ids)
		y = note(cr, y + 8, "usb.ids is not installed (hwdata): names are only what the devices say.");
	return y + 24;
}

/* ---- a page with all of one node ---- */

static double field(cairo_t *cr, double y, const char *caption, const char *value)
{
	return value && value[0] ? zmc_field(cr, y, caption, value) : y;
}

static double section(cairo_t *cr, double y, const char *title)
{
	zmc_fg(cr);
	zmc_text(cr, title, FONT_SEMILIGHT, 22, ZMC_MARGIN, y + 6, 0);
	return y + 44;
}

static double render_device(cairo_t *cr, double y, const struct usbt_node *n, int i)
{
	char v[192], a[64];
	y = field(cr, y, "Manufacturer", n->manufacturer);
	y = field(cr, y, "Product", n->product);
	y = field(cr, y, "Serial number", n->serial);
	snprintf(v, sizeof(v), "%04x%s%s", n->vid, n->vendor_name[0] ? "  " : "", n->vendor_name);
	y = field(cr, y, "Vendor", v);
	snprintf(v, sizeof(v), "%04x%s%s", n->pid, n->product_name[0] ? "  " : "", n->product_name);
	y = field(cr, y, "Product ID", v);
	usbt_bcd(n->bcd_device, a, sizeof(a));
	y = field(cr, y, "Revision", a);
	y = field(cr, y, "USB version", n->version);
	usbt_speed_rate(n->speed, a, sizeof(a));
	snprintf(v, sizeof(v), "%s%s%s%s", usbt_speed_name(n->speed), a[0] ? " (" : "", a, a[0] ? ")" : "");
	y = field(cr, y, "Speed", v);
	snprintf(v, sizeof(v), "%s%s%02x / %02x / %02x", n->class_name, n->class_name[0] ? "  " : "", n->cls, n->sub,
		n->proto);
	y = field(cr, y, "Class / subclass / protocol", v);
	snprintf(v, sizeof(v), "%s%s", n->name, n->root_hub ? " (the bus itself)" : "");
	y = field(cr, y, "Port path", v);
	snprintf(v, sizeof(v), "bus %d, address %d", n->busnum, n->devnum);
	y = field(cr, y, "Bus", v);
	if (i >= 0) {
		device_drivers(&tree, i, a, sizeof(a));
		snprintf(v, sizeof(v), "%s%s%s", n->driver, a[0] ? "; interfaces: " : "", a);
	} else {
		snprintf(v, sizeof(v), "%s", n->driver);
	}
	y = field(cr, y, "Driver", v[0] ? v : "None");
	y = field(cr, y, "Max power (bMaxPower)", n->max_power);
	if (n->cls == 9) {
		snprintf(v, sizeof(v), "%d", n->maxchild);
		y = field(cr, y, "Ports", v);
	}
	snprintf(v, sizeof(v), "%d%s", n->nconfigs, n->active_config ? "" : ", none in use");
	if (n->active_config)
		snprintf(v + strlen(v), sizeof(v) - strlen(v), ", number %d in use", n->active_config);
	y = field(cr, y, "Configurations", v);
	if (n->maxpacket0) {
		snprintf(v, sizeof(v), "%d bytes", n->maxpacket0);
		y = field(cr, y, "Endpoint 0 packet size", v);
	}
	y = field(cr, y, "Removable", strcmp(n->removable, "unknown") ? n->removable : "");
	if (i < 0)
		return y;
	/* what is in it: the interfaces in use */
	bool any = false;
	for (int k = i + 1; k < tree.n && tree.nodes[k].depth > n->depth; k++) {
		const struct usbt_node *c = &tree.nodes[k];
		if (c->kind != UT_IFACE || !c->active || tree.nodes[c->parent].parent != i)
			continue;
		if (!any)
			y = section(cr, y, "interfaces");
		any = true;
		char t[128], d[256];
		bool warn;
		node_text(&tree, k, t, sizeof(t), d, sizeof(d), &warn);
		zmc_fg(cr);
		zmc_text_line(cr, t, FONT_SEMILIGHT, 18, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
		if (warn)
			zmc_set_rgb(cr, RED);
		else
			cairo_set_source_rgb(cr, GREY, GREY, GREY);
		y += 28 + zmc_text(cr, d, FONT_REGULAR, 12.5, ZMC_MARGIN, y + 26, zmc.width - 2 * ZMC_MARGIN);
		for (int e = k + 1; e < tree.n && tree.nodes[e].parent == k; e++) {
			node_text(&tree, e, t, sizeof(t), d, sizeof(d), &warn);
			char line[320];
			snprintf(line, sizeof(line), "%s: %s", t, d);
			cairo_set_source_rgb(cr, GREY, GREY, GREY);
			zmc_text_line(cr, line, FONT_REGULAR, 12.5, ZMC_MARGIN + 16, y, zmc.width - 2 * ZMC_MARGIN - 16);
			y += 20;
		}
		y += 10;
	}
	return y;
}

static double render_detail(cairo_t *cr)
{
	const struct usbt_node *n = &shown;
	int i = shown_gone ? -1 : usbt_find(&tree, shown_id);
	char title[128], v[192], a[64];
	if (n->kind == UT_DEV)
		device_name(n, title, sizeof(title));
	else if (n->kind == UT_EP)
		snprintf(title, sizeof(title), "endpoint 0x%02x", n->ep_addr);	/* fits the title */
	else if (i >= 0) {
		char d[256];
		bool warn;
		node_text(&tree, i, title, sizeof(title), d, sizeof(d), &warn);
	} else
		snprintf(title, sizeof(title), "%s", n->name);
	double y = zmc_header(cr, title);
	if (shown_gone) {
		zmc_set_rgb(cr, RED);
		y += 12 + zmc_text(cr, "Unplugged. This is what it was.", FONT_REGULAR, 14, ZMC_MARGIN, y,
			zmc.width - 2 * ZMC_MARGIN);
	}
	switch (n->kind) {
	case UT_DEV:
		y = render_device(cr, y, n, i);
		break;
	case UT_CTRL:
		y = field(cr, y, "Controller", n->name);
		y = field(cr, y, "Driver", n->driver[0] ? n->driver : "None");
		for (int k = i + 1; i >= 0 && k < tree.n && tree.nodes[k].depth > 0; k++) {
			const struct usbt_node *b = &tree.nodes[k];
			if (b->parent != i)
				continue;
			snprintf(title, sizeof(title), "Bus %d", b->busnum);
			usbt_speed_rate(b->speed, a, sizeof(a));
			snprintf(v, sizeof(v), "USB %s, %s (%s), %d port%s", short_version(b->version),
				usbt_speed_name(b->speed), a, b->maxchild, b->maxchild == 1 ? "" : "s");
			y = field(cr, y, title, v);
		}
		break;
	case UT_UDC:
		y = field(cr, y, "Controller", n->name);
		y = field(cr, y, "State", n->udc_state);
		y = field(cr, y, "Speed now", n->udc_speed);
		y = field(cr, y, "Fastest", n->udc_max_speed);
		y = field(cr, y, "Gadget", n->udc_function[0] ? n->udc_function : "None bound");
		y = field(cr, y, "Driver", n->driver);
		y = note(cr, y, "The port as a device: what a computer sees when the phone is plugged into it.");
		break;
	case UT_CONF:
		snprintf(v, sizeof(v), "%d%s", n->conf_value, n->active ? ", in use" : "");
		y = field(cr, y, "Configuration", v);
		y = field(cr, y, "Name", n->str);
		snprintf(v, sizeof(v), "%d", n->n_ifaces);
		y = field(cr, y, "Interfaces", v);
		usbt_conf_attrs(n->conf_attr, a, sizeof(a));
		snprintf(v, sizeof(v), "%s (0x%02x)", a, n->conf_attr);
		y = field(cr, y, "Attributes", v);
		snprintf(v, sizeof(v), "%d mA", n->conf_power_ma);
		y = field(cr, y, "Max power", v);
		break;
	case UT_IFACE:
		snprintf(v, sizeof(v), "%d, alternate setting %d%s", n->if_num, n->if_alt, n->active ? ", in use" : "");
		y = field(cr, y, "Interface", v);
		y = field(cr, y, "Name", n->str);
		snprintf(v, sizeof(v), "%s%s%02x / %02x / %02x", n->class_name, n->class_name[0] ? "  " : "", n->cls,
			n->sub, n->proto);
		y = field(cr, y, "Class / subclass / protocol", v);
		y = field(cr, y, "Driver", n->driver[0] ? n->driver : "None");
		snprintf(v, sizeof(v), "%d", n->n_eps);
		y = field(cr, y, "Endpoints", v);
		y = field(cr, y, "sysfs", n->name);
		break;
	case UT_EP: {
		const struct usbt_node *d = i >= 0 ? owner_device(&tree, i) : NULL;
		snprintf(v, sizeof(v), "0x%02x, number %d, %s", n->ep_addr, n->ep_addr & 0x0f,
			n->ep_addr & 0x80 ? "in (to the phone)" : "out (from the phone)");
		y = field(cr, y, "Address", v);
		snprintf(v, sizeof(v), "%s (0x%02x)", usbt_ep_type(n->ep_attr), n->ep_attr);
		y = field(cr, y, "Transfer type", v);
		usbt_ep_packet(n, a, sizeof(a));
		y = field(cr, y, "Max packet size", a);
		usbt_ep_interval(n, d ? d->speed : "", a, sizeof(a));
		snprintf(v, sizeof(v), "%d%s%s", n->ep_interval, a[0] ? ": " : "", a);
		y = field(cr, y, "Interval", v);
		break;
	}
	}
	return y + 30;
}

static double render(cairo_t *cr, int page)
{
	return page == PAGE_DETAIL ? render_detail(cr) : render_tree(cr);
}

static void open_node(int i)
{
	snprintf(shown_id, sizeof(shown_id), "%s", tree.nodes[i].id);
	shown = tree.nodes[i];
	shown_gone = false;
}

static void activate(int action, int arg)
{
	if (arg < 0 || arg >= tree.n)
		return;
	if (action == ACT_TOGGLE) {
		zmc_list_lock(true);
		set_open(arg, !is_open(arg));
		build_rows();
		zmc_list_lock(false);
	} else if (action == ACT_OPEN) {
		open_node(arg);
		zmc_go(PAGE_DETAIL, 1);
	}
}

static bool back(void)
{
	if (zmc.page == PAGE_DETAIL) {
		zmc_go(PAGE_TREE, -1);
		return true;
	}
	return false;
}

static const struct zmc_app app = {
	.app_id = "ztusbview", .title = "USBView", .caps = "USBVIEW",
	.render = render, .activate = activate, .back = back,
};

int main(int argc, char **argv)
{
	if (getenv("ZT_USB_SYSFS"))
		sys_root = getenv("ZT_USB_SYSFS");
	const char *idp = getenv("ZT_USB_IDS");
	have_ids = usb_ids_load(&ids, idp ? idp : USB_IDS) || (!idp && usb_ids_load(&ids, USB_IDS_OLD));
	usbt_read(&tree, sys_root);
	usbt_names(&tree, have_ids ? &ids : NULL);
	build_rows();
	if (argc == 2 && !strcmp(argv[1], "--dump")) {
		usbt_dump(&tree, stdout);
		usb_ids_free(&ids);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--watch"))
		return watch_text();
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "--render")) {
		const char *scale = getenv("ZTMETRO_SCALE");
		if (scale && atof(scale) >= 1)
			zmc.scale = atof(scale) > 4 ? 4 : (int)(atof(scale) + 0.99);
		bool detail = !strcmp(argv[2], "detail");
		if (detail) {
			int i = argc == 5 ? usbt_find(&tree, argv[4]) : -1;
			for (int k = 0; i < 0 && k < tree.n; k++)
				if (tree.nodes[k].kind == UT_DEV && !tree.nodes[k].root_hub)
					i = k;	/* the first device, by default */
			if (i < 0)
				i = 0;
			if (tree.n)
				open_node(i);
		}
		return zmc_render_png(&app, detail ? PAGE_DETAIL : PAGE_TREE, 1500, argv[3]);
	}
	if (argc > 1) {
		fprintf(stderr, "usage: ztusbview [--dump | --watch | --render tree|detail PNG [ID]]\n");
		return 2;
	}
	if (open_uevents())
		zmc_watch_fd(nl_fd, uevent, NULL);
	return zmc_run(&app, PAGE_TREE);
}
