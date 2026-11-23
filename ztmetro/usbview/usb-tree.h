// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The USB tree as sysfs has it, for USBView: read only, and no root needed.
 *
 *	controller		the root hub's parent device (xhci-hcd.0.auto)
 *	  device		usbN (the root hub), then B-P.P.P by port path
 *	    configuration	from the device's descriptors file
 *	      interface		one per alternate setting; driver from B-P:C.I
 *	        endpoint
 *	gadget			a UDC: the port as a device (class/udc)
 *
 * Nodes are kept in the order they are drawn (depth first), each with its
 * parent and depth. usbt_read(root) reads a tree under `root` ("/sys", or a
 * fake one for the tests); usbt_dump writes it as text, one node a line.
 * Nothing here writes anything under sysfs.
 */
#ifndef USB_TREE_H
#define USB_TREE_H

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USBT_MAX	512
#define USBT_MAX_DEVS	128

enum usbt_kind { UT_CTRL, UT_UDC, UT_DEV, UT_CONF, UT_IFACE, UT_EP };

struct usbt_node {
	enum usbt_kind kind;
	int parent, depth;
	char id[64];		/* stable across reads: what stays expanded */
	char name[48];		/* the sysfs name: usb1, 1-1.2, xhci-hcd.0.auto */
	char driver[32];	/* bound driver ("" none); a controller's, an interface's */
	char str[96];		/* a configuration's or interface's own string */
	/* a device */
	unsigned vid, pid, bcd_device, bcd_usb;
	char manufacturer[96], product[96], serial[64];
	char speed[12];		/* Mbit/s as sysfs has it: 1.5 12 480 5000 10000 20000 */
	char version[8];	/* "2.00" */
	int cls, sub, proto;	/* also an interface's */
	char max_power[16];	/* the active configuration's bMaxPower: "500mA" */
	int busnum, devnum, maxchild, nconfigs, active_config, maxpacket0;
	char removable[16];
	bool root_hub;
	/* a configuration */
	int conf_value, conf_attr, conf_power_ma, n_ifaces;
	/* an interface (an alternate setting of one) */
	int if_num, if_alt, n_eps;
	bool active;		/* this configuration / setting is the one in use */
	/* an endpoint */
	int ep_addr, ep_attr, ep_maxp, ep_interval;
	/* a gadget (UDC) */
	char udc_state[32], udc_speed[24], udc_max_speed[24], udc_function[48];
	/* names from usb.ids, filled by usbt_names (usb-ids.h) */
	char vendor_name[96], product_name[96], class_name[96];
};

struct usbt {
	int n;
	struct usbt_node nodes[USBT_MAX];
	/* the USB-C port: class/typec and class/usb_role */
	bool typec;
	char typec_port[24], data_role[16], power_role[16], usb_role[16];
	bool partner;
	char partner_type[32];
};

static void usbt_read_attr(const char *dir, const char *attr, char *out, size_t size)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	out[0] = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	ssize_t n = read(fd, out, size - 1);
	close(fd);
	if (n < 0)
		n = 0;
	out[n] = 0;
	out[strcspn(out, "\n")] = 0;
	/* "version" is " 2.00": no spaces either side */
	char *s = out;
	while (*s == ' ')
		s++;
	if (s != out)
		memmove(out, s, strlen(s) + 1);
	size_t l = strlen(out);
	while (l && out[l - 1] == ' ')
		out[--l] = 0;
}

static long usbt_attr_num(const char *dir, const char *attr, int base, long dflt)
{
	char v[32];
	usbt_read_attr(dir, attr, v, sizeof(v));
	if (!v[0])
		return dflt;
	return strtol(v, NULL, base);
}

/* the last part of a symlink's target: a bound driver's name */
static void usbt_link_name(const char *dir, const char *link, char *out, size_t size)
{
	char path[PATH_MAX], target[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, link);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	out[0] = 0;
	if (n <= 0)
		return;
	target[n] = 0;
	const char *b = strrchr(target, '/');
	snprintf(out, size, "%s", b ? b + 1 : target);
}

