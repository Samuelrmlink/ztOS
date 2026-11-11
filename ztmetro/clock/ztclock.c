// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztclock: Alarms + Clock, for ztMETRO.
 *
 *   alarms	the alarms, each with its switch; tap one to change it, hold it
 *		to delete it, the plus for a new one. The alarm volume.
 *   world	the time in the places chosen; the plus adds one, hold to remove
 *   timer	a countdown: the drums to set it, then start, pause, reset
 *   stopwatch	start, stop, laps
 *
 * The alarms and the timer ring from ztclockd (a user service), with this
 * app closed and the screen off; this only writes what they are
 * (clock/clock-store.h). When something rings, the shell shows
 * `ztclock --ring` over the lock screen: snooze and dismiss.
 *
 *	ztclock				alarms
 *	ztclock --ring			the ring screen (the shell or ztclockd start it)
 *	ztclock --render PAGE FILE	a page into a PNG (alarms, edit, world, timer, stopwatch, ring)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>

#include "zrnmetrocore.h"
#include "zt-clock.h"
#include "clock-store.h"
#include "../volume-streams.h"

#define MARGIN ZMC_MARGIN

enum page { P_ALARMS, P_WORLD, P_TIMER, P_STOP, P_EDIT, P_SOUND, P_ZONES, P_RING };

enum action {
	ACT_EDIT = 1,	/* arg: which alarm */
	ACT_TOGGLE,	/* arg: which alarm */
	ACT_ADD,
	ACT_MENU, ACT_MENU_CLOSE,
	ACT_DAY,	/* arg: 0 Monday .. 6 Sunday */
	ACT_LABEL,
	ACT_SOUND_PAGE,
	ACT_SOUND,	/* arg: which sound */
	ACT_VIBRATE,
	ACT_SNOOZE,	/* picker */
	ACT_SAVE, ACT_DELETE, ACT_CANCEL,
	ACT_VOLUME,
	ACT_ZONE_ADD,
	ACT_ZONE_PICK,	/* arg: which match */
	ACT_ZONE_QUERY,
	ACT_ZONE_MENU, ACT_ZONE_MENU_CLOSE,
	ACT_T_START, ACT_T_PAUSE, ACT_T_RESET, ACT_T_PLUS,
	ACT_S_GO, ACT_S_LAP,
	ACT_R_SNOOZE, ACT_R_DISMISS,
	ACT_AMPM,
};

static bool sample;		/* --render: made-up content */

/* ---- the alarms ----------------------------------------------------------- */

static struct clock_alarm alarms[CLOCK_MAX_ALARMS];
static int n_alarms;
static struct timespec alarms_seen;
static int menu_for = -1;

static struct clock_alarm edit;	/* the alarm on the edit page */
static int editing = -1;	/* its index, -1 a new one */
static bool label_focus, snooze_open;
static const int SNOOZES[] = { 5, 10, 15, 20, 30 };
static const char *const SNOOZE_NAMES[] = { "5 minutes", "10 minutes", "15 minutes", "20 minutes", "30 minutes" };

static void alarms_load(void) {
	if (sample) return;
	int lock = clock_lock();
	n_alarms = clock_alarms_load(alarms, CLOCK_MAX_ALARMS);
	clock_unlock(lock);
	char path[512];
	struct stat st;
	clock_path(path, sizeof(path), "alarms");
	if (stat(path, &st) == 0) alarms_seen = st.st_mtim;
}

static void alarms_store(void) {
	if (sample) return;
	int lock = clock_lock();
	clock_alarms_save(alarms, n_alarms);
	clock_unlock(lock);
	char path[512];
	struct stat st;
	clock_path(path, sizeof(path), "alarms");
	if (stat(path, &st) == 0) alarms_seen = st.st_mtim;
	clock_tell("reload");
}

/* a snooze pending for it goes: it was switched, changed or deleted */
static void unsnooze(int id) {
	char word[32];
	snprintf(word, sizeof(word), "unsnooze %d", id);
	if (!sample) clock_tell(word);
}

static bool alarms_changed_on_disk(void) {
	char path[512];
	struct stat st;
	clock_path(path, sizeof(path), "alarms");
	if (stat(path, &st) != 0) return false;
	return st.st_mtim.tv_sec != alarms_seen.tv_sec || st.st_mtim.tv_nsec != alarms_seen.tv_nsec;
}

static void days_text(unsigned days, char *out, size_t size) {
	static const char *const names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	if (!days) snprintf(out, size, "Only once");
	else if (days == 0x7f) snprintf(out, size, "Every day");
	else if (days == 0x1f) snprintf(out, size, "Weekdays");
	else if (days == 0x60) snprintf(out, size, "Weekends");
	else {
		out[0] = 0;
		for (int d = 0; d < 7; d++) {
			if (!(days & (1u << d))) continue;
			size_t l = strlen(out);
			snprintf(out + l, size - l, "%s%s", l ? ", " : "", names[d]);
		}
	}
}

static void time_text(int hour, int minute, char *out, size_t size) {
	struct tm tm = { .tm_hour = hour, .tm_min = minute };
	zt_clock_text(&tm, zt_clock_24h(), out, size);
}

/* "in 7 hours 12 minutes" */
static void until_text(time_t t, char *out, size_t size) {
	long m = (long)((t - time(NULL) + 59) / 60);
	long d = m / 1440, h = m % 1440 / 60;
	m %= 60;
	if (d) snprintf(out, size, "Rings in %ld day%s %ld hour%s", d, d == 1 ? "" : "s", h, h == 1 ? "" : "s");
	else if (h) snprintf(out, size, "Rings in %ld hour%s %ld minute%s", h, h == 1 ? "" : "s", m, m == 1 ? "" : "s");
	else snprintf(out, size, "Rings in %ld minute%s", m, m == 1 ? "" : "s");
}

/* the four tabs */
static double tab_header(cairo_t *cr, int tab) {
	static const struct zmc_tab tabs[] = { { "alarms", P_ALARMS }, { "world clock", P_WORLD },
		{ "timer", P_TIMER }, { "stopwatch", P_STOP } };
	return zmc_pivot_wheel(cr, tabs, 4, tab);
}

