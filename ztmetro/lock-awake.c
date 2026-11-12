// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lock screen's "what keeps it awake" (ROADMAP 8.30; see lock-awake.h).
 * Included at the end of ztmetro.c.
 *
 * Cost: nothing while the screen is off. zrn-sleep-why runs as a child of the
 * shell (lowest priority) when the lock screen lights, every
 * LOCK_AWAKE_REFRESH_MS while it stays lit and locked, and when the sheet
 * opens; its reply comes back through a pipe in the event loop. The timer is
 * disarmed as the screen goes off or the phone unlocks, so a dark phone has
 * no timer of ours, and the line is drawn into its layer only when the reply
 * changed it.
 *
 * Read only: the sheet lists; it stops nothing. What it offers (the battery
 * app) opens only after the phone is unlocked the usual way.
 */
#include "lock-awake-parse.h"

#define LOCK_AWAKE_REFRESH_MS	30000
#define LOCK_AWAKE_CHILD_MS	15000	/* a reply this late: the child is let go */
#define LOCK_AWAKE_OPEN_MS	350	/* after the unlock, before the battery app */
#define LA_LINE_H		30.0
#define LA_LINE_SIZE		15.0
#define LA_PROGRAM		"/usr/local/sbin/zrn-sleep-why"
#define LA_BATTERY_APP		"ztusage"

enum { LA_HIT_LINE = 2001, LA_HIT_SHEET, LA_HIT_BUTTON };
enum la_mode { LA_OFF, LA_ON, LA_ALWAYS };

static enum la_mode la_mode = LA_ON;

static struct {
	struct la_data data;
	struct zt_layer line_layer, sheet_layer;
	double line_w;			/* the line's width, for the tap */
	bool sheet_open;
	double sheet_y;			/* the sheet's top, screen points, the lock at rest */
	double button_y, button_h;	/* its button, screen points */
	bool battery_after_unlock;	/* the button was tapped */
	bool battery_pending;		/* unlocked: open it at the next timer */
	pid_t child;
	uint32_t child_ms;
	int fd;
	struct wl_event_source *source, *timer;
	bool child_wakers, want_wakers;
	char buf[16384];
	size_t len;
} la = { .fd = -1 };

static void lock_awake_config(const char *value) {
	if (!value) la_mode = LA_ON;
	else if (!strcmp(value, "off") || !strcmp(value, "0")) la_mode = LA_OFF;
	else if (!strcmp(value, "always")) la_mode = LA_ALWAYS;
	else la_mode = LA_ON;
}

static bool la_active(struct zt_server *server) {
	return la_mode != LA_OFF && !retro_on(server) && server->width > 0;
}

/* The line's top, screen points, the lock at rest: just over the clock block,
 * which `lift` raises for the notification row. */
static double la_line_y(struct zt_server *server, float lift) {
	return LOCK_CLOCK_Y(server) - LA_LINE_H - 2 - LOCK_LIFT * lift;
}

static float la_lift(struct zt_server *server) {
	return server->lock_lift < 0.0f ? 0.0f : server->lock_lift > 1.0f ? 1.0f : server->lock_lift;
}

static void la_draw_line(struct zt_server *server) {
	if (!la_line_shown(&la.data)) {
		return;
	}
	char text[sizeof(la.data.line) + 8];
	snprintf(text, sizeof(text), "%s  \xe2\x80\xba", la.data.line);	/* a chevron: it opens */
	double max_w = server->width - 40;
	double tw = fmin(text_width(text, FONT_SEMILIGHT, LA_LINE_SIZE), max_w);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, LA_LINE_H, &cr);
	/* a faint band under it, so it reads over a bright photo */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.32);
	double x0 = 12, w = tw + 18, r = 4;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x0 + w - r, 4 + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x0 + w - r, LA_LINE_H - 4 - r, r, 0, M_PI / 2);
	cairo_arc(cr, x0 + r, LA_LINE_H - 4 - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x0 + r, 4 + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
	draw_text(cr, text, FONT_SEMILIGHT, LA_LINE_SIZE, x0 + 9, (LA_LINE_H - LA_LINE_SIZE * 1.33) / 2,
		max_w, 0, false);
	la.line_w = x0 + w + 12;
	zt_buffer_show(server, &la.line_layer, buffer, cr, server->width, LA_LINE_H);
}

/* The sheet's contents from its top; with cr NULL only measured. Stops
 * listing blockers before `max_h` leaves no room for the rest. */
