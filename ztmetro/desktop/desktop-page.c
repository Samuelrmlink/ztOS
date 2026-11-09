// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Settings' "desktop mode" page: see desktop-page.h. Words follow
 * ztmetro/STYLE.md: the title lowercase, switches, pickers and buttons in
 * Sentence case.
 */
#define _GNU_SOURCE
#include "desktop-page.h"
#include "zrnmetrocore.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	ACT_AUTOSTART = DESKTOP_ACTIONS,
	ACT_AUTOSTART_VIRTUAL,
	ACT_UNPLUG,
	ACT_SIZE,
	ACT_SESSION,
	ACT_VIRTUAL,
	ACT_VIEW,
	ACT_MODE,
};

static const char *const sizes[] = { "1280 × 720", "1600 × 900", "1920 × 1080" };
static const char *const size_values[] = { "1280x720", "1600x900", "1920x1080" };
static const char *const unplugs[] = { "Keep them on a virtual screen", "Move them to the phone" };
/* a monitor's resolution (monitor_mode =; "desktop mode" tells the shell now).
 * One the monitor does not have: the shell keeps the monitor's own. */
static const char *const modes[] = { "The monitor's own", "1920 × 1080", "1920 × 1080, 30 Hz",
	"1600 × 900", "1280 × 720" };
static const char *const mode_values[] = { "", "1920x1080@60", "1920x1080@30", "1600x900@60", "1280x720@60" };
#define NMODES 5

static struct {
	bool autostart, autostart_virtual;
	int unplug;		/* 0 keep, 1 phone */
	int size;		/* index in sizes */
	int mode;		/* index in modes */
	char other[256];	/* lines of the file this page doesn't set, kept */
	/* the shell's state */
	bool output, monitor, session;
	char output_name[32];
	int w, h;
	int open_picker;
} pg = { .size = 2 };

static void path(char *out, size_t size, const char *name) {
	const char *home = getenv("HOME");
	snprintf(out, size, "%s/%s", home ? home : "", name);
}

static void load(void) {
	char p[512], line[256];
	path(p, sizeof(p), ".config/ztmetro/desktop");
	pg.autostart = pg.autostart_virtual = false;
	pg.unplug = 0;
	pg.size = 2;
	pg.mode = 0;
	pg.other[0] = 0;
	FILE *f = fopen(p, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char key[64], value[128];
		if (sscanf(line, " %63[^= \t] = %127[^\n]", key, value) != 2) continue;
		bool on = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
		if (!strcmp(key, "autostart")) pg.autostart = on;
		else if (!strcmp(key, "autostart_virtual")) pg.autostart_virtual = on;
		else if (!strcmp(key, "unplug")) pg.unplug = !strcmp(value, "phone");
		else if (!strcmp(key, "monitor_mode")) {
			for (int i = 1; i < NMODES; i++) if (!strcmp(value, mode_values[i])) pg.mode = i;
		} else if (!strcmp(key, "virtual_size")) {
			for (int i = 0; i < 3; i++) if (!strncmp(value, size_values[i], strlen(size_values[i]))) pg.size = i;
		} else if (strlen(pg.other) + strlen(line) + 1 < sizeof(pg.other)) {
			strcat(pg.other, line);	/* scale, xwayland: kept as they are */
		}
	}
	if (f) fclose(f);
}

static void save(void) {
	char p[512], tmp[520];
	path(p, sizeof(p), ".config/ztmetro");
	mkdir(p, 0755);
	path(p, sizeof(p), ".config/ztmetro/desktop");
	snprintf(tmp, sizeof(tmp), "%s.new", p);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "autostart = %s\nautostart_virtual = %s\nunplug = %s\nvirtual_size = %s\n",
		pg.autostart ? "on" : "off", pg.autostart_virtual ? "on" : "off",
		pg.unplug ? "phone" : "keep", size_values[pg.size]);
	if (pg.mode > 0) fprintf(f, "monitor_mode = %s\n", mode_values[pg.mode]);
	fputs(pg.other, f);
	fclose(f);
	rename(tmp, p);
	if (system("pkill -HUP -x ztmetro 2>/dev/null") != 0) {}	/* the shell reads it again */
}

static void state(void) {
	char p[300], line[512];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(p, sizeof(p), "%s/ztmetro-desktop.state", runtime ? runtime : "/tmp");
	pg.output = pg.session = pg.monitor = false;
	FILE *f = fopen(p, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char kind[16];
		if (sscanf(line, "output %31s %15s %d %d", pg.output_name, kind, &pg.w, &pg.h) == 4) {
			pg.output = true;
			pg.monitor = !strcmp(kind, "monitor");
		} else if (!strncmp(line, "session running", 15)) {
			pg.session = true;
		}
	}
	if (f) fclose(f);
}