static void grey(cairo_t *cr) { cairo_set_source_rgb(cr, 0.6, 0.6, 0.6); }
static void white(cairo_t *cr) { cairo_set_source_rgb(cr, 1, 1, 1); }

static double draw_alarms(cairo_t *cr) {
	double y = tab_header(cr, P_ALARMS);
	if (n_alarms == 0) {
		grey(cr);
		y += zmc_text(cr, "No alarms yet. Tap the plus sign to add one.", FONT_REGULAR, 15, MARGIN, y + 4,
			zmc.width - 2 * MARGIN) + 20;
	}
	double menu_at = -1;
	for (int i = 0; i < n_alarms; i++) {
		struct clock_alarm *a = &alarms[i];
		char t[24], detail[160], days[80];
		time_text(a->hour, a->minute, t, sizeof(t));
		days_text(a->days, days, sizeof(days));
		if (a->label[0]) snprintf(detail, sizeof(detail), "%s, %s", a->label, days);
		else snprintf(detail, sizeof(detail), "%s", days);
		/* the switch first: its hit wins over the row's */
		zmc_toggle(cr, zmc.width - MARGIN - 54, y + 22, a->on);
		zmc_add_hit(zmc.width - MARGIN - 80, y, 80 + MARGIN, 76, ACT_TOGGLE, i);
		if (a->on) white(cr);
		else cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
		zmc_text(cr, t, FONT_LIGHT, 44, MARGIN - 2, y - 6, 0);
		grey(cr);
		zmc_text_line(cr, detail, FONT_REGULAR, 14, MARGIN, y + 52, zmc.width - 2 * MARGIN - 90);
		zmc_add_hit(0, y - 4, zmc.width - MARGIN - 80, 82, ACT_EDIT, i);
		if (i == menu_for) menu_at = y + 78;
		y += 92;
	}
	white(cr);
	zmc_round_button(cr, zmc.width / 2, y + 6, "add", "add", ACT_ADD, 0);
	y += 100;
	int volume = sample ? CLOCK_VOLUME_DEFAULT : clock_setting("volume", CLOCK_VOLUME_DEFAULT);
	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, volume, "Alarm volume", ACT_VOLUME);
	y += 60;
	grey(cr);
	y += zmc_text(cr, "Alarms ring with the screen off and the app closed, and keep ringing until you "
		"snooze or dismiss them.", FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN) + 30;
	if (menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "delete" };
		zmc_menu(cr, menu_at, items, 1, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y;
}

/* ---- the drums: a touch-friendly number picker ---------------------------- */

/* A column of numbers, the chosen one boxed in the middle with one either
 * side. Drag it up or down (each DRUM_ROW points a step), or tap the number
 * above or below. They go round. */
#define DRUM_ROW 66.0
#define MAX_DRUMS 3
struct drum {
	int page;
	double x, y, w;
	int *value, count;
	bool hours;	/* in 12-hour time, shown as such */
};
static struct drum drums[MAX_DRUMS];
static int n_drums;
static struct { int drum, start; double y0; bool moved; int32_t id; } drag = { -1, 0, 0, false, -1 };

static int wrap(int v, int n) { return ((v % n) + n) % n; }

static void drum(cairo_t *cr, double x, double y, double w, int *value, int count, bool hours) {
	if (n_drums < MAX_DRUMS)
		drums[n_drums++] = (struct drum){ zmc.page, x, y, w, value, count, hours };
	bool h12 = hours && !zt_clock_24h();
	for (int k = -1; k <= 1; k++) {
		int v = wrap(*value + k, count);
		char s[16];
		if (h12) snprintf(s, sizeof(s), "%d", v % 12 ? v % 12 : 12);
		else snprintf(s, sizeof(s), "%02d", v);
		double top = y + (k + 1) * DRUM_ROW;
		if (k == 0) {
			zmc_accent(cr);
			cairo_rectangle(cr, x, top, w, DRUM_ROW);
			cairo_fill(cr);
			white(cr);
		} else {
			cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
			cairo_rectangle(cr, x + 1, top + 1, w - 2, DRUM_ROW - 2);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
		}
		zmc_text_centred(cr, s, FONT_LIGHT, 40, x + w / 2, top + 8);
	}
}

static double page_y_of(double y) { return y < ZMC_HEADER_H ? y : y + zmc.scroll; }

static bool drums_live(void) {
	if (zmc.page == P_EDIT) return true;
	if (zmc.page == P_TIMER) {
		struct clock_timer t;
		clock_timer_load(&t);
		return t.state == CLOCK_TIMER_IDLE;
	}
	return false;
}

static bool touch(int phase, int id, double x, double y, uint32_t time) {
	(void)time;
	if (phase == 0) {
		if (!drums_live() || zmc.anim_active) return false;
		double py = page_y_of(y);
		for (int i = 0; i < n_drums; i++) {
			struct drum *d = &drums[i];
			if (d->page != zmc.page || x < d->x || x >= d->x + d->w || py < d->y || py >= d->y + 3 * DRUM_ROW)
				continue;
			drag.drum = i;
			drag.start = *d->value;
			drag.y0 = y;
			drag.moved = false;
			drag.id = id;
			return true;
		}
		return false;
	}
	if (drag.drum < 0 || id != drag.id) return false;
	struct drum *d = &drums[drag.drum];
	if (phase == 1) {
		double dy = y - drag.y0;
		if (fabs(dy) > 10) drag.moved = true;
		if (drag.moved) {
			int v = wrap(drag.start - (int)lround(dy / DRUM_ROW), d->count);
			if (v != *d->value) {
				*d->value = v;
				zmc_haptic("tick");
				zmc_redraw();
			}
		}
	} else if (phase == 2 && !drag.moved) {
		double py = page_y_of(drag.y0);
		int step = py < d->y + DRUM_ROW ? -1 : py >= d->y + 2 * DRUM_ROW ? 1 : 0;
		if (step) {
			*d->value = wrap(*d->value + step, d->count);
			zmc_haptic("tick");
			zmc_redraw();
		}
	}
	if (phase >= 2) drag.drum = -1;
	return true;
}

