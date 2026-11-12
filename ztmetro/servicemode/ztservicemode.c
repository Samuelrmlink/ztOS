// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ServiceMode: Samsung's *#0011# field test and the other modem menus, to
 * look at. The menus are the modem's own; ztoscorenetworkingd enters them,
 * passes on the keys it allows and writes each screen the modem sends to
 * /run/ztoscorenetworking/svc (modemd/servicemode.h has the rules). This app
 * shows that file as it changes (inotify, no polling), and taps a menu line
 * or a key of the keypad to ask for the next screen. Nothing here changes a
 * setting: the daemon refuses anything that would.
 *
 *	ztservicemode [CODE]		CODE: 0011, 197328640, 2263, 9090
 *	ztservicemode --render home|screen PNG	(ZT_SVC_FILE: a screen file to draw)
 *
 * ZT_SVC_REDACT=1 hides cell and subscriber identities, for screenshots.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "zrnmetrocore.h"

#define CTL		"/run/ztoscorenetworking.ctl"
#define SVC_DIR		"/run/ztoscorenetworking"
#define SVC_FILE	SVC_DIR "/svc"
#define MAX_LINES	100
#define PING_MS		30000	/* the daemon leaves after 90 s of nothing */

enum page { PAGE_HOME, PAGE_SCREEN };
enum action { ACT_CODE = 1, ACT_KEY, ACT_BACK, ACT_END, ACT_NETINFO };

static const struct { const char *code, *title, *detail; } codes[] = {
	{ "0011", "field test", "*#0011#: serving cell, band, EARFCN, signal, neighbours" },
	{ "197328640", "service menu", "*#197328640#: the modem's main menu; menus only" },
	{ "2263", "band selection", "*#2263#: to look at; nothing can be chosen" },
	{ "9090", "diag control", "*#9090#: to look at; nothing can be chosen" },
};
#define N_CODES (int)(sizeof(codes) / sizeof(codes[0]))

static struct {
	char state[8], code[16], what[48], note[96], allow[12];
	int keys, waiting;
	unsigned screens;
	char net_mode[8], ext_band[136], pci_lock[32];
	int n;
	char lines[MAX_LINES][48];
} svc;

static const char *svc_path = SVC_FILE;
static int current = -1;	/* the code entered, index in codes[] */
static int inotify_fd = -1;
static bool redact;

static void tell(const char *line)
{
	int fd = open(CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, line, strlen(line)) < 0) {
		/* the daemon is not running: the page says nothing came */
	}
	close(fd);
}

/* For screenshots: on a line naming an identity, every word with a digit in
 * it after the name becomes x's; anywhere, a run of 6 or more hex digits too. */
static void redact_line(char *l)
{
	static const char *const ids[] = { "CELL", "CID", "ECI", "ECGI", "TAC", "LAC", "RAC", "IMSI", "IMEI",
		"GUTI", "TMSI", "MSISDN", "ICCID", "GCI", "NCI", "ENB", "PLMN", "MCC", "MNC", "SID", "NID", "BSIC" };
	char up[64];
	size_t n = 0;
	for (; l[n] && n < sizeof(up) - 1; n++)
		up[n] = (char)toupper((unsigned char)l[n]);
	up[n] = 0;
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		char *at = strstr(up, ids[i]);
		if (at) {
			for (char *p = l + (at - up) + strlen(ids[i]); *p;) {
				while (*p && !isalnum((unsigned char)*p))
					p++;
				char *w = p;
				bool digit = false;
				for (; isalnum((unsigned char)*p); p++)
					digit = digit || isdigit((unsigned char)*p);
				if (digit)
					memset(w, 'x', (size_t)(p - w));
			}
			break;
		}
	}
	for (char *p = l; *p;) {
		char *s = p;
		while (isxdigit((unsigned char)*p))
			p++;
		if (p - s >= 6)
			memset(s, 'x', (size_t)(p - s));
		if (*p)
			p++;
	}
}