static double la_sheet_layout(struct zt_server *server, cairo_t *cr, double max_h) {
	const struct la_data *d = &la.data;
	double W = server->width, x = 20, tw = W - 40, y = 18;
#define LA_TEXT(t, font, size, yy, alpha) do { if (cr) { cairo_set_source_rgba(cr, 1, 1, 1, alpha); \
	draw_text(cr, t, font, size, x, yy, tw, 0, false); } } while (0)
	LA_TEXT("keeping it awake", FONT_SEMILIGHT, 26, y, 1.0);
	y += 42;
	/* what is under the rest: the woke line, the busy list, the button, the footer */
	double rest = 30 + 40 + 24 + (d->n_wakers ? d->n_wakers : 1) * 22 + 60 + 50;
	int shown = 0;
	for (int i = 0; i < d->n_items; i++) {
		double need = d->items[i].detail[0] ? 52 : 32;
		if (y + need + rest > max_h && shown > 0) {
			char more[48];
			snprintf(more, sizeof(more), "and %d more", d->n_items - i);
			LA_TEXT(more, FONT_REGULAR, 13, y, 0.6);
			y += 22;
			break;
		}
		LA_TEXT(d->items[i].label, FONT_SEMILIGHT, 19, y, 1.0);
		y += 25;
		if (d->items[i].detail[0]) {
			LA_TEXT(d->items[i].detail, FONT_REGULAR, 13, y, 0.6);
			y += 19;
		}
		y += 8;
		shown++;
	}
	if (d->n_items == 0) {
		LA_TEXT(d->valid ? "Nothing keeps it awake" : "Asking zrn-sleepd\xe2\x80\xa6", FONT_SEMILIGHT, 19, y, 0.8);
		y += 33;
	}
	y += 6;
	LA_TEXT("what woke it", FONT_SEMILIGHT, 16, y, 0.6);
	y += 24;
	if (cr) cairo_set_source_rgba(cr, 1, 1, 1, 1);
	y += wrap_text(cr, d->woke[0] ? d->woke : "\xe2\x80\xa6", FONT_REGULAR, 15, x, y, tw, false) + 12;
	char head[96];
	snprintf(head, sizeof(head), "busiest while awake%s%s", d->window[0] ? ", " : "", d->window);
	LA_TEXT(head, FONT_SEMILIGHT, 16, y, 0.6);
	y += 24;
	if (!d->wakers_known) {
		LA_TEXT("Measuring\xe2\x80\xa6", FONT_REGULAR, 15, y, 0.8);
		y += 22;
	} else if (d->n_wakers == 0) {
		LA_TEXT("Nothing stands out", FONT_REGULAR, 15, y, 0.8);
		y += 22;
	}
	for (int i = 0; i < d->n_wakers; i++) {
		const struct la_waker *w = &d->wakers[i];
		char name[64];
		snprintf(name, sizeof(name), "%s%s", w->irq ? "interrupt: " : "", w->name);
		double rw = cr ? text_width(w->rate, FONT_REGULAR, 13) : 0;
		if (cr) {
			cairo_set_source_rgba(cr, 1, 1, 1, 1);
			draw_text(cr, name, FONT_REGULAR, 15, x, y, tw - rw - 12, 0, false);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
			draw_text(cr, w->rate, FONT_REGULAR, 13, x + tw - rw, y + 2, 0, 0, false);
		}
		y += 22;
	}
	y += 12;
	/* the one thing it offers, and only for after the unlock */
	la.button_y = y;
	la.button_h = 40;
	if (cr) {
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, x + 1, y + 1, tw - 2, la.button_h - 2);
		if (la.battery_after_unlock) {
			set_rgb(cr, server->accent);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 1);
		}
		cairo_stroke(cr);
		draw_text(cr, la.battery_after_unlock ? "battery use opens after you unlock" :
			"see battery use after you unlock", FONT_SEMILIGHT, 16, x, y, tw, la.button_h, true);
	}
	y += la.button_h + 12;
	if (cr) cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
	y += wrap_text(cr, "Nothing here can be stopped while the phone is locked. Tap to close; swipe up to unlock.",
		FONT_REGULAR, 13, x, y, tw, false);
	y += 22;
#undef LA_TEXT
	return y;
}

