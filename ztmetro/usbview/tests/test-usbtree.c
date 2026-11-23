// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USBView's reading of sysfs, on a fake tree: an xHCI controller with a USB 2
 * and a USB 3 root hub, a four-port hub on the first, EarPods (audio and HID,
 * one interface with two alternate settings) and a device no driver took on
 * the hub, the USB-C port as host, and a UDC. The tree read from it, with
 * names from a small usb.ids, must be tests/expected.txt.
 *
 *	test-usbtree DIR/expected.txt		(DIR: usbview/tests)
 *	test-usbtree --print			what it reads, to make expected.txt
 *	test-usbtree --make DIR			leave the fake tree in DIR
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "../usb-ids.h"

static char root[PATH_MAX];
static int failed;

static void mkdirs(const char *rel)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s", root, rel);
	for (char *s = p + strlen(root) + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
	}
	mkdir(p, 0755);
}

static void put(const char *dir, const char *attr, const char *fmt, ...)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s/%s", root, dir, attr);
	FILE *f = fopen(p, "w");
	if (!f) {
		perror(p);
		exit(2);
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static void bin(const char *dir, const unsigned char *b, size_t n)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s/descriptors", root, dir);
	FILE *f = fopen(p, "wb");
	fwrite(b, 1, n, f);
	fclose(f);
}

/* link REL (relative to root) -> TARGET (relative to the link's directory) */
static void lnk(const char *target, const char *rel)
{
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s", root, rel);
	if (symlink(target, p) < 0 && errno != EEXIST) {
		perror(p);
		exit(2);
	}
}

#define CTRL "devices/platform/soc/11000000.dwc3/xhci-hcd.0.auto"

static void device(const char *dir, const char *name, unsigned vid, unsigned pid, const char *mfr,
	const char *product, const char *speed, const char *ver, int cls, int bus, int dev, int ports,
	const char *power, const char *driver)
{
	mkdirs(dir);
	put(dir, "idVendor", "%04x", vid);
	put(dir, "idProduct", "%04x", pid);
	put(dir, "bcdDevice", "0100");
	if (mfr)
		put(dir, "manufacturer", "%s", mfr);
	if (product)
		put(dir, "product", "%s", product);
	put(dir, "speed", "%s", speed);
	put(dir, "version", "%s", ver);
	put(dir, "bDeviceClass", "%02x", cls);
	put(dir, "bDeviceSubClass", "00");
	put(dir, "bDeviceProtocol", cls == 9 ? "01" : "00");
	put(dir, "busnum", "%d", bus);
	put(dir, "devnum", "%d", dev);
	put(dir, "maxchild", "%d", ports);
	put(dir, "bNumConfigurations", "1");
	put(dir, "bConfigurationValue", "1");
	put(dir, "bMaxPower", "%s", power);
	put(dir, "removable", "unknown");
	if (driver) {
		char l[PATH_MAX];
		snprintf(l, sizeof(l), "%s/driver", dir);
		lnk("/bus/usb/drivers/usb", l);
	}
	char l[PATH_MAX], t[PATH_MAX];
	snprintf(l, sizeof(l), "bus/usb/devices/%s", name);
	snprintf(t, sizeof(t), "../../../%s", dir);
	lnk(t, l);
}

static void iface(const char *devdir, const char *name, int alt, int cls, int sub, const char *driver,
	const char *string)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", devdir, name);
	mkdirs(dir);
	put(dir, "bAlternateSetting", "%2d", alt);
	char l[PATH_MAX], t[PATH_MAX];
	snprintf(l, sizeof(l), "bus/usb/devices/%s", name);
	snprintf(t, sizeof(t), "../../../%s", dir);
	lnk(t, l);
	put(dir, "bInterfaceClass", "%02x", cls);
	put(dir, "bInterfaceSubClass", "%02x", sub);
	if (string)
		put(dir, "interface", "%s", string);
	if (driver) {
		snprintf(l, sizeof(l), "%s/driver", dir);
		snprintf(t, sizeof(t), "/bus/usb/drivers/%s", driver);
		lnk(t, l);
	}
}