static void svc_read(void)
{
	FILE *f = fopen(svc_path, "r");
	memset(&svc, 0, sizeof(svc));
	if (!f)
		return;
	char l[256];
	while (fgets(l, sizeof(l), f)) {
		l[strcspn(l, "\n")] = 0;
		char *v = strchr(l, '=');
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(l, "line") && svc.n < MAX_LINES) {
			snprintf(svc.lines[svc.n], sizeof(svc.lines[0]), "%s", v);
			if (redact)
				redact_line(svc.lines[svc.n]);
			svc.n++;
		}
#define S(k) else if (!strcmp(l, #k)) snprintf(svc.k, sizeof(svc.k), "%s", v)
		S(state); S(code); S(what); S(note); S(allow); S(net_mode); S(ext_band); S(pci_lock);
#undef S
		else if (!strcmp(l, "keys")) svc.keys = atoi(v);
		else if (!strcmp(l, "waiting")) svc.waiting = atoi(v);
		else if (!strcmp(l, "screens")) svc.screens = (unsigned)atoi(v);
	}
	fclose(f);
}

static void svc_changed(int fd, void *data)
{
	char buf[4096];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	svc_read();
	zmc_redraw();
}

static void watch(bool on)
{
	if (on && inotify_fd < 0) {
		inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		/* the daemon renames svc.new over svc */
		if (inotify_fd >= 0 && inotify_add_watch(inotify_fd, SVC_DIR, IN_MOVED_TO | IN_CLOSE_WRITE) >= 0)
			zmc_watch_fd(inotify_fd, svc_changed, NULL);
	} else if (!on && inotify_fd >= 0) {
		zmc_watch_fd(inotify_fd, NULL, NULL);
		close(inotify_fd);
		inotify_fd = -1;
	}
}

static void ping(void)
{
	if (current >= 0)
		tell("svc ping\n");
}

static void leave(void)
{
	if (current >= 0)
		tell("svc end\n");
	current = -1;
	zmc_every(0, NULL);
	watch(false);
}

static void end_at_exit(void)
{
	if (current >= 0)
		tell("svc end\n");
}

static void enter(int i)
{
	char line[64];
	current = i;
	watch(true);
	snprintf(line, sizeof(line), "svc enter %s\n", codes[i].code);
	tell(line);
	zmc_every(PING_MS, ping);
}

/* The character in a line's brackets, as the stock app takes it: "[1]" and
 * "[ 1]" are '1'. 0: not a menu line. */
static char line_key(const char *l)
{
	const char *open = strchr(l, '['), *close = open ? strchr(open, ']') : NULL;
	if (!close)
		return 0;
	const char *in = open + 1;
	while (in < close && *in == ' ')
		in++;
	return close - in == 1 && isdigit((unsigned char)*in) ? *in : 0;
}

static double render_home(cairo_t *cr)
{
	double y = zmc_header(cr, "service mode");
	for (int i = 0; i < N_CODES; i++)
		y = zmc_entry(cr, y, codes[i].title, codes[i].detail, ACT_CODE, i);
	y = zmc_entry(cr, y, "network settings", "read the RAT mask, LTE bands and cell lock", ACT_NETINFO, 0);
	if (svc.net_mode[0] || svc.ext_band[0] || svc.pci_lock[0]) {
		y += 8;
		y = zmc_field(cr, y, "RAT mask (080a)", svc.net_mode[0] ? svc.net_mode : "-");
		y = zmc_field(cr, y, "LTE bands (0830)", svc.ext_band[0] ? svc.ext_band : "-");
		y = zmc_field(cr, y, "PCI/EARFCN lock (0833)", svc.pci_lock[0] ? svc.pci_lock : "-");
	}
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += 10 + zmc_text(cr, "The modem's own menus. Only looking: menus that change bands, "
		"the network or the modem's memory are refused by the modem daemon.",
		FONT_REGULAR, 13, ZMC_MARGIN, y + 10, zmc.width - 2 * ZMC_MARGIN);
	return y + 30;
}