static void shell_tell(const char *line) {
	char p[300];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(p, sizeof(p), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
	int fd = open(p, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) {}
		close(fd);
	}
}

const char *desktop_page_summary(void) {
	static char s[96];
	load();
	state();
	if (pg.output && pg.session) snprintf(s, sizeof(s), "Running on %s", pg.monitor ? "a monitor" : "a virtual screen");
	else snprintf(s, sizeof(s), "%s", pg.autostart ? "Starts when a monitor is connected" : "Off");
	return s;
}

void desktop_page_arrived(void) {
	load();
	state();
}

double desktop_page_render(cairo_t *cr) {
	state();
	double y = zmc_header(cr, "desktop mode");
	/* where it is now */
	char now[128];
	if (!pg.output) snprintf(now, sizeof(now), "No desktop screen");
	else snprintf(now, sizeof(now), "%s, %d × %d%s", pg.monitor ? "A monitor" : "A virtual screen",
		pg.w, pg.h, pg.session ? "" : ", desktop not started");
	y = zmc_field(cr, y, "Desktop", now);
	y = zmc_switch(cr, y, "Start automatically", pg.autostart, ACT_AUTOSTART);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "When a monitor is connected over USB-C, XFCE starts on it, beside the phone.",
		FONT_REGULAR, 13, ZMC_MARGIN, y - 10, zmc.width - 2 * ZMC_MARGIN) + 8;
	y = zmc_switch(cr, y, "On a virtual screen too", pg.autostart_virtual, ACT_AUTOSTART_VIRTUAL);
	y = zmc_picker(cr, y, "Its apps, when the monitor goes", unplugs, 2, pg.unplug,
		pg.open_picker == ACT_UNPLUG, ACT_UNPLUG);
	y = zmc_picker(cr, y, "Monitor resolution", modes, NMODES, pg.mode, pg.open_picker == ACT_MODE, ACT_MODE);
	y = zmc_picker(cr, y, "Virtual screen size", sizes, 3, pg.size, pg.open_picker == ACT_SIZE, ACT_SIZE);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (pg.output) {
		y = zmc_button(cr, ZMC_MARGIN, y + 6, zmc.width - 2 * ZMC_MARGIN,
			pg.session ? "Stop the desktop" : "Start the desktop", ACT_SESSION, 0);
	}
	if (!pg.monitor) {
		y = zmc_button(cr, ZMC_MARGIN, y + 6, zmc.width - 2 * ZMC_MARGIN,
			pg.output ? "Remove the virtual screen" : "Start on a virtual screen", ACT_VIRTUAL, 0);
	}
	y = zmc_entry(cr, y + 10, "desktop", "See and use the desktop from the phone", ACT_VIEW, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "Apps move between the phone and the desktop: pull an app's card down in the app "
		"switcher, or use the phone button in a window's title bar.", FONT_REGULAR, 13, ZMC_MARGIN, y,
		zmc.width - 2 * ZMC_MARGIN);
	return y + 30;
}

void desktop_page_activate(int action, int arg) {
	if ((action == ACT_UNPLUG || action == ACT_SIZE || action == ACT_MODE) && arg < 0) {
		pg.open_picker = pg.open_picker == action ? 0 : action;
		return;
	}
	pg.open_picker = 0;
	switch (action) {
	case ACT_AUTOSTART: pg.autostart = !pg.autostart; save(); break;
	case ACT_AUTOSTART_VIRTUAL: pg.autostart_virtual = !pg.autostart_virtual; save(); break;
	case ACT_UNPLUG: if (arg >= 0 && arg < 2) { pg.unplug = arg; save(); } break;
	case ACT_SIZE: if (arg >= 0 && arg < 3) { pg.size = arg; save(); } break;
	case ACT_MODE:
		if (arg >= 0 && arg < NMODES) {
			char line[64];
			pg.mode = arg;
			save();
			snprintf(line, sizeof(line), "desktop mode %s\n", arg ? mode_values[arg] : "preferred");
			shell_tell(line);
			usleep(300000);
		}
		break;
	case ACT_SESSION: shell_tell(pg.session ? "desktop stop\n" : "desktop start\n"); usleep(300000); break;
	case ACT_VIRTUAL:
		if (pg.output) {
			shell_tell("desktop remove\n");
		} else {
			char line[64];
			snprintf(line, sizeof(line), "desktop add %s\ndesktop start\n", size_values[pg.size]);
			shell_tell(line);
		}
		usleep(300000);	/* the shell's state file, before this page is drawn again */
		break;
	case ACT_VIEW:
		if (fork() == 0) {
			setsid();
			execlp("ztdesk", "ztdesk", (char *)NULL);
			_exit(127);
		}
		break;
	}
}