static void la_draw_sheet(struct zt_server *server) {
	double max_h = server->height - STATUS_H - 24;
	double h = fmin(la_sheet_layout(server, NULL, max_h), max_h);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, h, &cr);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_paint(cr);
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, 0, 0, server->width, 3);
	cairo_fill(cr);
	la_sheet_layout(server, cr, max_h);
	la.sheet_y = server->height - h;
	la.button_y += la.sheet_y;
	zt_buffer_show(server, &la.sheet_layer, buffer, cr, server->width, h);
}

static void la_child_done(struct zt_server *server) {
	if (la.source) {
		wl_event_source_remove(la.source);
		la.source = NULL;
	}
	if (la.fd >= 0) {
		close(la.fd);
		la.fd = -1;
	}
	la.child = 0;	/* SIGCHLD is ignored: the kernel reaps it */
	(void)server;
}

static void la_spawn(struct zt_server *server, bool wakers);

static int la_readable(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	for (;;) {
		char scratch[1024];
		char *to = la.len < sizeof(la.buf) - 1 ? la.buf + la.len : scratch;
		size_t room = la.len < sizeof(la.buf) - 1 ? sizeof(la.buf) - 1 - la.len : sizeof(scratch);
		ssize_t n = read(fd, to, room);
		if (n > 0) {
			if (to != scratch) la.len += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) return 0;
		break;	/* the end of the reply */
	}
	la.buf[la.len] = 0;
	bool wakers = la.child_wakers;
	la_child_done(server);
	struct la_data fresh;
	la_parse(&fresh, la.buf);
	if (!fresh.wakers_known && la.data.wakers_known) {
		/* a quick look keeps the busy list the last full one found */
		fresh.wakers_known = true;
		fresh.n_wakers = la.data.n_wakers;
		memcpy(fresh.wakers, la.data.wakers, sizeof(fresh.wakers));
		memcpy(fresh.window, la.data.window, sizeof(fresh.window));
	}
	bool line_changed = strcmp(fresh.line, la.data.line) != 0 || !la.line_layer.tex;
	la.data = fresh;
	if (line_changed) la_draw_line(server);
	if (la.sheet_open) la_draw_sheet(server);
	if (la.want_wakers && !wakers && la.sheet_open) {
		la.want_wakers = false;
		la_spawn(server, true);
	}
	damage(server);
	return 0;
}

static void la_spawn(struct zt_server *server, bool wakers) {
	if (la.child > 0) {
		if (now_ms() - la.child_ms < LOCK_AWAKE_CHILD_MS) {
			la.want_wakers |= wakers;
			return;
		}
		kill(la.child, SIGKILL);	/* stuck: start again */
		la_child_done(server);
	}
	const char *program = getenv("ZTMETRO_AWAKE_CMD");
	if (!program || !*program) program = LA_PROGRAM;
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		child_priority();
		signal(SIGCHLD, SIG_DFL);
		dup2(p[1], STDOUT_FILENO);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDERR_FILENO);
		}
		close_inherited();
		const char *argv[6];
		int n = 0;
		argv[n++] = program;
		argv[n++] = "--shell";
		if (la_mode == LA_ALWAYS) argv[n++] = "--always";
		if (wakers) argv[n++] = "--wakers";
		argv[n] = NULL;
		execv(program, (char *const *)argv);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	la.child = pid;
	la.child_ms = now_ms();
	la.child_wakers = wakers;
	la.fd = p[0];
	la.len = 0;
	la.source = wl_event_loop_add_fd(server->loop, p[0], WL_EVENT_READABLE, la_readable, server);
}

static int la_timer_fired(void *data) {
	struct zt_server *server = data;
	if (la.battery_pending) {
		la.battery_pending = false;
		if (!server->locked) {
			launch(server, LA_BATTERY_APP);
		}
		return 0;
	}
	/* only on a lit, locked screen: never a wakeup of ours in the dark */
	if (!server->screen_on || !server->locked || !la_active(server)) {
		return 0;
	}
	la_spawn(server, la.sheet_open);
	wl_event_source_timer_update(la.timer, LOCK_AWAKE_REFRESH_MS);
	return 0;
}

static void la_arm(struct zt_server *server, int ms) {
	if (!la.timer) {
		la.timer = wl_event_loop_add_timer(server->loop, la_timer_fired, server);
	}
	if (la.timer) {
		wl_event_source_timer_update(la.timer, ms);
	}
}

static void lock_awake_refresh(struct zt_server *server) {
	if (!la_active(server)) {
		memset(&la.data, 0, sizeof(la.data));
		la.sheet_open = false;
		return;
	}
	if (!server->screen_on || !server->locked || g_dump_dir) {
		return;
	}
	la_spawn(server, la.sheet_open);
	la_arm(server, LOCK_AWAKE_REFRESH_MS);
}

