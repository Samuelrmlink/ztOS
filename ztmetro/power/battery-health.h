// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The battery's cycles and health, worded for what each number really is
 * (ROADMAP 8.27, power/battery-cycles.md). Header-only: Power, Settings and
 * ZrnDevTest's charging page share it.
 *
 * - The fuel gauge's Cycles register counts in 1 % of a cycle (MAX17055
 *   family, ModelGauge m5; Samsung's own fg_cycle divides it by 100), and the
 *   kernel's cycle_count hands it over raw. It counts since the gauge's
 *   counter last started, not over the battery's life.
 * - charge_full is what the gauge has learned the battery holds; health is
 *   that over the design capacity, an estimate that moves as it relearns.
 * - Samsung's count is Android's: the per cent of charge discharged over the
 *   battery's life, kept in EFS as batt_discharge_level, a cycle per 100.
 *   Only Android adds to it, so it stops on the day Android last ran. Read
 *   here, never written.
 */
#ifndef ZT_BATTERY_HEALTH_H
#define ZT_BATTERY_HEALTH_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#ifndef BH_SAMSUNG_LEVEL
#define BH_SAMSUNG_LEVEL "/run/android/mnt/vendor/efs/FactoryApp/batt_discharge_level"
#endif

struct battery_health {
	long gauge_raw;		/* Cycles register, 1 % of a cycle; -1 unknown */
	long full_mah, design_mah;	/* learned and design capacity; -1 unknown */
	int health_pct;		/* full / design, -1 unknown */
	long samsung_level;	/* per cent discharged over life; -1 unread */
	time_t samsung_when;	/* when Android last wrote it */
};

static inline long bh_read_long(const char *dir, const char *leaf) {
	char path[256], text[32];
	snprintf(path, sizeof(path), "%s/%s", dir, leaf);
	FILE *f = fopen(path, "re");
	if (!f) return -1;
	long v = -1;
	if (fgets(text, sizeof(text), f)) {
		char *end;
		v = strtol(text, &end, 10);
		if (end == text) v = -1;
	}
	fclose(f);
	return v;
}

static inline void battery_health_read(const char *gauge_dir, struct battery_health *b) {
	b->gauge_raw = bh_read_long(gauge_dir, "cycle_count");
	long full = bh_read_long(gauge_dir, "charge_full");
	long design = bh_read_long(gauge_dir, "charge_full_design");
	b->full_mah = full > 0 ? full / 1000 : -1;
	b->design_mah = design > 0 ? design / 1000 : -1;
	b->health_pct = (full > 0 && design > 0) ? (int)((full * 100 + design / 2) / design) : -1;

	b->samsung_level = -1;
	b->samsung_when = 0;
	FILE *f = fopen(BH_SAMSUNG_LEVEL, "re");
	if (f) {
		char text[32];
		if (fgets(text, sizeof(text), f)) {
			char *end;
			long v = strtol(text, &end, 10);
			if (end != text && v >= 0) b->samsung_level = v;
		}
		struct stat st;
		if (fstat(fileno(f), &st) == 0) b->samsung_when = st.st_mtime;
		fclose(f);
	}
}

/* "6.6 cycles since the gauge's counter started" */
static inline void battery_health_gauge_text(const struct battery_health *b, char *out, size_t sz) {
	if (b->gauge_raw < 0)
		snprintf(out, sz, "unavailable");
	else
		snprintf(out, sz, "%.1f cycles since the gauge's counter started", b->gauge_raw / 100.0);
}

/* "1039 cycles, as of 6 Nov 2026 (Android's count)" */
static inline void battery_health_samsung_text(const struct battery_health *b, char *out, size_t sz) {
	if (b->samsung_level < 0) {
		snprintf(out, sz, "unavailable (EFS not mounted)");
		return;
	}
	char when[32] = "";
	struct tm tm;
	if (b->samsung_when && localtime_r(&b->samsung_when, &tm))
		strftime(when, sizeof(when), ", as of %-d %b %Y", &tm);
	snprintf(out, sz, "%ld cycles%s, when Android last ran", b->samsung_level / 100, when);
}

/* "about 2236 of 2967 mAh (75 %), learned by the gauge" */
static inline void battery_health_capacity_text(const struct battery_health *b, char *out, size_t sz) {
	if (b->health_pct < 0)
		snprintf(out, sz, "unavailable");
	else
		snprintf(out, sz, "about %ld of %ld mAh (%d %%), the gauge's estimate",
			 b->full_mah, b->design_mah, b->health_pct);
}
#endif
