// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Names for USB numbers from usb.ids (hwdata), for USBView: vendors, their
 * products, and classes with their subclasses and protocols. The file is
 * read once into memory with an index of its vendor lines; without it, a
 * short table of the classes still names them.
 *
 *	vvvv  Vendor			C cc  Class
 *	\tpppp  Product			\tss  Subclass
 *					\t\tpp  Protocol
 */
#ifndef USB_IDS_H
#define USB_IDS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb-tree.h"

struct usb_ids {
	char *text;
	size_t len;
	struct { unsigned id; size_t at; } *vendors;
	int n_vendors;
	size_t classes;		/* where "C 00" starts; 0: none */
};

static void usb_ids_free(struct usb_ids *ids)
{
	free(ids->text);
	free(ids->vendors);
	memset(ids, 0, sizeof(*ids));
}

static bool usb_ids_load(struct usb_ids *ids, const char *path)
{
	memset(ids, 0, sizeof(*ids));
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 16 << 20 || !(ids->text = malloc((size_t)size + 1))) {
		fclose(f);
		return false;
	}
	ids->len = fread(ids->text, 1, (size_t)size, f);
	ids->text[ids->len] = 0;
	fclose(f);
	int cap = 0;
	for (size_t at = 0; at < ids->len;) {
		const char *l = ids->text + at;
		const char *nl = memchr(l, '\n', ids->len - at);
		size_t next = nl ? (size_t)(nl - ids->text) + 1 : ids->len;
		if (!strncmp(l, "C ", 2) && !ids->classes)
			ids->classes = at;
		else if (!ids->classes && l[0] != '#' && l[0] != '\t' && l[0] != '\n') {
			char *end;
			unsigned long id = strtoul(l, &end, 16);
			if (end == l + 4 && *end == ' ') {
				if (ids->n_vendors == cap) {
					cap = cap ? cap * 2 : 4096;
					void *v = realloc(ids->vendors, (size_t)cap * sizeof(ids->vendors[0]));
					if (!v)
						break;
					ids->vendors = v;
				}
				ids->vendors[ids->n_vendors].id = (unsigned)id;
				ids->vendors[ids->n_vendors++].at = at;
			}
		}
		at = next;
	}
	return true;
}

/* the name after "xxxx  " on the line at `at`, into out */
static void usb_ids_name(const struct usb_ids *ids, size_t at, int skip, char *out, size_t size)
{
	const char *l = ids->text + at + skip;
	while (*l == ' ')
		l++;
	size_t n = strcspn(l, "\n");
	snprintf(out, size, "%.*s", (int)n, l);
}

/* the line at `at` and those after it that start with `tabs` tabs and then
 * `id` as `digits` hex digits, before the next line with fewer tabs */
static size_t usb_ids_child(const struct usb_ids *ids, size_t at, int tabs, unsigned id, int digits)
{
	const char *nl = memchr(ids->text + at, '\n', ids->len - at);
	at = nl ? (size_t)(nl - ids->text) + 1 : ids->len;
	while (at < ids->len) {
		const char *l = ids->text + at;
		nl = memchr(l, '\n', ids->len - at);
		size_t next = nl ? (size_t)(nl - ids->text) + 1 : ids->len;
		int t = 0;
		while (l[t] == '\t')
			t++;
		if (l[0] == '#' || l[0] == '\n') {
			at = next;
			continue;
		}
		if (t < tabs)
			return 0;
		if (t == tabs) {
			char *end;
			unsigned long v = strtoul(l + t, &end, 16);
			if (end == l + t + digits && v == id)
				return at;
		}
		at = next;
	}
	return 0;
}

static bool usb_ids_vendor(const struct usb_ids *ids, unsigned vid, size_t *at)
{
	int lo = 0, hi = ids->n_vendors - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (ids->vendors[mid].id == vid) {
			*at = ids->vendors[mid].at;
			return true;
		}
		if (ids->vendors[mid].id < vid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/* Without usb.ids: the base classes, as usb.org names them. */
static const char *usb_class_fallback(int cls)
{
	switch (cls) {
	case 0x00: return "(Defined at Interface level)";
	case 0x01: return "Audio";
	case 0x02: return "Communications";
	case 0x03: return "Human Interface Device";
	case 0x05: return "Physical Interface Device";
	case 0x06: return "Imaging";
	case 0x07: return "Printer";
	case 0x08: return "Mass Storage";
	case 0x09: return "Hub";
	case 0x0a: return "CDC Data";
	case 0x0b: return "Chip/SmartCard";
	case 0x0d: return "Content Security";
	case 0x0e: return "Video";
	case 0x0f: return "Personal Healthcare";
	case 0x10: return "Audio/Video";
	case 0x11: return "Billboard";
	case 0x12: return "Type-C Bridge";
	case 0xdc: return "Diagnostic";
	case 0xe0: return "Wireless";
	case 0xef: return "Miscellaneous Device";
	case 0xfe: return "Application Specific Interface";
	case 0xff: return "Vendor Specific Class";
	}
	return "";
}

/* "Audio / Streaming", or the class alone, from the C lines */
static void usb_ids_class(const struct usb_ids *ids, int cls, int sub, int proto, char *out, size_t size)
{
	out[0] = 0;
	if (ids && ids->classes) {
		size_t c = ids->classes;
		const char *l;
		/* the C line itself */
		for (size_t at = c; at < ids->len;) {
			l = ids->text + at;
			const char *nl = memchr(l, '\n', ids->len - at);
			if (!strncmp(l, "C ", 2) && strtoul(l + 2, NULL, 16) == (unsigned long)cls) {
				char cn[64], sn[64] = "", pn[64] = "";
				usb_ids_name(ids, at, 4, cn, sizeof(cn));
				size_t s = usb_ids_child(ids, at, 1, (unsigned)sub, 2);
				if (s) {
					usb_ids_name(ids, s, 3, sn, sizeof(sn));
					size_t p = usb_ids_child(ids, s, 2, (unsigned)proto, 2);
					if (p)
						usb_ids_name(ids, p, 4, pn, sizeof(pn));
				}
				snprintf(out, size, "%s%s%s%s%s", cn, sn[0] ? " / " : "", sn, pn[0] ? " / " : "", pn);
				return;
			}
			if (!nl)
				break;
			at = (size_t)(nl - ids->text) + 1;
		}
	}
	snprintf(out, size, "%s", usb_class_fallback(cls));
}

/* The names of every device and interface in the tree. */
static void usbt_names(struct usbt *t, const struct usb_ids *ids)
{
	for (int i = 0; i < t->n; i++) {
		struct usbt_node *n = &t->nodes[i];
		if (n->kind == UT_DEV) {
			size_t v;
			if (ids && ids->text && usb_ids_vendor(ids, n->vid, &v)) {
				usb_ids_name(ids, v, 4, n->vendor_name, sizeof(n->vendor_name));
				size_t p = usb_ids_child(ids, v, 1, n->pid, 4);
				if (p)
					usb_ids_name(ids, p, 5, n->product_name, sizeof(n->product_name));
			}
			usb_ids_class(ids, n->cls, n->sub, n->proto, n->class_name, sizeof(n->class_name));
		} else if (n->kind == UT_IFACE) {
			usb_ids_class(ids, n->cls, n->sub, n->proto, n->class_name, sizeof(n->class_name));
		}
	}
}

#endif