/* ---- one alarm ------------------------------------------------------------ */

static double draw_edit(cairo_t *cr) {
	double y = zmc_header(cr, editing < 0 ? "new alarm" : "edit alarm");
	n_drums = 0;
	double w = zmc.width - 2 * MARGIN, col = 110, gap = 16;
	double x0 = MARGIN;
	drum(cr, x0, y, col, &edit.hour, 24, true);
	white(cr);
	zmc_text_centred(cr, ":", FONT_LIGHT, 40, x0 + col + gap / 2, y + DRUM_ROW + 6);
	drum(cr, x0 + col + gap, y, col, &edit.minute, 60, false);
	if (!zt_clock_24h()) {
		/* AM / PM: a box of its own, tapped to turn */
		double ax = x0 + 2 * (col + gap), ay = y + DRUM_ROW, aw = zmc.width - MARGIN - ax;
		cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
		cairo_rectangle(cr, ax, ay, aw, DRUM_ROW);
		cairo_fill(cr);
		white(cr);
		zmc_text_centred(cr, edit.hour < 12 ? "AM" : "PM", FONT_SEMILIGHT, 28, ax + aw / 2, ay + 14);
		zmc_add_hit(ax, ay, aw, DRUM_ROW, ACT_AMPM, 0);
	}
	y += 3 * DRUM_ROW + 14;
	struct clock_alarm probe = edit;
	probe.on = true;
	char until[80];
	time_t next = clock_alarm_next(&probe, time(NULL));
	if (next > 0) {
		until_text(next, until, sizeof(until));
		grey(cr);
		zmc_text(cr, until, FONT_REGULAR, 14, MARGIN, y, 0);
	}
	y += 34;
	/* the days: seven squares, lit when it rings that day */
	grey(cr);
	zmc_text(cr, "Repeat", FONT_REGULAR, 13, MARGIN, y, 0);
	y += 22;
	static const char *const letters[] = { "M", "T", "W", "T", "F", "S", "S" };
	double dg = 6, dw = (w - 6 * dg) / 7;
	for (int d = 0; d < 7; d++) {
		double x = MARGIN + d * (dw + dg);
		bool on = edit.days & (1u << d);
		if (on) {
			zmc_accent(cr);
			cairo_rectangle(cr, x, y, dw, dw);
			cairo_fill(cr);
		} else {
			cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, x + 1, y + 1, dw - 2, dw - 2);
			cairo_stroke(cr);
		}
		white(cr);
		zmc_text_centred(cr, letters[d], FONT_SEMILIGHT, 20, x + dw / 2, y + dw / 2 - 13);
		zmc_add_hit(x, y, dw, dw, ACT_DAY, d);
	}
	y += dw + 8;
	char days[80];
	days_text(edit.days, days, sizeof(days));
	grey(cr);
	zmc_text(cr, days, FONT_REGULAR, 13, MARGIN, y, 0);
	y += 34;
	y = zmc_textbox(cr, y, "Name", edit.label, label_focus, ACT_LABEL, 0) + 6;
	char sound[64];
	snprintf(sound, sizeof(sound), "%s", edit.sound);
	char *dot = strrchr(sound, '.');
	if (dot) *dot = 0;
	y = zmc_entry(cr, y, "sound", sound, ACT_SOUND_PAGE, 0);
	y = zmc_switch(cr, y, "Vibrate", edit.vibrate, ACT_VIBRATE);
	int sel = 1;
	for (int i = 0; i < 5; i++)
		if (SNOOZES[i] == edit.snooze) sel = i;
	y = zmc_picker(cr, y, "Snooze time", SNOOZE_NAMES, 5, sel, snooze_open, ACT_SNOOZE);
	y += 8;
	white(cr);
	double third = zmc.width / 3;
	zmc_round_button(cr, third / 2 + 8, y, "save", "save", ACT_SAVE, 0);
	if (editing >= 0) zmc_round_button(cr, third * 1.5, y, "delete", "delete", ACT_DELETE, 0);
	zmc_round_button(cr, third * 2.5 - 8, y, "cancel", "cancel", ACT_CANCEL, 0);
	return y + 110;
}

/* ---- sounds ---------------------------------------------------------------- */

#define MAX_SOUNDS 80
static char sounds[MAX_SOUNDS][64];
static int n_sounds;
static pid_t preview_pid;

static int sound_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void sounds_load(void) {
	n_sounds = 0;
	DIR *d = opendir(CLOCK_SOUNDS);
	struct dirent *e;
	while (d && (e = readdir(d)) && n_sounds < MAX_SOUNDS) {
		size_t l = strlen(e->d_name);
		if (l < 5 || l >= 64 || strcmp(e->d_name + l - 4, ".wav") || !clock_sound_ok(e->d_name)) continue;
		/* the phone's own tones are no alarm */
		if (!strncmp(e->d_name, "dtmf", 4) || !strncmp(e->d_name, "ringback", 8) || !strcmp(e->d_name, "Busy.wav") ||
				!strcmp(e->d_name, "Calldrop.wav") || !strcmp(e->d_name, "Callwait.wav") || !strcmp(e->d_name, "Disconnct.wav"))
			continue;
		snprintf(sounds[n_sounds++], 64, "%s", e->d_name);
	}
	if (d) closedir(d);
	qsort(sounds, (size_t)n_sounds, sizeof(sounds[0]), sound_cmp);
}

static void preview_stop(void) {
	if (preview_pid > 0) {
		kill(preview_pid, SIGTERM);
		waitpid(preview_pid, NULL, 0);
	}
	preview_pid = 0;
}

static void preview(const char *file) {
	preview_stop();
	if (sample) return;
	char path[300];
	snprintf(path, sizeof(path), "%s%s", CLOCK_SOUNDS, file);
	pid_t pid = fork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		/* as it will ring: the alarm's stream and level (volume-streams.h) */
		vs_exec_play(VS_ALARM, 1.0, false, path);
		_exit(127);
	}
	preview_pid = pid > 0 ? pid : 0;
}