/* "host [device]" -> "device" */
static void usbt_bracketed(char *v)
{
	char *o = strchr(v, '['), *c = o ? strchr(o, ']') : NULL;
	if (o && c) {
		*c = 0;
		memmove(v, o + 1, strlen(o + 1) + 1);
	}
}

static struct usbt_node *usbt_add(struct usbt *t, enum usbt_kind kind, int parent)
{
	if (t->n >= USBT_MAX)
		return NULL;
	struct usbt_node *n = &t->nodes[t->n++];
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	n->parent = parent;
	n->depth = parent >= 0 ? t->nodes[parent].depth + 1 : 0;
	return n;
}

/* the parent of a device by its name: 1-1.2 -> 1-1, 1-1 -> usb1 */
static void usbt_parent_name(const char *name, char *out, size_t size)
{
	out[0] = 0;
	if (!strncmp(name, "usb", 3))
		return;
	const char *dot = strrchr(name, '.'), *dash = strchr(name, '-');
	if (dot)
		snprintf(out, size, "%.*s", (int)(dot - name), name);
	else if (dash)
		snprintf(out, size, "usb%.*s", (int)(dash - name), name);
}

/* order by bus, then port by port as numbers: 1-1.2 before 1-1.10 */
static int usbt_name_cmp(const void *a, const void *b)
{
	const char *x = *(const char *const *)a, *y = *(const char *const *)b;
	bool rx = !strncmp(x, "usb", 3), ry = !strncmp(y, "usb", 3);
	if (rx != ry)
		return rx ? -1 : 1;
	if (rx)
		x += 3, y += 3;
	while (*x && *y) {
		if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
			char *ex, *ey;
			long nx = strtol(x, &ex, 10), ny = strtol(y, &ey, 10);
			if (nx != ny)
				return nx < ny ? -1 : 1;
			x = ex, y = ey;
		} else {
			if (*x != *y)
				return (unsigned char)*x - (unsigned char)*y;
			x++, y++;
		}
	}
	return (unsigned char)*x - (unsigned char)*y;
}

static int usbt_str_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static unsigned usbt_le16(const unsigned char *p)
{
	return p[0] | (unsigned)p[1] << 8;
}

/* Configurations, interfaces and endpoints from the descriptors file: the
 * device descriptor (18 bytes), then each configuration whole. */
