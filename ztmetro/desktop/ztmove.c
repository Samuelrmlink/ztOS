// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztMove ("Move Apps"): running apps between the phone and the desktop, with
 * their windows and state, without restarting them. One app for both sides.
 * It knows which side it is on (zt_environment_v1, zmc_on_desktop()) and
 * changes as it is moved itself:
 *
 *   - first, the apps on the other side: a tap (a click) brings one here;
 *   - then the apps on this side: a tap sends one over.
 *
 * It asks the shell through the control pipe, as the switcher and the title
 * bar's window menu do: "desktop move APP" (phone -> desktop) and "desktop
 * phone APP" (desktop -> phone). What is where comes from the shell's own
 * files: ztmetro-desktop.state (the desktop's windows) and ztmetro-apps (the
 * phone's). X11 windows stay on the desktop: Metro draws Wayland windows only.
 * The phone's controller, ztdesk, stays on the phone.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zrnmetrocore.h"

#define MAX_APPS 32
enum { ACT_BRING = 1, ACT_SEND, ACT_START };

struct app {
	char id[96];
	char name[96];
	char title[128];
};

static struct {
	struct app desk[MAX_APPS], phone[MAX_APPS];
	int n_desk, n_phone;
	bool desktop_up;	/* there is a desktop (an output for it) */
	char sig[2048];		/* what was last shown, to redraw only on a change */
} m;

static void runtime_path(char *out, size_t size, const char *name) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(out, size, "%s/%s", runtime ? runtime : "/tmp", name);
}

static void shell_tell(const char *line) {
	char p[300];
	runtime_path(p, sizeof(p), "ztmetro.ctl");
	int fd = open(p, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) {}
		close(fd);
	}
}

/* Name= of the app's .desktop file, or its app_id */
static void app_name(const char *id, char *out, size_t size) {
	snprintf(out, size, "%s", id);
	const char *home = getenv("HOME");
	char dirs[5][256];
	snprintf(dirs[0], sizeof(dirs[0]), "%s/.local/share/applications", home ? home : "");
	snprintf(dirs[1], sizeof(dirs[1]), "/var/lib/flatpak/exports/share/applications");
	snprintf(dirs[2], sizeof(dirs[2]), "/usr/local/share/applications");
	snprintf(dirs[3], sizeof(dirs[3]), "/usr/share/applications");
	dirs[4][0] = 0;
	for (int i = 0; dirs[i][0]; i++) {
		char p[512], line[256];
		snprintf(p, sizeof(p), "%s/%s.desktop", dirs[i], id);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "Name=", 5)) {
				line[strcspn(line, "\n")] = 0;
				snprintf(out, size, "%s", line + 5);
				break;
			}
		}
		fclose(f);
		return;
	}
}

static bool ours(const char *id) {
	return !strcmp(id, "ztmove") || !strcmp(id, "ztdesk");
}

static void scan(void) {
	char p[300], line[512];
	m.n_desk = m.n_phone = 0;
	m.desktop_up = false;
	runtime_path(p, sizeof(p), "ztmetro-desktop.state");
	FILE *f = fopen(p, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char kind[8], id[96];
		if (!strncmp(line, "output ", 7) && strncmp(line, "output none", 11)) {
			m.desktop_up = true;
		} else if (sscanf(line, "window %7s %95s", kind, id) == 2 && !strcmp(kind, "xdg") && strcmp(id, "-") &&
				!ours(id) && m.n_desk < MAX_APPS) {
			struct app *a = &m.desk[m.n_desk++];
			snprintf(a->id, sizeof(a->id), "%s", id);
			app_name(id, a->name, sizeof(a->name));
			char *tab = strchr(line, '\t');
			snprintf(a->title, sizeof(a->title), "%s", tab ? tab + 1 : "");
			a->title[strcspn(a->title, "\n")] = 0;
		}
	}
	if (f) fclose(f);
	runtime_path(p, sizeof(p), "ztmetro-apps");
	f = fopen(p, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!line[0] || ours(line) || m.n_phone >= MAX_APPS) continue;
		struct app *a = &m.phone[m.n_phone++];
		snprintf(a->id, sizeof(a->id), "%s", line);
		app_name(line, a->name, sizeof(a->name));
		a->title[0] = 0;
	}
	if (f) fclose(f);
}