static double render_screen(cairo_t *cr)
{
	const char *title = current >= 0 ? codes[current].title : svc.what[0] ? svc.what : "service mode";
	double y = zmc_header(cr, title);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	char status[160];
	snprintf(status, sizeof(status), "%s%s  ·  %s", strcmp(svc.state, "on") ? "not in service mode" : "in *#",
		strcmp(svc.state, "on") ? "" : (svc.code[0] ? svc.code : "?"), svc.note[0] ? svc.note : "waiting");
	y += zmc_text(cr, status, FONT_REGULAR, 12.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 10;
	if (!svc.n) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "waiting for the modem's screen", FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
		y += 40;
	}
	for (int i = 0; i < svc.n; i++) {
		/* live only where the daemon would take it */
		char k = line_key(svc.lines[i]);
		if (k && !strchr(svc.allow, k))
			k = 0;
		if (k) {
			zmc_accent(cr);
			zmc_add_hit(0, y - 3, zmc.width, 26, ACT_KEY, k);
		} else {
			cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
		}
		zmc_text_line(cr, svc.lines[i][0] ? svc.lines[i] : " ", FONT_MONO, 12.5, ZMC_MARGIN, y,
			zmc.width - 2 * ZMC_MARGIN);
		y += k ? 26 : 19;
	}
	y += 16;
	/* the keypad, as stock's "Key Input" without typed values: digits and back */
	double gap = 8, w = (zmc.width - 2 * ZMC_MARGIN - 2 * gap) / 3;
	if (svc.keys) {
		static const char *const pad = "123456789";
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				char label[2] = { pad[r * 3 + c], 0 };	/* the daemon says no to the rest */
				zmc_button(cr, ZMC_MARGIN + c * (w + gap), y, w, label, ACT_KEY, label[0]);
			}
			y += 48;
		}
		zmc_button(cr, ZMC_MARGIN + (w + gap), y, w, "0", ACT_KEY, '0');
	}
	zmc_button(cr, ZMC_MARGIN, y, w, "back", ACT_BACK, 0);
	y = zmc_button(cr, ZMC_MARGIN + 2 * (w + gap), y, w, "end", ACT_END, 0);
	return y + 20;
}

static double render(cairo_t *cr, int page)
{
	return page == PAGE_SCREEN ? render_screen(cr) : render_home(cr);
}

static void activate(int action, int arg)
{
	char line[32];
	switch (action) {
	case ACT_CODE:
		enter(arg);
		svc.n = 0;
		zmc_go(PAGE_SCREEN, 1);
		break;
	case ACT_KEY:
		snprintf(line, sizeof(line), "svc key %c\n", arg);
		tell(line);
		break;
	case ACT_BACK:
		tell("svc back\n");
		break;
	case ACT_END:
		leave();
		zmc_go(PAGE_HOME, -1);
		break;
	case ACT_NETINFO:
		watch(true);
		tell("svc netinfo\n");
		break;
	}
}

static bool back(void)
{
	if (zmc.page == PAGE_SCREEN) {
		leave();
		zmc_go(PAGE_HOME, -1);
		return true;
	}
	leave();
	return false;
}

static void arrived(int page)
{
	svc_read();
	zmc_redraw();
}

static const struct zmc_app app = {
	.app_id = "ztservicemode", .title = "Service mode", .caps = "SERVICE MODE",
	.render = render, .activate = activate, .back = back, .arrived = arrived,
};

int main(int argc, char **argv)
{
	redact = getenv("ZT_SVC_REDACT") && atoi(getenv("ZT_SVC_REDACT"));
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		if (getenv("ZT_SVC_FILE"))
			svc_path = getenv("ZT_SVC_FILE");
		svc_read();
		bool screen = !strcmp(argv[2], "screen");
		for (int i = 0; screen && i < N_CODES; i++)
			if (!strcmp(svc.code, codes[i].code))
				current = i;
		return zmc_render_png(&app, screen ? PAGE_SCREEN : PAGE_HOME, 1400, argv[3]);
	}
	int first = PAGE_HOME;
	if (argc == 2) {
		/* "0011" or "*#0011#", as dialled */
		char code[24];
		snprintf(code, sizeof(code), "%s", argv[1] + (strncmp(argv[1], "*#", 2) ? 0 : 2));
		code[strcspn(code, "#")] = 0;
		for (int i = 0; i < N_CODES; i++) {
			if (!strcmp(code, codes[i].code)) {
				enter(i);
				first = PAGE_SCREEN;
			}
		}
	}
	watch(true);
	svc_read();
	atexit(end_at_exit);
	return zmc_run(&app, first);
}