static void usbt_descriptors(struct usbt *t, int dev, const char *dir)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/descriptors", dir);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	static unsigned char buf[65536];
	size_t len = 0;
	ssize_t r;
	while (len < sizeof(buf) && (r = read(fd, buf + len, sizeof(buf) - len)) > 0)
		len += (size_t)r;
	close(fd);
	if (len < 18 || buf[1] != 1)
		return;
	struct usbt_node *d = &t->nodes[dev];
	d->bcd_usb = usbt_le16(buf + 2);
	d->maxpacket0 = buf[7];
	if (!d->nconfigs)
		d->nconfigs = buf[17];
	/* a root hub's interfaces are B-0:C.I */
	char name[48];
	if (d->root_hub)
		snprintf(name, sizeof(name), "%d-0", d->busnum);
	else
		snprintf(name, sizeof(name), "%s", d->name);
	bool super = atof(d->speed) >= 5000;
	size_t off = 18;
	while (off + 9 <= len) {
		const unsigned char *c = buf + off;
		if (c[0] < 2) {
			break;
		}
		if (c[1] != 2) {	/* not a configuration: skip what it is */
			off += c[0];
			continue;
		}
		size_t total = usbt_le16(c + 2);
		if (total < 9 || off + total > len)
			total = len - off;
		int ci = t->n;
		struct usbt_node *cn = usbt_add(t, UT_CONF, dev);
		if (!cn)
			return;
		cn->conf_value = c[5];
		cn->conf_attr = c[7];
		cn->conf_power_ma = c[8] * (super ? 8 : 2);
		cn->n_ifaces = c[4];
		cn->active = cn->conf_value == t->nodes[dev].active_config;
		snprintf(cn->id, sizeof(cn->id), "%s#c%d", name, cn->conf_value);
		snprintf(cn->name, sizeof(cn->name), "%s", name);
		if (cn->active)
			usbt_read_attr(dir, "configuration", cn->str, sizeof(cn->str));
		int ii = -1;
		for (size_t p = c[0]; p + 2 <= total;) {
			const unsigned char *e = c + p;
			if (e[0] < 2 || p + e[0] > total)
				break;
			if (e[1] == 4 && e[0] >= 9) {
				ii = t->n;
				struct usbt_node *in = usbt_add(t, UT_IFACE, ci);
				if (!in)
					return;
				in->if_num = e[2];
				in->if_alt = e[3];
				in->n_eps = e[4];
				in->cls = e[5];
				in->sub = e[6];
				in->proto = e[7];
				snprintf(in->id, sizeof(in->id), "%s#c%di%da%d", name, cn->conf_value, in->if_num, in->if_alt);
				snprintf(in->name, sizeof(in->name), "%s:%d.%d", name, cn->conf_value, in->if_num);
				if (t->nodes[ci].active) {
					char idir[PATH_MAX];
					snprintf(idir, sizeof(idir), "%s/%s", dir, in->name);
					if (access(idir, F_OK) == 0) {
						/* the setting in use is the one sysfs shows */
						in->active = usbt_attr_num(idir, "bAlternateSetting", 16, 0) == in->if_alt;
						if (in->active) {
							usbt_link_name(idir, "driver", in->driver, sizeof(in->driver));
							usbt_read_attr(idir, "interface", in->str, sizeof(in->str));
						}
					}
				}
			} else if (e[1] == 5 && e[0] >= 7 && ii >= 0) {
				struct usbt_node *en = usbt_add(t, UT_EP, ii);
				if (!en)
					return;
				en->ep_addr = e[2];
				en->ep_attr = e[3];
				en->ep_maxp = (int)usbt_le16(e + 4);
				en->ep_interval = e[6];
				en->active = t->nodes[ii].active;
				snprintf(en->id, sizeof(en->id), "%s#e%02x", t->nodes[ii].id, en->ep_addr);
				snprintf(en->name, sizeof(en->name), "%s", t->nodes[ii].name);
			}
			p += e[0];
		}
		off += total;
	}
}

static char *usbt_devs[USBT_MAX_DEVS];
static int usbt_n_devs;

static void usbt_add_device(struct usbt *t, const char *base, const char *name, int parent)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", base, name);
	int di = t->n;
	struct usbt_node *d = usbt_add(t, UT_DEV, parent);
	if (!d)
		return;
	snprintf(d->name, sizeof(d->name), "%s", name);
	snprintf(d->id, sizeof(d->id), "%s", name);
	d->vid = (unsigned)usbt_attr_num(dir, "idVendor", 16, 0);
	d->pid = (unsigned)usbt_attr_num(dir, "idProduct", 16, 0);
	d->bcd_device = (unsigned)usbt_attr_num(dir, "bcdDevice", 16, 0);
	usbt_read_attr(dir, "manufacturer", d->manufacturer, sizeof(d->manufacturer));
	usbt_read_attr(dir, "product", d->product, sizeof(d->product));
	usbt_read_attr(dir, "serial", d->serial, sizeof(d->serial));
	usbt_read_attr(dir, "speed", d->speed, sizeof(d->speed));
	usbt_read_attr(dir, "version", d->version, sizeof(d->version));
	usbt_read_attr(dir, "bMaxPower", d->max_power, sizeof(d->max_power));
	usbt_read_attr(dir, "removable", d->removable, sizeof(d->removable));
	d->cls = (int)usbt_attr_num(dir, "bDeviceClass", 16, 0);
	d->sub = (int)usbt_attr_num(dir, "bDeviceSubClass", 16, 0);
	d->proto = (int)usbt_attr_num(dir, "bDeviceProtocol", 16, 0);
	d->busnum = (int)usbt_attr_num(dir, "busnum", 10, 0);
	d->devnum = (int)usbt_attr_num(dir, "devnum", 10, 0);
	d->maxchild = (int)usbt_attr_num(dir, "maxchild", 10, 0);
	d->nconfigs = (int)usbt_attr_num(dir, "bNumConfigurations", 10, 0);
	d->active_config = (int)usbt_attr_num(dir, "bConfigurationValue", 10, 0);
	d->root_hub = !strncmp(name, "usb", 3);
	usbt_link_name(dir, "driver", d->driver, sizeof(d->driver));
	usbt_descriptors(t, di, dir);
	/* then the devices on its ports */
	for (int i = 0; i < usbt_n_devs; i++) {
		char p[48];
		usbt_parent_name(usbt_devs[i], p, sizeof(p));
		if (!strcmp(p, name))
			usbt_add_device(t, base, usbt_devs[i], di);
	}
}

