// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * usb-energy-ui.h: the USB accessories' energy record (usb-energy.h), shown.
 * Settings' battery page draws ue_ui_section(); USBView puts an accessory's
 * total on its page with ue_ui_total(). Header only; needs zrnmetrocore.h.
 * The record is read again only when its file changes.
 */
#ifndef ZT_USB_ENERGY_UI_H
#define ZT_USB_ENERGY_UI_H

#include "usb-energy.h"

static struct ue_store *ue_ui_store;

static inline const struct ue_store *ue_ui_load(void)
{
	static struct timespec seen;
	static bool tried;
	char path[PATH_MAX];
	ue_store_path(path, sizeof(path));
	struct stat st;
	bool exists = stat(path, &st) == 0;
	if (!ue_ui_store) {
		ue_ui_store = calloc(1, sizeof(*ue_ui_store));
		if (!ue_ui_store)
			return NULL;
	}
	if (!exists) {
		ue_ui_store->n = 0;
		return ue_ui_store;
	}
	if (tried && st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec)
		return ue_ui_store;
	tried = true;
	seen = st.st_mtim;
	ue_load(path, ue_ui_store);	/* what parsed is shown even if the rest did not */
	return ue_ui_store;
}

static inline void ue_ui_when(long long t, char *out, size_t size)
{
	time_t now = time(NULL), tt = t;
	struct tm a, b;
	localtime_r(&tt, &a);
	localtime_r(&now, &b);
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
		strftime(out, size, "today %H:%M", &a);
	else if (a.tm_year == b.tm_year)
		strftime(out, size, "%b %e, %H:%M", &a);
	else
		strftime(out, size, "%b %e %Y", &a);
}

static inline bool ue_ui_connected(const struct ue_acc *a)
{
	return a->n_sess && !a->sess[a->n_sess - 1].end;
}

/* "12 sessions, 3 h 05 min in, last seen today 14:05" */
static inline void ue_ui_usage_line(const struct ue_acc *a, char *out, size_t size)
{
	char when[48], dur[32];
	double s = a->connected_s;
	if (s < 3600)
		snprintf(dur, sizeof(dur), "%.0f min", s / 60);
	else
		snprintf(dur, sizeof(dur), "%d h %02d min", (int)(s / 3600), (int)(s / 60) % 60);
	if (ue_ui_connected(a))
		snprintf(when, sizeof(when), "plugged in now");
	else {
		char at[32];
		ue_ui_when(a->last_seen, at, sizeof(at));
		snprintf(when, sizeof(when), "last seen %s", at);
	}
	snprintf(out, size, "%d session%s, %s in all, %s", a->n_sessions, a->n_sessions == 1 ? "" : "s", dur, when);
}

/* The section for Settings > battery */
static inline double ue_ui_section(cairo_t *cr, double y)
{
	const struct ue_store *s = ue_ui_load();
	zmc_accent(cr);
	zmc_text(cr, "USB accessories", FONT_SEMILIGHT, 19, ZMC_MARGIN, y, 0);
	y += 34;
	double w = zmc.width - 2 * ZMC_MARGIN;
	if (!s || !s->n) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		return y + 12 + zmc_text(cr, "What USB accessories draw from the phone shows here once one has been "
			"plugged in.", FONT_REGULAR, 14, ZMC_MARGIN, y, w);
	}
	const struct ue_acc *by[UE_MAX_ACC];
	for (int i = 0; i < s->n; i++)
		by[i] = &s->acc[i];
	qsort(by, s->n, sizeof(by[0]), ue_by_last_seen);
	for (int i = 0; i < s->n && i < 12; i++) {
		const struct ue_acc *a = by[i];
		char line[200];
		zmc_fg(cr);
		zmc_text_line(cr, a->name, FONT_SEMILIGHT, 19, ZMC_MARGIN, y, w);
		y += 28;
		ue_describe(line, sizeof(line), a->wh, a->method, a->est_err_wh);
		zmc_fg(cr);
		y += 4 + zmc_text(cr, line, FONT_REGULAR, 15, ZMC_MARGIN, y, w);
		ue_ui_usage_line(a, line, sizeof(line));
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += 14 + zmc_text(cr, line, FONT_REGULAR, 13, ZMC_MARGIN, y, w);
	}
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += 12 + zmc_text(cr, "The phone cannot measure what it gives out over USB-C. A total is the device's "
		"declared maximum (bMaxPower) unless the battery showed it clearly above the phone's own draw.",
		FONT_REGULAR, 12.5, ZMC_MARGIN, y, w);
	return y;
}

/* An accessory's total for USBView, by what identifies it; false if none */
static inline bool ue_ui_total(unsigned vid, unsigned pid, const char *serial, const char *port, char *out,
	size_t size)
{
	const struct ue_store *s = ue_ui_load();
	if (!s)
		return false;
	char v[8], p[8], key[160];
	snprintf(v, sizeof(v), "%04x", vid);
	snprintf(p, sizeof(p), "%04x", pid);
	ue_make_key(key, sizeof(key), v, p, serial, port);
	for (int i = 0; i < s->n; i++) {
		const struct ue_acc *a = &s->acc[i];
		if (strcmp(a->key, key))
			continue;
		char d[160], u[160];
		ue_describe(d, sizeof(d), a->wh, a->method, a->est_err_wh);
		ue_ui_usage_line(a, u, sizeof(u));
		snprintf(out, size, "%s; %s", d, u);
		return true;
	}
	return false;
}

#endif