static double draw_sound(cairo_t *cr) {
	double y = zmc_header(cr, "sound");
	for (int i = 0; i < n_sounds; i++) {
		char name[64];
		snprintf(name, sizeof(name), "%s", sounds[i]);
		char *dot = strrchr(name, '.');
		if (dot) *dot = 0;
		if (!strcmp(sounds[i], edit.sound)) zmc_accent(cr);
		else white(cr);
		zmc_text(cr, name, FONT_SEMILIGHT, 24, MARGIN, y, 0);
		zmc_add_hit(0, y - 6, zmc.width, 50, ACT_SOUND, i);
		y += 50;
	}
	return y + 30;
}

/* ---- world clock ------------------------------------------------------------ */

#define MAX_ZONES 24
static char zones[MAX_ZONES][64];
static int n_zones;
static int zone_menu_for = -1;
#define MAX_ALL_ZONES 600
static char all_zones[MAX_ALL_ZONES][48];
static int n_all_zones;
static char zone_query[48];
static int matches[60], n_matches;

static void zones_load(void) {
	if (sample) return;
	char path[512], line[96];
	clock_path(path, sizeof(path), "zones");
	n_zones = 0;
	FILE *f = fopen(path, "r");
	while (f && n_zones < MAX_ZONES && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#' && !strstr(line, "..")) snprintf(zones[n_zones++], 64, "%s", line);
	}
	if (f) fclose(f);
}

static void zones_save(void) {
	char path[512], tmp[520];
	clock_path(path, sizeof(path), "zones");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	for (int i = 0; i < n_zones; i++) fprintf(f, "%s\n", zones[i]);
	if (fclose(f) == 0) rename(tmp, path);
}

static int zone_cmp(const void *a, const void *b) {
	const char *x = strrchr(a, '/'), *y = strrchr(b, '/');
	return strcasecmp(x ? x + 1 : a, y ? y + 1 : b);
}

static void all_zones_load(void) {
	if (n_all_zones) return;
	FILE *f = fopen("/usr/share/zoneinfo/zone1970.tab", "r");
	if (!f) f = fopen("/usr/share/zoneinfo/zone.tab", "r");
	char line[512];
	while (f && n_all_zones < MAX_ALL_ZONES && fgets(line, sizeof(line), f)) {
		if (line[0] == '#') continue;
		char cc[64], coord[64], tz[64];
		if (sscanf(line, "%63s %63s %63s", cc, coord, tz) == 3) snprintf(all_zones[n_all_zones++], 48, "%s", tz);
	}
	if (f) fclose(f);
	static const char *const extra[] = { "UTC" };
	if (n_all_zones < MAX_ALL_ZONES) snprintf(all_zones[n_all_zones++], 48, "%s", extra[0]);
	qsort(all_zones, (size_t)n_all_zones, sizeof(all_zones[0]), zone_cmp);
}

/* "Los Angeles" from "America/Los_Angeles" */
static void zone_city(const char *zone, char *out, size_t size) {
	const char *slash = strrchr(zone, '/');
	snprintf(out, size, "%s", slash ? slash + 1 : zone);
	for (char *p = out; *p; p++)
		if (*p == '_') *p = ' ';
}

/* the local time in `zone` (and its offset from ours, in minutes) */
static void zone_time(const char *zone, time_t now, struct tm *out) {
	char *old = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
	setenv("TZ", zone, 1);
	tzset();
	localtime_r(&now, out);
	if (old) {
		setenv("TZ", old, 1);
		free(old);
	} else {
		unsetenv("TZ");
	}
	tzset();
}

static void zone_matches(void) {
	n_matches = 0;
	char q[48];
	snprintf(q, sizeof(q), "%s", zone_query);
	for (char *p = q; *p; p++)
		if (*p == ' ') *p = '_';
	for (int i = 0; i < n_all_zones && n_matches < 60; i++)
		if (!q[0] || strcasestr(all_zones[i], q)) matches[n_matches++] = i;
}

static double draw_world(cairo_t *cr) {
	double y = tab_header(cr, P_WORLD);
	time_t now = time(NULL);
	struct tm here;
	localtime_r(&now, &here);
	bool h24 = zt_clock_24h();
	double menu_at = -1;
	for (int i = -1; i < n_zones; i++) {
		struct tm tm;
		char city[64], detail[96], t[24];
		if (i < 0) {
			tm = here;
			snprintf(city, sizeof(city), "Here");
			const char *tzname_now = here.tm_zone ? here.tm_zone : "";
			snprintf(detail, sizeof(detail), "Local time, %s", tzname_now);
		} else {
			zone_time(zones[i], now, &tm);
			zone_city(zones[i], city, sizeof(city));
			long diff = (tm.tm_gmtoff - here.tm_gmtoff) / 60;
			int day = (tm.tm_year - here.tm_year) * 400 + tm.tm_yday - here.tm_yday;
			const char *when = day > 0 ? "Tomorrow" : day < 0 ? "Yesterday" : "Today";
			if (diff == 0) snprintf(detail, sizeof(detail), "%s, same time", when);
			else if (diff % 60) snprintf(detail, sizeof(detail), "%s, %+ld:%02ld h", when, diff / 60, labs(diff % 60));
			else snprintf(detail, sizeof(detail), "%s, %+ld h", when, diff / 60);
		}
		zt_clock_text(&tm, h24, t, sizeof(t));
		double tw = zmc_text_width(t, FONT_LIGHT, 36);
		white(cr);
		zmc_text(cr, t, FONT_LIGHT, 36, zmc.width - MARGIN - tw, y - 2, 0);
		zmc_text_line(cr, city, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN - tw - 12);
		grey(cr);
		zmc_text_line(cr, detail, FONT_REGULAR, 13.5, MARGIN, y + 34, zmc.width - 2 * MARGIN - tw - 12);
		if (i >= 0) {
			zmc_add_hit(0, y - 4, zmc.width, 66, ACT_ZONE_MENU, i);
			if (i == zone_menu_for) menu_at = y + 62;
		}
		y += 74;
	}
	white(cr);
	zmc_round_button(cr, zmc.width / 2, y + 6, "add", "add", ACT_ZONE_ADD, 0);
	y += 110;
	if (zone_menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "remove" };
		zmc_menu(cr, menu_at, items, 1, ACT_ZONE_MENU, ACT_ZONE_MENU_CLOSE);
	}
	return y;
}