static void lock_awake_screen_off(struct zt_server *server) {
	(void)server;
	if (la.timer) {
		wl_event_source_timer_update(la.timer, 0);
	}
	la.sheet_open = false;
	la.battery_after_unlock = false;
	la.want_wakers = false;
}

static void lock_awake_unlocked(struct zt_server *server) {
	la.sheet_open = false;
	la.want_wakers = false;
	if (la.timer) {
		wl_event_source_timer_update(la.timer, 0);
	}
	if (la.battery_after_unlock) {
		/* unlocked the usual way: now it may open, once the lock has gone */
		la.battery_after_unlock = false;
		la.battery_pending = true;
		la_arm(server, LOCK_AWAKE_OPEN_MS);
	}
}

static int lock_awake_hit(struct zt_server *server, double x, double y) {
	if (!la_active(server) || !server->screen_on) {
		return -1;
	}
	if (la.sheet_open) {
		return y >= la.button_y && y < la.button_y + la.button_h && x >= 20 && x < server->width - 20 ?
			LA_HIT_BUTTON : LA_HIT_SHEET;
	}
	if (la_line_shown(&la.data) && la.line_layer.tex) {
		double top = la_line_y(server, la_lift(server));
		if (y >= top - 10 && y < top + LA_LINE_H + 10 && x < la.line_w + 20) {
			return LA_HIT_LINE;
		}
	}
	return -1;
}

static bool lock_awake_tap(struct zt_server *server, int index, double x, double y) {
	(void)x;
	(void)y;
	switch (index) {
	case LA_HIT_LINE:
		la.sheet_open = true;
		la_draw_sheet(server);
		la_spawn(server, true);		/* fresh, with the busy list */
		hap(server, HAP_TAP, HG_SYSTEM);
		break;
	case LA_HIT_BUTTON:
		la.battery_after_unlock = !la.battery_after_unlock;
		la_draw_sheet(server);
		hap(server, HAP_TAP, HG_SYSTEM);
		break;
	case LA_HIT_SHEET:
		la.sheet_open = false;
		break;
	default:
		return false;
	}
	damage(server);
	return true;
}

static void lock_awake_draw(struct zt_server *server, float lift) {
	if (!la_active(server) || !server->screen_on || server->aod) {
		return;
	}
	if (la.sheet_open && la.sheet_layer.tex) {
		/* the lock screen dimmed under it */
		float m[16];
		mat_identity(m);
		mat_translate(m, 0, server->lock_y);
		mat_scale(m, server->width, server->height);
		quad(server, GL_TEXTURE_2D, server->black_tex, m, 0.55f, false, false, 1, 1);
		layer_draw(server, &la.sheet_layer, 0, server->lock_y + (float)la.sheet_y, 1.0f);
	} else if (la_line_shown(&la.data) && la.line_layer.tex) {
		layer_draw(server, &la.line_layer, 0, server->lock_y + (float)la_line_y(server, lift), 1.0f);
	}
}

/* --render: the line and the sheet as they look with a typical reply */
static void lock_awake_render_sample(struct zt_server *server) {
	la_parse(&la.data,
		"line\tAwake: Brave playing \xc2\xb7 2 more\n"
		"item\taudio\tBrave playing\tbrave \xc2\xb7 pid 2345 \xc2\xb7 sound keeps the phone awake\n"
		"item\tgps\tLocation (GPS)\tztos-gnss-gate \xc2\xb7 pid 812 \xc2\xb7 ztos-gnss-gate.service \xc2\xb7 "
		"timed-gnss, for another 1 min; read by gpsd\n"
		"item\taod\tAlways-on display\tset to show once the screen goes off\n"
		"item\tdisplay\tScreen on\tztmetro\n"
		"woke\tWoke 4 times in the last hour, mostly by the modem (umts_ipc0)\n"
		"window\tlast hour\n"
		"waker\tbrave\t412 wakeups a minute\n"
		"waker\tztmetro\t95 wakeups a minute\n"
		"irq\tarch_timer\t900 interrupts a minute\n");
	g_dump_name = "lock-awake-line";
	la_draw_line(server);
	g_dump_name = "lock-awake-sheet";
	la_draw_sheet(server);
	memset(&la.data, 0, sizeof(la.data));
}