/* A root hub's controller: the device its sysfs directory is in. */
static void usbt_controller_of(const char *base, const char *rh, char *dir, size_t size)
{
	char path[PATH_MAX], real[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/..", base, rh);
	if (!realpath(path, real))
		snprintf(real, sizeof(real), "%s", "?");
	snprintf(dir, size, "%s", real);
}

static void usbt_read_role(struct usbt *t, const char *root)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/class/typec", root);
	DIR *d = opendir(dir);
	struct dirent *e;
	char partner[64] = "";
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "port", 4))
			continue;
		if (strstr(e->d_name, "-partner")) {
			t->partner = true;
			snprintf(partner, sizeof(partner), "%s", e->d_name);
			continue;
		}
		if (strchr(e->d_name, '-') || (t->typec && strcmp(e->d_name, t->typec_port) > 0))
			continue;	/* the first port (cables and plugs aside) */
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		t->typec = true;
		snprintf(t->typec_port, sizeof(t->typec_port), "%s", e->d_name);
		usbt_read_attr(p, "data_role", t->data_role, sizeof(t->data_role));
		usbt_read_attr(p, "power_role", t->power_role, sizeof(t->power_role));
		usbt_bracketed(t->data_role);
		usbt_bracketed(t->power_role);
	}
	if (d)
		closedir(d);
	if (partner[0]) {
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", dir, partner);
		usbt_read_attr(p, "type", t->partner_type, sizeof(t->partner_type));
	}
	snprintf(dir, sizeof(dir), "%s/class/usb_role", root);
	d = opendir(dir);
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		usbt_read_attr(p, "role", t->usb_role, sizeof(t->usb_role));
		break;
	}
	if (d)
		closedir(d);
}

/* Read the whole tree under `root` (normally "/sys"). */
static void usbt_read(struct usbt *t, const char *root)
{
	memset(t, 0, sizeof(*t));
	usbt_read_role(t, root);
	char base[PATH_MAX];
	snprintf(base, sizeof(base), "%s/bus/usb/devices", root);
	for (int i = 0; i < usbt_n_devs; i++)
		free(usbt_devs[i]);
	usbt_n_devs = 0;
	char *roots[32];
	int n_roots = 0;
	DIR *d = opendir(base);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
			continue;
		if (!strncmp(e->d_name, "usb", 3)) {
			if (n_roots < 32)
				roots[n_roots++] = strdup(e->d_name);
		} else if (usbt_n_devs < USBT_MAX_DEVS) {
			usbt_devs[usbt_n_devs++] = strdup(e->d_name);
		}
	}
	if (d)
		closedir(d);
	qsort(roots, (size_t)n_roots, sizeof(roots[0]), usbt_name_cmp);
	qsort(usbt_devs, (size_t)usbt_n_devs, sizeof(usbt_devs[0]), usbt_name_cmp);
	/* the controllers, each once, in the order of their first bus */
	bool done[32] = { false };
	for (int i = 0; i < n_roots; i++) {
		if (done[i])
			continue;
		char cdir[PATH_MAX];
		usbt_controller_of(base, roots[i], cdir, sizeof(cdir));
		int ci = t->n;
		struct usbt_node *c = usbt_add(t, UT_CTRL, -1);
		if (!c)
			break;
		const char *b = strrchr(cdir, '/');
		snprintf(c->name, sizeof(c->name), "%s", b ? b + 1 : cdir);
		snprintf(c->id, sizeof(c->id), "ctrl:%s", c->name);
		usbt_link_name(cdir, "driver", c->driver, sizeof(c->driver));
		for (int j = i; j < n_roots; j++) {
			char other[PATH_MAX];
			if (j != i)
				usbt_controller_of(base, roots[j], other, sizeof(other));
			if (j == i || !strcmp(other, cdir)) {
				done[j] = true;
				usbt_add_device(t, base, roots[j], ci);
			}
		}
	}
	for (int i = 0; i < n_roots; i++)
		free(roots[i]);
	/* the port as a device: each UDC */
	snprintf(base, sizeof(base), "%s/class/udc", root);
	d = opendir(base);
	char *udcs[8];
	int n_udcs = 0;
	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.' && n_udcs < 8)
			udcs[n_udcs++] = strdup(e->d_name);
	if (d)
		closedir(d);
	qsort(udcs, (size_t)n_udcs, sizeof(udcs[0]), usbt_str_cmp);
	for (int i = 0; i < n_udcs; i++) {
		struct usbt_node *u = usbt_add(t, UT_UDC, -1);
		if (u) {
			char dir[PATH_MAX];
			snprintf(dir, sizeof(dir), "%s/%s", base, udcs[i]);
			snprintf(u->name, sizeof(u->name), "%s", udcs[i]);
			snprintf(u->id, sizeof(u->id), "udc:%s", udcs[i]);
			usbt_read_attr(dir, "state", u->udc_state, sizeof(u->udc_state));
			usbt_read_attr(dir, "current_speed", u->udc_speed, sizeof(u->udc_speed));
			usbt_read_attr(dir, "maximum_speed", u->udc_max_speed, sizeof(u->udc_max_speed));
			usbt_read_attr(dir, "function", u->udc_function, sizeof(u->udc_function));
			usbt_link_name(dir, "device/driver", u->driver, sizeof(u->driver));
		}
		free(udcs[i]);
	}
}