static double draw_zones(cairo_t *cr) {
	double y = zmc_header(cr, "add a city");
	y = zmc_textbox(cr, y, "City or region", zone_query, true, ACT_ZONE_QUERY, 0) + 10;
	for (int k = 0; k < n_matches; k++) {
		const char *z = all_zones[matches[k]];
		char city[64];
		zone_city(z, city, sizeof(city));
		y = zmc_entry(cr, y, city, z, ACT_ZONE_PICK, k);
	}
	return y + 30;
}

/* ---- timer ------------------------------------------------------------------- */

static struct clock_timer timer;
static int t_h, t_m = 5, t_s;
static long timer_shown = -1;

static int64_t timer_left(void) {
	if (timer.state == CLOCK_TIMER_RUNNING) {
		int64_t left = timer.value_ms - clock_now_ms();
		return left > 0 ? left : 0;
	}
	if (timer.state == CLOCK_TIMER_PAUSED) return timer.value_ms;
	return timer.total_ms;
}

static void duration_text(int64_t ms, char *out, size_t size, bool tenths) {
	long s = (long)(ms / 1000);
	if (s >= 3600) snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
	else if (tenths) snprintf(out, size, "%02ld:%02ld.%01ld", s / 60, s % 60, (long)(ms % 1000 / 100));
	else snprintf(out, size, "%02ld:%02ld", s / 60, s % 60);
}

static void timer_store(void) {
	if (!sample) clock_timer_save(&timer);
}

static double draw_timer(cairo_t *cr) {
	double y = tab_header(cr, P_TIMER);
	n_drums = 0;
	if (!sample) clock_timer_load(&timer);
	double w = zmc.width - 2 * MARGIN;
	if (timer.state == CLOCK_TIMER_IDLE) {
		double col = (w - 2 * 12) / 3;
		static const char *const caps[] = { "hours", "minutes", "seconds" };
		int *v[] = { &t_h, &t_m, &t_s };
		int n[] = { 24, 60, 60 };
		for (int i = 0; i < 3; i++) {
			double x = MARGIN + i * (col + 12);
			grey(cr);
			zmc_text_centred(cr, caps[i], FONT_REGULAR, 13, x + col / 2, y);
			drum(cr, x, y + 22, col, v[i], n[i], false);
		}
		y += 22 + 3 * DRUM_ROW + 30;
		zmc_button(cr, MARGIN, y, w, "start", ACT_T_START, 0);
		return y + 80;
	}
	int64_t left = timer_left();
	char text[32];
	duration_text(left + 999, text, sizeof(text), false);
	white(cr);
	zmc_text_centred(cr, text, FONT_LIGHT, 80, zmc.width / 2, y + 20);
	y += 150;
	/* how much is left, as a bar */
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
	cairo_rectangle(cr, MARGIN, y, w, 6);
	cairo_fill(cr);
	zmc_accent(cr);
	double share = timer.total_ms > 0 ? (double)left / (double)timer.total_ms : 0;
	cairo_rectangle(cr, MARGIN, y, w * (share > 1 ? 1 : share), 6);
	cairo_fill(cr);
	y += 22;
	char total[32], line[64];
	duration_text(timer.total_ms, total, sizeof(total), false);
	snprintf(line, sizeof(line), "%s timer%s", total, timer.state == CLOCK_TIMER_PAUSED ? ", paused" : "");
	grey(cr);
	zmc_text(cr, line, FONT_REGULAR, 14, MARGIN, y, 0);
	y += 50;
	double half = (w - 12) / 2;
	zmc_button(cr, MARGIN, y, half, timer.state == CLOCK_TIMER_RUNNING ? "pause" : "resume", ACT_T_PAUSE, 0);
	zmc_button(cr, MARGIN + half + 12, y, half, "reset", ACT_T_RESET, 0);
	y += 56;
	zmc_button(cr, MARGIN, y, w, "+1 minute", ACT_T_PLUS, 0);
	return y + 80;
}

/* ---- stopwatch --------------------------------------------------------------- */

#define MAX_LAPS 99
static struct {
	bool running;
	int64_t since;		/* boot time, ms: when it last started */
	int64_t before;		/* what it had run before that */
	int64_t laps[MAX_LAPS];	/* the total at each lap */
	int n_laps;
} sw;

static int64_t boot_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t sw_total(void) {
	return sw.before + (sw.running ? boot_ms() - sw.since : 0);
}

static void sw_save(void) {
	if (sample) return;
	char path[512], tmp[520];
	clock_path(path, sizeof(path), "stopwatch");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%d %lld %lld\n", sw.running, (long long)sw.since, (long long)sw.before);
	for (int i = 0; i < sw.n_laps; i++) fprintf(f, "%lld\n", (long long)sw.laps[i]);
	if (fclose(f) == 0) rename(tmp, path);
}

static void sw_load(void) {
	char path[512];
	clock_path(path, sizeof(path), "stopwatch");
	FILE *f = fopen(path, "r");
	int running;
	long long since, before, lap;
	if (f && fscanf(f, "%d %lld %lld", &running, &since, &before) == 3) {
		sw.running = running;
		sw.since = since;
		sw.before = before;
		while (sw.n_laps < MAX_LAPS && fscanf(f, "%lld", &lap) == 1) sw.laps[sw.n_laps++] = lap;
		if (sw.since > boot_ms()) memset(&sw, 0, sizeof(sw));	/* from before a restart */
	}
	if (f) fclose(f);
}