/* descriptors: the device's, then one configuration */
static size_t desc_dev(unsigned char *b, unsigned bcd_usb, int cls, int mp0)
{
	unsigned char d[18] = { 18, 1, bcd_usb & 0xff, bcd_usb >> 8, (unsigned char)cls, 0, 0, (unsigned char)mp0,
		0, 0, 0, 0, 0, 1, 1, 2, 0, 1 };
	memcpy(b, d, 18);
	return 18;
}

static size_t desc(unsigned char *b, size_t at, int n, ...)
{
	va_list ap;
	va_start(ap, n);
	for (int i = 0; i < n; i++)
		b[at + i] = (unsigned char)va_arg(ap, int);
	va_end(ap);
	return at + (size_t)n;
}

static void conf_len(unsigned char *b, size_t conf_at, size_t end)
{
	size_t total = end - conf_at;
	b[conf_at + 2] = total & 0xff;
	b[conf_at + 3] = (unsigned char)(total >> 8);
}

static void hub_desc(const char *dir, unsigned bcd, int ss)
{
	unsigned char b[64];
	size_t at = desc_dev(b, bcd, 9, ss ? 9 : 64), c = at;
	at = desc(b, at, 9, 9, 2, 0, 0, 1, 1, 0, 0xe0, 0);			/* config 1, self powered, wakeup */
	at = desc(b, at, 9, 9, 4, 0, 0, 1, 9, 0, 0, 0);			/* hub */
	at = desc(b, at, 7, 7, 5, 0x81, 3, 4, 0, 12);			/* interrupt in, 4 bytes */
	if (ss)
		at = desc(b, at, 6, 6, 0x30, 0, 0, 2, 0);			/* SuperSpeed companion: skipped */
	conf_len(b, c, at);
	bin(dir, b, at);
}