/* the rows of one side: the app's name, and what a tap does */
static double rows(cairo_t *cr, double y, const struct app *apps, int n, const char *verb, int action) {
	for (int i = 0; i < n; i++) {
		char detail[200];
		if (apps[i].title[0] && strcmp(apps[i].title, apps[i].name)) {
			snprintf(detail, sizeof(detail), "%s · %s", verb, apps[i].title);
		} else {
			snprintf(detail, sizeof(detail), "%s", verb);
		}
		double y0 = y;
		y = zmc_entry(cr, y, apps[i].name, detail, action, i);
		if (getenv("ZTMOVE_TRACE")) {
			/* for tests: where each row is, in the window (points) */
			fprintf(stderr, "ztmove: row %s %s %.0f %.0f\n", action == ACT_BRING ? "bring" : "send",
				apps[i].id, y0, y);
		}
	}
	return y;
}

static double note(cairo_t *cr, double y, const char *s) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, s, FONT_REGULAR, 14, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 14;
	cairo_set_source_rgb(cr, 1, 1, 1);
	return y;
}

static double section(cairo_t *cr, double y, const char *s) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, s, FONT_SEMIBOLD, 13, ZMC_MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	return y + 30;
}

static double render(cairo_t *cr, int page) {
	scan();
	double y = zmc_header(cr, "move apps");
	bool desk = zmc_on_desktop();
	if (getenv("ZTMOVE_TRACE")) fprintf(stderr, "ztmove: page on the %s\n", desk ? "desktop" : "phone");
	const char *tap = desk ? "Click" : "Tap";
	if (!m.desktop_up) {
		y = note(cr, y, "No desktop is running. Plug in a monitor, or start one on a virtual screen.");
		cairo_set_source_rgb(cr, 1, 1, 1);
		return zmc_button(cr, ZMC_MARGIN, y + 6, zmc.width - 2 * ZMC_MARGIN, "Start a virtual desktop",
			ACT_START, 0) + 20;
	}
	/* the other side first: what can come here */
	const struct app *there = desk ? m.phone : m.desk, *here = desk ? m.desk : m.phone;
	int n_there = desk ? m.n_phone : m.n_desk, n_here = desk ? m.n_desk : m.n_phone;
	y = section(cr, y, desk ? "ON THE PHONE" : "ON THE DESKTOP");
	char verb[64];
	snprintf(verb, sizeof(verb), "%s to bring it here", tap);
	y = n_there ? rows(cr, y, there, n_there, verb, ACT_BRING) :
		note(cr, y, desk ? "No apps are open on the phone." : "No windows are open on the desktop.");
	y = section(cr, y + 8, desk ? "ON THIS DESKTOP" : "ON THIS PHONE");
	snprintf(verb, sizeof(verb), "%s to send it to the %s", tap, desk ? "phone" : "desktop");
	y = n_here ? rows(cr, y, here, n_here, verb, ACT_SEND) :
		note(cr, y, desk ? "No other windows are open here." : "No other apps are open here.");
	y = note(cr, y + 8, "Apps keep running as they move: nothing is closed or started again. "
		"X11 apps stay on the desktop.");
	return y + 20;
}

static void activate(int action, int arg) {
	bool desk = zmc_on_desktop();
	char line[160];
	if (action == ACT_START) {
		shell_tell("desktop add\ndesktop start\n");
		return;
	}
	/* bring here = from the other side; send = from this side */
	bool to_desktop = (action == ACT_BRING) == desk;
	const struct app *list = to_desktop ? m.phone : m.desk;
	int n = to_desktop ? m.n_phone : m.n_desk;
	if (arg < 0 || arg >= n) return;
	snprintf(line, sizeof(line), "desktop %s %s\n", to_desktop ? "move" : "phone", list[arg].id);
	shell_tell(line);
	usleep(200000);	/* the shell's files, before the page is drawn again */
}

/* the files change by themselves: draw again when what they say has */
static void tick(void) {
	char p[300], sig[2048] = "", buf[1024];
	const char *names[] = { "ztmetro-desktop.state", "ztmetro-apps" };
	for (int i = 0; i < 2; i++) {
		runtime_path(p, sizeof(p), names[i]);
		FILE *f = fopen(p, "r");
		size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
		if (f) fclose(f);
		buf[n] = 0;
		strncat(sig, buf, sizeof(sig) - strlen(sig) - 1);
	}
	if (strcmp(sig, m.sig)) {
		snprintf(m.sig, sizeof(m.sig), "%s", sig);
		zmc_redraw();
	}
}

static void environment(bool desktop) {
	(void)desktop;	/* the page says "tap" or "click", and which side is which */
}

static const struct zmc_app app = {
	.app_id = "ztmove", .title = "Move Apps", .caps = "DESKTOP MODE",
	.render = render, .activate = activate, .environment = environment,
};

int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "--render")) {
		return zmc_render_png(&app, 0, 900, argv[2]);
	}
	zmc_every(1000, tick);
	return zmc_run(&app, 0);
}