static double draw_stop(cairo_t *cr) {
	double y = tab_header(cr, P_STOP);
	double w = zmc.width - 2 * MARGIN;
	char text[32];
	duration_text(sw_total(), text, sizeof(text), true);
	white(cr);
	zmc_text_centred(cr, text, FONT_LIGHT, 72, zmc.width / 2, y + 20);
	y += 140;
	double half = (w - 12) / 2;
	zmc_button(cr, MARGIN, y, half, sw.running ? "stop" : sw_total() ? "resume" : "start", ACT_S_GO, 0);
	zmc_button(cr, MARGIN + half + 12, y, half, sw.running ? "lap" : "reset", ACT_S_LAP, 0);
	y += 70;
	for (int i = sw.n_laps - 1; i >= 0; i--) {
		char lap[32], total[32], name[16];
		duration_text(sw.laps[i] - (i ? sw.laps[i - 1] : 0), lap, sizeof(lap), true);
		duration_text(sw.laps[i], total, sizeof(total), true);
		snprintf(name, sizeof(name), "Lap %d", i + 1);
		white(cr);
		zmc_text(cr, name, FONT_SEMILIGHT, 20, MARGIN, y, 0);
		zmc_text(cr, lap, FONT_SEMILIGHT, 20, MARGIN + w * 0.38, y, 0);
		grey(cr);
		double tw = zmc_text_width(total, FONT_REGULAR, 16);
		zmc_text(cr, total, FONT_REGULAR, 16, zmc.width - MARGIN - tw, y + 3, 0);
		y += 40;
	}
	return y + 30;
}

/* ---- the ring screen ---------------------------------------------------------- */

static struct {
	char kind[16], label[CLOCK_LABEL_MAX], when[16];
	int id, snooze;
} rung;

static bool ring_read(void) {
	if (sample) return true;
	char path[300], line[128];
	clock_runtime_path(path, sizeof(path), "ztclock.ring");
	FILE *f = fopen(path, "r");
	if (!f) return false;
	char *fields[5] = { rung.kind, rung.label, rung.when, NULL, NULL };
	size_t sizes[3] = { sizeof(rung.kind), sizeof(rung.label), sizeof(rung.when) };
	int i = 0;
	while (i < 5 && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (i == 0) snprintf(fields[0], sizes[0], "%s", line);
		else if (i == 1) rung.id = atoi(line);
		else if (i == 2) snprintf(rung.label, sizeof(rung.label), "%s", line);
		else if (i == 3) snprintf(rung.when, sizeof(rung.when), "%s", line);
		else rung.snooze = atoi(line);
		i++;
	}
	fclose(f);
	return i >= 4;
}

static double draw_ring(cairo_t *cr) {
	cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
	cairo_paint(cr);
	double w = zmc.width - 2 * MARGIN, y = 34;
	bool is_timer = !strcmp(rung.kind, "timer");
	zmc_accent(cr);
	zmc_text(cr, is_timer ? "TIMER" : "ALARM", FONT_REGULAR, 16, MARGIN, y, 0);
	y += 40;
	char now[24];
	zt_clock_text_now(time(NULL), now, sizeof(now));
	white(cr);
	zmc_text(cr, now, FONT_LIGHT, 88, MARGIN - 4, y, 0);
	y += 120;
	const char *label = rung.label[0] ? rung.label : is_timer ? "Time's up" : "Alarm";
	y += zmc_text(cr, label, FONT_SEMILIGHT, 32, MARGIN, y, w) + 10;
	if (!is_timer && rung.when[0]) {
		int h = 0, m = 0;
		char t[24], line[64];
		sscanf(rung.when, "%d:%d", &h, &m);
		time_text(h, m, t, sizeof(t));
		snprintf(line, sizeof(line), "Set for %s", t);
		grey(cr);
		zmc_text(cr, line, FONT_REGULAR, 18, MARGIN, y, w);
	}
	double bottom = zmc.height - 170;
	if (!is_timer && rung.snooze > 0) {
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, bottom, w, 70);
		cairo_fill(cr);
		white(cr);
		char s[48];
		snprintf(s, sizeof(s), "snooze %d min", rung.snooze);
		zmc_text_centred(cr, s, FONT_SEMILIGHT, 26, zmc.width / 2, bottom + 16);
		zmc_add_hit(MARGIN, bottom, w, 70, ACT_R_SNOOZE, 0);
	}
	cairo_set_source_rgb(cr, .25, .25, .25);
	cairo_rectangle(cr, MARGIN, bottom + 82, w, 62);
	cairo_fill(cr);
	white(cr);
	zmc_text_centred(cr, "dismiss", FONT_SEMILIGHT, 24, zmc.width / 2, bottom + 96);
	zmc_add_hit(MARGIN, bottom + 82, w, 62, ACT_R_DISMISS, 0);
	return zmc.height;
}

/* ---- drawing and taps --------------------------------------------------------- */

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_ALARMS: return draw_alarms(cr);
	case P_WORLD: return draw_world(cr);
	case P_TIMER: return draw_timer(cr);
	case P_STOP: return draw_stop(cr);
	case P_EDIT: return draw_edit(cr);
	case P_SOUND: return draw_sound(cr);
	case P_ZONES: return draw_zones(cr);
	case P_RING: return draw_ring(cr);
	}
	return 0;
}

static int new_id(void) {
	int id = 1;
	for (int i = 0; i < n_alarms; i++)
		if (alarms[i].id >= id) id = alarms[i].id + 1;
	return id;
}

static void open_edit(int i) {
	label_focus = snooze_open = false;
	editing = i;
	if (i >= 0) {
		edit = alarms[i];
	} else {
		time_t now = time(NULL) + 3600;
		struct tm tm;
		localtime_r(&now, &tm);
		edit = (struct clock_alarm){ .id = new_id(), .on = true, .hour = tm.tm_hour, .minute = 0,
			.days = 0, .vibrate = true, .snooze = CLOCK_SNOOZE_DEFAULT };
		snprintf(edit.sound, sizeof(edit.sound), "%s", CLOCK_SOUND_DEFAULT);
	}
	zmc_go(P_EDIT, 1);
}

static void leave_edit(void) {
	label_focus = false;
	zmc_keyboard(false);
	zmc_go(P_ALARMS, -1);
}