static void build(void)
{
	mkdirs("bus/usb/devices");
	mkdirs("bus/usb/drivers");
	mkdirs(CTRL);
	lnk("../../../../../bus/platform/drivers/xhci-hcd", CTRL "/driver");

	/* USB 2 root hub, then its hub, then two devices on the hub */
	device(CTRL "/usb1", "usb1", 0x1d6b, 0x0002, "Linux 6.12 xhci-hcd", "xHCI Host Controller", "480", " 2.00",
		9, 1, 1, 1, "0mA", "usb");
	hub_desc(CTRL "/usb1", 0x0200, 0);
	iface(CTRL "/usb1", "1-0:1.0", 0, 9, 0, "hub", NULL);

	device(CTRL "/usb1/1-1", "1-1", 0x05e3, 0x0610, "GenesysLogic", "USB2.1 Hub", "480", " 2.10", 9, 1, 2, 4,
		"100mA", "usb");
	hub_desc(CTRL "/usb1/1-1", 0x0210, 0);
	iface(CTRL "/usb1/1-1", "1-1:1.0", 0, 9, 0, "hub", NULL);

	device(CTRL "/usb1/1-1/1-1.2", "1-1.2", 0x05ac, 0x110b, "Apple, Inc.", "EarPods", "12", " 1.10", 0, 1, 4,
		0, "100mA", "usb");
	{
		unsigned char b[256];
		size_t at = desc_dev(b, 0x0110, 0, 8), c = at;
		at = desc(b, at, 9, 9, 2, 0, 0, 3, 1, 0, 0x80, 50);		/* bus powered, 100 mA */
		at = desc(b, at, 9, 9, 4, 0, 0, 0, 1, 1, 0, 0);		/* audio control */
		at = desc(b, at, 10, 10, 0x24, 1, 0, 1, 0x1e, 0, 1, 1, 1);	/* class specific: skipped */
		at = desc(b, at, 9, 9, 4, 1, 0, 0, 1, 2, 0, 0);		/* streaming, alt 0: no endpoints */
		at = desc(b, at, 9, 9, 4, 1, 1, 1, 1, 2, 0, 0);		/* alt 1: one iso out */
		at = desc(b, at, 9, 9, 5, 0x01, 0x09, 0xc0, 0, 1, 0, 0);	/* iso out, adaptive, 192 bytes, 1 ms */
		at = desc(b, at, 9, 9, 4, 2, 0, 1, 3, 0, 0, 0);		/* HID */
		at = desc(b, at, 7, 7, 5, 0x83, 3, 3, 0, 10);		/* interrupt in, 3 bytes, 10 ms */
		conf_len(b, c, at);
		bin(CTRL "/usb1/1-1/1-1.2", b, at);
	}
	iface(CTRL "/usb1/1-1/1-1.2", "1-1.2:1.0", 0, 1, 1, "snd-usb-audio", NULL);
	iface(CTRL "/usb1/1-1/1-1.2", "1-1.2:1.1", 1, 1, 2, "snd-usb-audio", NULL);	/* playing: alt 1 */
	iface(CTRL "/usb1/1-1/1-1.2", "1-1.2:1.2", 0, 3, 0, "usbhid", "EarPods remote");
	put(CTRL "/usb1/1-1/1-1.2", "configuration", "Default");

	/* on port 10: sorted after port 2; nothing bound to its interface */
	device(CTRL "/usb1/1-1/1-1.10", "1-1.10", 0x1234, 0xabcd, NULL, NULL, "480", " 2.00", 0xff, 1, 5, 0,
		"500mA", "usb");
	{
		unsigned char b[64];
		size_t at = desc_dev(b, 0x0200, 0xff, 64), c = at;
		at = desc(b, at, 9, 9, 2, 0, 0, 1, 1, 0, 0x80, 250);
		at = desc(b, at, 9, 9, 4, 0, 0, 2, 0xff, 0, 0, 0);
		at = desc(b, at, 7, 7, 5, 0x81, 2, 0x00, 0x02, 0);		/* bulk in 512 */
		at = desc(b, at, 7, 7, 5, 0x02, 2, 0x00, 0x02, 0);		/* bulk out 512 */
		conf_len(b, c, at);
		bin(CTRL "/usb1/1-1/1-1.10", b, at);
	}
	iface(CTRL "/usb1/1-1/1-1.10", "1-1.10:1.0", 0, 0xff, 0, NULL, NULL);

	/* the USB 3 root hub of the same controller */
	device(CTRL "/usb2", "usb2", 0x1d6b, 0x0003, "Linux 6.12 xhci-hcd", "xHCI Host Controller", "5000", " 3.00",
		9, 2, 1, 1, "0mA", "usb");
	hub_desc(CTRL "/usb2", 0x0300, 1);
	iface(CTRL "/usb2", "2-0:1.0", 0, 9, 0, "hub", NULL);

	/* the port, as host, and the gadget side as well */
	mkdirs("class/typec/port0");
	put("class/typec/port0", "data_role", "[host] device");
	put("class/typec/port0", "power_role", "[source] sink");
	mkdirs("class/typec/port0-partner");
	put("class/typec/port0-partner", "type", "");
	mkdirs("class/usb_role/10c00000.usb-role-switch");
	put("class/usb_role/10c00000.usb-role-switch", "role", "host");
	mkdirs("class/udc/10c00000.dwc3");
	put("class/udc/10c00000.dwc3", "state", "not attached");
	put("class/udc/10c00000.dwc3", "current_speed", "UNKNOWN");
	put("class/udc/10c00000.dwc3", "maximum_speed", "super-speed");

	char ids[PATH_MAX];
	snprintf(ids, sizeof(ids), "%s/usb.ids", root);
	FILE *f = fopen(ids, "w");
	fputs("# usb.ids, a few lines of it\n"
		"#\tthe real one is hwdata's\n"
		"05ac  Apple, Inc.\n"
		"\t110a  Some other Apple thing\n"
		"\t110b  EarPods\n"
		"05e3  Genesys Logic, Inc.\n"
		"\t0610  Hub\n"
		"1d6b  Linux Foundation\n"
		"\t0002  2.0 root hub\n"
		"\t0003  3.0 root hub\n"
		"\n"
		"# List of known device classes\n"
		"C 00  (Defined at Interface level)\n"
		"C 01  Audio\n"
		"\t01  Control Device\n"
		"\t02  Streaming\n"
		"\t03  MIDI Streaming\n"
		"C 03  Human Interface Device\n"
		"\t00  No Subclass\n"
		"\t\t00  None\n"
		"C 09  Hub\n"
		"\t00  Unused\n"
		"\t\t00  Full speed (or root) hub\n"
		"\t\t01  Single TT\n"
		"C ff  Vendor Specific Class\n"
		"\t00  Vendor Specific Subclass\n"
		"\n"
		"AT 0100  USB Undefined\n", f);
	fclose(f);
}

