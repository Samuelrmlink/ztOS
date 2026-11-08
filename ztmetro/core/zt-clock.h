/*
 * zt-clock.h: the time of day as ztOS shows it, 24-hour ("21:41") or 12-hour
 * ("9:41 PM", no leading zero). One setting for the shell and every app:
 * `clock_24h = on|off` in ~/.config/ztmetro/config (Settings > Date & time;
 * on when absent). Durations ("5 min ago", uptime) are not times of day and
 * do not use this.
 *
 * Header only, for the shell and for apps that do not link the core.
 */
#ifndef ZT_CLOCK_H
#define ZT_CLOCK_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ZT_CLOCK_24H_DEFAULT	true	/* with no clock_24h in the config */

/* The hours and minutes ("9:41" or "21:41") and, in 12-hour time, "AM" or
 * "PM" in `ampm` (empty in 24-hour). Either buffer may be NULL. */
static inline void zt_clock_parts(const struct tm *tm, bool h24, char *hm, size_t hm_size,
		char *ampm, size_t ampm_size) {
	int h = tm->tm_hour;
	if (hm) {
		if (h24) {
			snprintf(hm, hm_size, "%02d:%02d", h, tm->tm_min);
		} else {
			snprintf(hm, hm_size, "%d:%02d", h % 12 ? h % 12 : 12, tm->tm_min);
		}
	}
	if (ampm) {
		snprintf(ampm, ampm_size, "%s", h24 ? "" : h < 12 ? "AM" : "PM");
	}
}

/* The whole of it on one line: "21:41" or "9:41 PM". */
static inline void zt_clock_text(const struct tm *tm, bool h24, char *out, size_t size) {
	char hm[16], ampm[4];
	zt_clock_parts(tm, h24, hm, sizeof(hm), ampm, sizeof(ampm));
	snprintf(out, size, ampm[0] ? "%s %s" : "%s", hm, ampm);
}

/* The setting from the config file, read again only when the file changes:
 * cheap enough to ask at every drawing. */
static inline bool zt_clock_24h(void) {
	static bool value = ZT_CLOCK_24H_DEFAULT;
	static struct timespec seen;
	static bool read_once;
	char path[512], line[256], v[16];
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", getenv("HOME") ? getenv("HOME") : "");
	struct stat st;
	if (stat(path, &st) != 0) {
		return value;
	}
	if (read_once && st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec) {
		return value;
	}
	read_once = true;
	seen = st.st_mtim;
	value = ZT_CLOCK_24H_DEFAULT;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, " clock_24h = %15s", v) == 1) {
			value = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
		}
	}
	if (f) {
		fclose(f);
	}
	return value;
}

/* The time `t` on one line, by the setting. */
static inline void zt_clock_text_now(time_t t, char *out, size_t size) {
	struct tm tm;
	localtime_r(&t, &tm);
	zt_clock_text(&tm, zt_clock_24h(), out, size);
}

#endif