static void activate(int action, int arg) {
	switch (action) {
	case ACT_EDIT:
		open_edit(arg);
		break;
	case ACT_TOGGLE:
		if (arg >= 0 && arg < n_alarms) {
			alarms_load();	/* as it is now: the daemon may have switched one off */
			if (arg < n_alarms) {
				alarms[arg].on = !alarms[arg].on;
				alarms_store();
				unsnooze(alarms[arg].id);
			}
		}
		break;
	case ACT_ADD:
		if (n_alarms < CLOCK_MAX_ALARMS) open_edit(-1);
		break;
	case ACT_MENU:
		if (menu_for >= 0 && menu_for < n_alarms) {
			alarms_load();
			if (menu_for < n_alarms) {
				unsnooze(alarms[menu_for].id);
				memmove(&alarms[menu_for], &alarms[menu_for + 1], sizeof(alarms[0]) * (size_t)(n_alarms - menu_for - 1));
				n_alarms--;
				alarms_store();
			}
		}
		menu_for = -1;
		break;
	case ACT_MENU_CLOSE:
		menu_for = -1;
		break;
	case ACT_DAY:
		edit.days ^= 1u << arg;
		break;
	case ACT_AMPM:
		edit.hour = (edit.hour + 12) % 24;
		break;
	case ACT_LABEL:
		label_focus = true;
		zmc_keyboard(true);
		break;
	case ACT_SOUND_PAGE:
		label_focus = false;
		zmc_keyboard(false);
		sounds_load();
		zmc_go(P_SOUND, 1);
		break;
	case ACT_SOUND:
		if (arg >= 0 && arg < n_sounds) {
			snprintf(edit.sound, sizeof(edit.sound), "%s", sounds[arg]);
			preview(sounds[arg]);
		}
		break;
	case ACT_VIBRATE:
		edit.vibrate = !edit.vibrate;
		break;
	case ACT_SNOOZE:
		if (arg < 0) snooze_open = !snooze_open;
		else {
			edit.snooze = SNOOZES[arg];
			snooze_open = false;
		}
		break;
	case ACT_SAVE: {
		alarms_load();
		edit.on = true;
		int at = -1;
		for (int i = 0; i < n_alarms; i++)
			if (alarms[i].id == edit.id) at = i;
		if (at < 0 && n_alarms < CLOCK_MAX_ALARMS) at = n_alarms++;
		if (at >= 0) alarms[at] = edit;
		alarms_store();
		unsnooze(edit.id);
		zmc_haptic("click");
		leave_edit();
		break;
	}
	case ACT_DELETE: {
		alarms_load();
		int k = 0;
		for (int i = 0; i < n_alarms; i++)
			if (alarms[i].id != edit.id) alarms[k++] = alarms[i];
		n_alarms = k;
		alarms_store();
		unsnooze(edit.id);
		leave_edit();
		break;
	}
	case ACT_CANCEL:
		leave_edit();
		break;
	case ACT_ZONE_ADD:
		all_zones_load();
		zone_query[0] = 0;
		zone_matches();
		zmc_go(P_ZONES, 1);
		zmc_keyboard(true);
		break;
	case ACT_ZONE_QUERY:
		zmc_keyboard(true);
		break;
	case ACT_ZONE_PICK:
		if (arg >= 0 && arg < n_matches && n_zones < MAX_ZONES) {
			snprintf(zones[n_zones++], 64, "%s", all_zones[matches[arg]]);
			zones_save();
		}
		zmc_keyboard(false);
		zmc_go(P_WORLD, -1);
		break;
	case ACT_ZONE_MENU:
		if (zone_menu_for < 0) {
			zone_menu_for = arg;	/* a tap on a city: the menu to remove it */
		} else {
			if (zone_menu_for < n_zones) {
				memmove(&zones[zone_menu_for], &zones[zone_menu_for + 1], sizeof(zones[0]) * (size_t)(n_zones - zone_menu_for - 1));
				n_zones--;
				zones_save();
			}
			zone_menu_for = -1;
		}
		break;
	case ACT_ZONE_MENU_CLOSE:
		zone_menu_for = -1;
		break;
	case ACT_T_START:
		timer.total_ms = ((int64_t)t_h * 3600 + t_m * 60 + t_s) * 1000;
		if (timer.total_ms <= 0) break;
		timer.state = CLOCK_TIMER_RUNNING;
		timer.value_ms = clock_now_ms() + timer.total_ms;
		timer_store();
		break;
	case ACT_T_PAUSE:
		if (timer.state == CLOCK_TIMER_RUNNING) {
			timer.value_ms = timer_left();
			timer.state = CLOCK_TIMER_PAUSED;
		} else if (timer.state == CLOCK_TIMER_PAUSED) {
			timer.value_ms = clock_now_ms() + timer.value_ms;
			timer.state = CLOCK_TIMER_RUNNING;
		}
		timer_store();
		break;
	case ACT_T_RESET:
		timer.state = CLOCK_TIMER_IDLE;
		timer.value_ms = 0;
		timer_store();
		break;
	case ACT_T_PLUS:
		timer.value_ms += 60000;
		timer.total_ms += 60000;
		timer_store();
		break;
	case ACT_S_GO:
		if (sw.running) {
			sw.before = sw_total();
			sw.running = false;
		} else {
			sw.since = boot_ms();
			sw.running = true;
		}
		sw_save();
		break;
	case ACT_S_LAP:
		if (sw.running) {
			if (sw.n_laps < MAX_LAPS) sw.laps[sw.n_laps++] = sw_total();
		} else {
			memset(&sw, 0, sizeof(sw));
		}
		sw_save();
		break;
	case ACT_R_SNOOZE:
		clock_tell("snooze");
		zmc_quit();
		break;
	case ACT_R_DISMISS:
		clock_tell("dismiss");
		zmc_quit();
		break;
	}
	zmc_redraw();
}

static bool hold(int action, int arg) {
	if (action == ACT_EDIT || action == ACT_TOGGLE) {
		menu_for = arg;
		zmc_redraw();
		return true;
	}
	if (action == ACT_ZONE_MENU) {
		zone_menu_for = arg;
		zmc_redraw();
		return true;
	}
	return false;
}