static int rm_cb(const char *p)
{
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
	return system(cmd);
}

static void check(bool ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failed++;
	}
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--make")) {
		/* the fake tree left in DIR, for ztusbview --render (ZT_USB_SYSFS=DIR) */
		snprintf(root, sizeof(root), "%s", argv[2]);
		mkdir(root, 0755);
		build();
		return 0;
	}
	snprintf(root, sizeof(root), "%s/usbtree-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 2;
	}
	build();
	static struct usbt t;
	struct usb_ids ids;
	char idp[PATH_MAX];
	snprintf(idp, sizeof(idp), "%s/usb.ids", root);
	check(usb_ids_load(&ids, idp), "usb.ids loads");
	usbt_read(&t, root);
	usbt_names(&t, &ids);

	char *got = NULL;
	size_t got_len = 0;
	FILE *m = open_memstream(&got, &got_len);
	usbt_dump(&t, m);
	fclose(m);
	if (argc > 1 && !strcmp(argv[1], "--print")) {
		fputs(got, stdout);
		rm_cb(root);
		return 0;
	}

	/* the shape, whatever the text says */
	int i = usbt_find(&t, "1-1.2");
	check(i >= 0 && t.nodes[t.nodes[i].parent].kind == UT_DEV && !strcmp(t.nodes[t.nodes[i].parent].name, "1-1"),
		"EarPods hang off the hub");
	check(usbt_find(&t, "1-1.2") < usbt_find(&t, "1-1.10"), "port 2 before port 10");
	check(t.nodes[0].kind == UT_CTRL && !strcmp(t.nodes[0].driver, "xhci-hcd"), "the controller first, with its driver");
	int a1 = usbt_find(&t, "1-1.2#c1i1a1"), a0 = usbt_find(&t, "1-1.2#c1i1a0");
	check(a1 >= 0 && t.nodes[a1].active && !strcmp(t.nodes[a1].driver, "snd-usb-audio"), "alt 1 in use, bound");
	check(a0 >= 0 && !t.nodes[a0].active && !t.nodes[a0].driver[0], "alt 0 not in use");
	check(usbt_find(&t, "ctrl:xhci-hcd.0.auto") == 0 && usbt_find(&t, "usb2") > usbt_find(&t, "1-1.10"),
		"one controller for both buses");
	check(!strcmp(t.data_role, "host") && !strcmp(t.power_role, "source") && !strcmp(t.usb_role, "host"), "roles");

	char exp_path[PATH_MAX];
	snprintf(exp_path, sizeof(exp_path), "%s", argc > 1 ? argv[1] : "tests/expected.txt");
	FILE *e = fopen(exp_path, "r");
	if (!e) {
		perror(exp_path);
		failed++;
	} else {
		char *want = NULL;
		size_t cap = 0;
		ssize_t n = getdelim(&want, &cap, 0, e);
		fclose(e);
		if (n < 0 || strcmp(want, got)) {
			fprintf(stderr, "FAIL: the tree differs from %s; it is:\n%s", exp_path, got);
			failed++;
		}
		free(want);
	}
	free(got);
	usb_ids_free(&ids);
	rm_cb(root);
	if (!failed)
		printf("usbview: tree from a fake sysfs OK\n");
	return failed ? 1 : 0;
}