static bool usbt_has_children(const struct usbt *t, int i)
{
	return i + 1 < t->n && t->nodes[i + 1].parent == i;
}

static int usbt_find(const struct usbt *t, const char *id)
{
	for (int i = 0; i < t->n; i++)
		if (!strcmp(t->nodes[i].id, id))
			return i;
	return -1;
}

/* ---- words for what the numbers mean ---- */

static const char *usbt_speed_name(const char *mbps)
{
	double s = atof(mbps);
	if (!mbps[0])
		return "";
	if (s <= 1.5)
		return "low speed";
	if (s <= 12)
		return "full speed";
	if (s <= 480)
		return "high speed";
	if (s <= 5000)
		return "SuperSpeed";
	if (s <= 10000)
		return "SuperSpeed+";
	return "SuperSpeed+ 2x2";
}

/* "480 Mbit/s", "5 Gbit/s" */
static void usbt_speed_rate(const char *mbps, char *out, size_t size)
{
	double s = atof(mbps);
	if (!mbps[0])
		out[0] = 0;
	else if (s >= 1000)
		snprintf(out, size, "%g Gbit/s", s / 1000);
	else
		snprintf(out, size, "%s Mbit/s", mbps);
}

static const char *usbt_ep_type(int attr)
{
	static const char *const t[] = { "control", "isochronous", "bulk", "interrupt" };
	return t[attr & 3];
}

/* bInterval as time: frames (1 ms) at low and full speed, microframes
 * (125 us, as a power of two) at high speed and above */
static void usbt_ep_interval(const struct usbt_node *ep, const char *speed, char *out, size_t size)
{
	int type = ep->ep_attr & 3, iv = ep->ep_interval;
	double s = atof(speed);
	out[0] = 0;
	if (type == 0 || type == 2 || iv == 0)
		return;	/* control and bulk: no period */
	double us;
	if (s >= 480 || type == 1)
		us = (s >= 480 ? 125.0 : 1000.0) * (double)(1u << (iv > 16 ? 15 : iv - 1));
	else
		us = 1000.0 * iv;
	if (us >= 1000)
		snprintf(out, size, "every %g ms", us / 1000);
	else
		snprintf(out, size, "every %g us", us);
}

static void usbt_ep_packet(const struct usbt_node *ep, char *out, size_t size)
{
	int base = ep->ep_maxp & 0x7ff, extra = (ep->ep_maxp >> 11) & 3;
	if (extra)
		snprintf(out, size, "%d x %d bytes", extra + 1, base);
	else
		snprintf(out, size, "%d bytes", base);
}