static void slide(int action, int percent, bool done) {
	if (action == ACT_VOLUME && done && !sample) clock_setting_set("volume", percent);
	zmc_redraw();
}

static void typed(const char *s) {
	char *text = zmc.page == P_ZONES ? zone_query : label_focus ? edit.label : NULL;
	size_t size = zmc.page == P_ZONES ? sizeof(zone_query) : sizeof(edit.label);
	if (!text) return;
	size_t l = strlen(text);
	if (s[0] == '\b') {
		while (l > 0 && (text[l - 1] & 0xc0) == 0x80) l--;	/* a whole UTF-8 character */
		if (l > 0) l--;
		text[l] = 0;
	} else if (s[0] == '\n') {
		label_focus = false;
		zmc_keyboard(false);
	} else if (l + strlen(s) < size - 1) {
		strcat(text, s);
	}
	if (zmc.page == P_ZONES) zone_matches();
	zmc_redraw();
}

static bool back(void) {
	if (menu_for >= 0 || zone_menu_for >= 0) {
		menu_for = zone_menu_for = -1;
		zmc_redraw();
		return true;
	}
	if (zmc.page == P_SOUND) {
		preview_stop();
		zmc_go(P_EDIT, -1);
		return true;
	}
	if (zmc.page == P_EDIT) {
		leave_edit();
		return true;
	}
	if (zmc.page == P_ZONES) {
		zmc_keyboard(false);
		zmc_go(P_WORLD, -1);
		return true;
	}
	return false;
}

static bool page_moving(void) {
	return zmc.anim_active || zmc.slide_active || zmc.swiping || zmc.spring;
}

/* ten times a second: what moves on the page in front */
static void tick(void) {
	while (waitpid(-1, NULL, WNOHANG) > 0) {}
	if (page_moving() || drag.drum >= 0) return;
	static long minute_shown = -1;
	time_t now = time(NULL);
	if (zmc.page == P_STOP && sw.running) {
		zmc_redraw();
	} else if (zmc.page == P_TIMER) {
		clock_timer_load(&timer);
		long s = timer.state == CLOCK_TIMER_IDLE ? -2 : (long)((timer_left() + 999) / 1000);
		if (s != timer_shown) {
			timer_shown = s;
			zmc_redraw();
		}
	} else if (zmc.page == P_ALARMS && alarms_changed_on_disk()) {
		alarms_load();
		zmc_redraw();
	} else if ((zmc.page == P_WORLD || zmc.page == P_EDIT || zmc.page == P_ALARMS) && now / 60 != minute_shown) {
		minute_shown = now / 60;
		zmc_redraw();
	}
}

static void ring_tick(void) {
	if (!ring_read()) {
		zmc_quit();	/* stopped elsewhere: snoozed, dismissed or rang its time */
		return;
	}
	static long minute_shown = -1;
	if (time(NULL) / 60 != minute_shown) {
		minute_shown = time(NULL) / 60;
		zmc_redraw();
	}
}

static bool ring_back(void) { return true; }	/* only snooze or dismiss end it */

static const struct zmc_app clock_app = {
	.app_id = "ztclock", .title = "Alarms + Clock", .caps = "ALARMS + CLOCK",
	.render = render, .activate = activate, .back = back, .hold = hold, .slide = slide,
	.text = typed, .touch = touch,
};

static const struct zmc_app ring_app = {
	.app_id = "ztclock-ring", .title = "Alarm", .caps = "ALARM",
	.render = render, .activate = activate, .back = ring_back, .fixed = true,
};

static void make_sample(void) {
	sample = true;
	static const struct clock_alarm made[] = {
		{ 1, true, 6, 45, 0x1f, true, 10, "Ring03-WP8.wav", "Work" },
		{ 2, false, 9, 30, 0x60, true, 10, "Ring07-WP8.wav", "" },
		{ 3, true, 21, 0, 0, false, 5, "Ring01-WP8.wav", "Take the bread out" },
	};
	n_alarms = 3;
	memcpy(alarms, made, sizeof(made));
	n_zones = 3;
	snprintf(zones[0], 64, "Europe/London");
	snprintf(zones[1], 64, "America/New_York");
	snprintf(zones[2], 64, "Asia/Tokyo");
	timer = (struct clock_timer){ CLOCK_TIMER_RUNNING, clock_now_ms() + 263000, 300000 };
	sw.before = 83400;
	sw.n_laps = 2;
	sw.laps[0] = 40100;
	sw.laps[1] = 79900;
	snprintf(rung.kind, sizeof(rung.kind), "alarm");
	snprintf(rung.label, sizeof(rung.label), "Work");
	snprintf(rung.when, sizeof(rung.when), "06:45");
	rung.snooze = 10;
	edit = made[0];
	editing = 0;
}

int main(int argc, char **argv) {
	atexit(preview_stop);
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		make_sample();
		static const char *const names[] = { "alarms", "world", "timer", "stopwatch", "edit", "sound", "zones", "ring" };
		int page = P_ALARMS;
		for (int i = 0; i < 8; i++)
			if (!strcmp(argv[2], names[i])) page = i;
		if (page == P_SOUND) sounds_load();
		if (page == P_ZONES) {
			all_zones_load();
			snprintf(zone_query, sizeof(zone_query), "new");
			zone_matches();
		}
		return zmc_render_png(page == P_RING ? &ring_app : &clock_app, page, 740, argv[3]);
	}
	if (argc == 2 && !strcmp(argv[1], "--ring")) {
		if (!ring_read()) return 0;	/* nothing rings */
		char hello[32];
		snprintf(hello, sizeof(hello), "hello %d", (int)getpid());
		clock_tell(hello);
		zmc_every(250, ring_tick);
		return zmc_run(&ring_app, P_RING);
	}
	alarms_load();
	zones_load();
	sw_load();
	clock_timer_load(&timer);
	if (timer.total_ms > 0) {
		long s = (long)(timer.total_ms / 1000);
		t_h = (int)(s / 3600);
		t_m = (int)(s / 60 % 60);
		t_s = (int)(s % 60);
	}
	zmc_every(100, tick);
	return zmc_run(&clock_app, P_ALARMS);
}