/* The parts of a configuration's bmAttributes. */
static void usbt_conf_attrs(int attr, char *out, size_t size)
{
	snprintf(out, size, "%s%s", attr & 0x40 ? "self powered" : "bus powered",
		attr & 0x20 ? ", remote wakeup" : "");
}

/* the bcd as "2.00" */
static void usbt_bcd(unsigned bcd, char *out, size_t size)
{
	snprintf(out, size, "%x.%02x", bcd >> 8, bcd & 0xff);
}

/* What usbview shows of a node, for the tests and for `ztusbview --dump`. */
static void usbt_dump(const struct usbt *t, FILE *f)
{
	if (t->typec)
		fprintf(f, "typec %s data=%s power=%s partner=%s\n", t->typec_port, t->data_role, t->power_role,
			t->partner ? (t->partner_type[0] ? t->partner_type : "yes") : "no");
	if (t->usb_role[0])
		fprintf(f, "usb_role %s\n", t->usb_role);
	for (int i = 0; i < t->n; i++) {
		const struct usbt_node *n = &t->nodes[i];
		fprintf(f, "%*s", n->depth * 2, "");
		char a[32], b[32];
		switch (n->kind) {
		case UT_CTRL:
			fprintf(f, "controller %s driver=%s\n", n->name, n->driver[0] ? n->driver : "-");
			break;
		case UT_UDC:
			fprintf(f, "gadget %s state=%s speed=%s max=%s function=%s\n", n->name, n->udc_state,
				n->udc_speed, n->udc_max_speed, n->udc_function[0] ? n->udc_function : "-");
			break;
		case UT_DEV:
			usbt_bcd(n->bcd_usb, a, sizeof(a));
			fprintf(f, "device %s %04x:%04x bus=%d dev=%d \"%s\" \"%s\" speed=%s usb=%s bcd=%s class=%02x/%02x/%02x "
				"driver=%s power=%s ports=%d configs=%d ep0=%d%s",
				n->name, n->vid, n->pid, n->busnum, n->devnum, n->manufacturer, n->product, n->speed,
				n->version, a, n->cls, n->sub, n->proto, n->driver[0] ? n->driver : "-",
				n->max_power[0] ? n->max_power : "-", n->maxchild, n->nconfigs, n->maxpacket0,
				n->root_hub ? " root" : "");
			if (n->vendor_name[0] || n->product_name[0])
				fprintf(f, " ids=\"%s\"/\"%s\"", n->vendor_name, n->product_name);
			if (n->class_name[0])
				fprintf(f, " cname=\"%s\"", n->class_name);
			fputc('\n', f);
			break;
		case UT_CONF:
			usbt_conf_attrs(n->conf_attr, a, sizeof(a));
			fprintf(f, "config %d%s ifaces=%d %s %dmA \"%s\"\n", n->conf_value, n->active ? " active" : "",
				n->n_ifaces, a, n->conf_power_ma, n->str);
			break;
		case UT_IFACE:
			fprintf(f, "interface %d alt %d%s class=%02x/%02x/%02x eps=%d driver=%s \"%s\"", n->if_num,
				n->if_alt, n->active ? " active" : "", n->cls, n->sub, n->proto, n->n_eps,
				n->driver[0] ? n->driver : "-", n->str);
			if (n->class_name[0])
				fprintf(f, " cname=\"%s\"", n->class_name);
			fputc('\n', f);
			break;
		case UT_EP: {
			/* the speed is the device's, two levels up */
			const struct usbt_node *dev = &t->nodes[t->nodes[n->parent].parent];
			dev = &t->nodes[dev->parent];
			usbt_ep_packet(n, a, sizeof(a));
			usbt_ep_interval(n, dev->speed, b, sizeof(b));
			fprintf(f, "endpoint %02x %s %s %s%s%s\n", n->ep_addr, n->ep_addr & 0x80 ? "in" : "out",
				usbt_ep_type(n->ep_attr), a, b[0] ? " " : "", b);
			break;
		}
		}
	}
}

#endif
