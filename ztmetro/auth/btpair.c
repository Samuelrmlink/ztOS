/* Included by the compositor after auth/prompt.c: the Bluetooth pairing sheet.
 *
 * It is the password sheet with another field: while it shows, auth_active is
 * set too, so the keyboard's keys come here (auth_input), the rest of the shell
 * takes no touches and no other dialog replaces it. Only one is up at a time:
 * ztos-bt-agent (rootfs/overlay/usr/local/bin) decides what is asked, when it
 * ends and what happens next. See auth/README.md.
 *
 * The typed PIN is shown, not starred: it is short, it is copied from the
 * other device, and a wrong digit costs a failed pairing. */

static bool bt_pair_entry(const struct bt_pair *b) {
	return b->req.mode == BT_PAIR_PIN || b->req.mode == BT_PAIR_PASSKEY;
}

/* What stops it showing now; it stays asked, and comes back. */
static bool bt_pair_may_show(struct zt_server *server) {
	return server->width > 0 && server->screen_on && !server->locked && !server->splash &&
		!server->sensorhub_fail_screen && (server->bt_pair.shown || !server->auth_active) &&
		(server->bt_pair.shown || !server->auth_source);
}

static void bt_pair_wipe_entry(struct bt_pair *b) {
	explicit_bzero(b->entry, sizeof(b->entry));
	b->entry_len = 0;
}

static void bt_pair_hide(struct zt_server *server) {
	struct bt_pair *b = &server->bt_pair;
	bt_pair_wipe_entry(b);
	if (!b->shown) return;
	bool entry = bt_pair_entry(b);
	b->shown = false;
	server->auth_active = false;
	if (entry) {
		server->osk_numeric = false;
		server->osk_symbols = false;
		server->osk_terminal = b->osk_was_terminal;
		osk_show(server, b->osk_was_visible);
	}
	dialog_close(server);
}

static void bt_pair_show(struct zt_server *server) {
	struct bt_pair *b = &server->bt_pair;
	struct bt_pair_request *r = &b->req;
	char text[sizeof(server->dialog_text)];
	const char *btn1 = "pair", *btn2 = "cancel", *act2 = "bt_pair_cancel";
	const char *act1 = "bt_pair_ok";
	const char *addr = r->addr[0] && strcmp(r->addr, r->name) ? r->addr : "";
	const char *open = addr[0] ? " (" : "", *close = addr[0] ? ")" : "";
	switch (r->mode) {
	case BT_PAIR_PIN:
		snprintf(text, sizeof(text), "Enter the PIN code of %s%s%s%s.", r->name, open, addr, close);
		break;
	case BT_PAIR_PASSKEY:
		snprintf(text, sizeof(text), "Enter the 6-digit passkey shown on %s%s%s%s.", r->name, open, addr, close);
		break;
	case BT_PAIR_SHOW_PIN:
		snprintf(text, sizeof(text), "Type this PIN on %s%s%s%s, then press its Enter key.", r->name, open, addr, close);
		btn1 = "cancel"; act1 = "bt_pair_cancel"; btn2 = NULL; act2 = NULL;
		break;
	case BT_PAIR_SHOW_PASSKEY:
		snprintf(text, sizeof(text), "Type this passkey on %s%s%s%s, then press its Enter key.", r->name, open, addr, close);
		btn1 = "cancel"; act1 = "bt_pair_cancel"; btn2 = NULL; act2 = NULL;
		break;
	case BT_PAIR_CONFIRM:
		snprintf(text, sizeof(text), "Does %s%s%s%s show this number?", r->name, open, addr, close);
		btn1 = "yes"; btn2 = "no";
		break;
	case BT_PAIR_AUTHORIZE:
		snprintf(text, sizeof(text), "%s%s%s%s wants to pair with this phone. Allow it only if you started this.",
			r->name, open, addr, close);
		break;
	case BT_PAIR_SERVICE:
		snprintf(text, sizeof(text), "%s%s%s%s wants to use %s on this phone.", r->name, open, addr, close,
			r->value[0] ? r->value : "a service");
		btn1 = "allow"; btn2 = "deny";
		break;
	default:
		return;
	}
	bt_pair_wipe_entry(b);
	snprintf(b->status, sizeof(b->status), "%s", r->mode == BT_PAIR_PIN ? "Letters and digits, up to 16" :
		r->mode == BT_PAIR_PASSKEY ? "6 digits" : "");
	b->osk_was_visible = server->osk_visible;
	b->osk_was_terminal = server->osk_terminal;
	/* dialog_show keeps out everything else while auth_active is set: us too */
	b->shown = true;
	dialog_show(server, "Bluetooth pairing", text, btn1, act1, btn2, act2);
	server->auth_active = true;
	if (bt_pair_entry(b)) {
		server->osk_terminal = false;
		server->key_prefix[0] = 0;
		server->osk_shift = server->osk_ctrl = server->osk_alt = false;
		server->osk_emoji = false;
		/* a passkey has its own keypad; a PIN starts on the digits page, with
		 * the letters a key away */
		server->osk_numeric = r->mode == BT_PAIR_PASSKEY;
		server->osk_symbols = r->mode == BT_PAIR_PIN;
		osk_show(server, true);
	}
	activity(server);
}

/* The note on the lock screen says what the screen woke for. It says the
 * device's name, as a lock screen shows who is calling. */
static void bt_pair_note(struct zt_server *server, bool on) {
	static const char *app = "Bluetooth", *title = "Pairing request";
	if (on) {
		if (server->bt_pair.noted) return;
		char text[160];
		snprintf(text, sizeof(text), "Unlock to answer %s", server->bt_pair.req.name);
		note_quiet(server, "", app, title, text);
		server->bt_pair.noted = true;
		return;
	}
	if (!server->bt_pair.noted) return;
	server->bt_pair.noted = false;
	for (int i = 0; i < server->n_notes; i++) {
		if (strcmp(server->notes[i].app, app) || strcmp(server->notes[i].title, title)) continue;
		memmove(&server->notes[i], &server->notes[i + 1], sizeof(server->notes[0]) * (server->n_notes - i - 1));
		server->n_notes--;
		server->notes_dirty = true;
		if (server->width) {
			lock_notes_changed(server);
			if (server->shade_open || server->shade_h > 0.1f) draw_shade(server);
			damage(server);
		}
		break;
	}
}

/* Bring what is on the screen into line with what is asked. Called whenever a
 * reason for hiding it may have ended. */
static void bt_pair_sync(struct zt_server *server) {
	struct bt_pair *b = &server->bt_pair;
	bool wanted = b->active && bt_pair_may_show(server);
	if (b->shown && !wanted) bt_pair_hide(server);
	else if (!b->shown && wanted) bt_pair_show(server);
	if (b->active && server->width) bt_pair_note(server, !wanted);
}

/* The sheet goes away and the request is over, on this side. */
static void bt_pair_end(struct zt_server *server) {
	struct bt_pair *b = &server->bt_pair;
	bt_pair_hide(server);
	bt_pair_note(server, false);
	b->active = false;
	if (b->deadline) wl_event_source_timer_update(b->deadline, 0);
	/* (the id stays, in the request, for nothing: the next one replaces it) */
	explicit_bzero(b->req.value, sizeof(b->req.value));
	b->req.mode = BT_PAIR_NONE;
}

/* To the agent, on a connection of its own, never blocking the compositor. */
static bool bt_pair_tell(struct zt_server *server, bool ok, const char *value) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (!runtime || snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ztos-bt-agent.sock", runtime) >= (int)sizeof(addr.sun_path))
		return false;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return false;
	bool sent = false;
	struct ucred cred;
	socklen_t size = sizeof(cred);
	if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)) &&
			!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) && cred.uid == getuid()) {
		char line[64];
		int n = bt_pair_reply_line(line, sizeof(line), server->bt_pair.req.id, ok, value);
		sent = n > 0 && send(fd, line, n, MSG_NOSIGNAL) == n;
		explicit_bzero(line, sizeof(line));
	}
	close(fd);
	if (!sent) wlr_log(WLR_ERROR, "bluetooth pairing: the agent can't be reached");
	return sent;
}

static void bt_pair_answer(struct zt_server *server, bool ok) {
	struct bt_pair *b = &server->bt_pair;
	if (!b->active) return;
	if (ok && bt_pair_entry(b)) {
		if (!bt_pair_entry_done(b->req.mode, b->entry_len)) {
			snprintf(b->status, sizeof(b->status), "%s", b->req.mode == BT_PAIR_PASSKEY ?
				"The passkey has 6 digits" : "Enter the PIN first");
			hap(server, HAP_TAP, HG_SYSTEM);
			draw_dialog(server);
			damage(server);
			return;
		}
		bt_pair_tell(server, true, b->entry);
	} else {
		bt_pair_tell(server, ok, "");
	}
	bt_pair_end(server);
}

/* Keys, from the on-screen keyboard or a real one (auth_input). Enter answers a
 * field only: it must not be able to say yes to a question. */
static void bt_pair_input(struct zt_server *server, uint32_t keycode, const char *text) {
	struct bt_pair *b = &server->bt_pair;
	if (keycode == KEY_ESC) { bt_pair_answer(server, false); return; }
	if (!bt_pair_entry(b)) return;
	if (keycode == KEY_ENTER || keycode == KEY_KPENTER) { bt_pair_answer(server, true); return; }
	if (keycode == KEY_BACKSPACE) {
		if (b->entry_len) b->entry[--b->entry_len] = 0;
	} else if (bt_pair_entry_takes(b->req.mode, b->entry_len, text)) {
		b->entry[b->entry_len++] = text[0];
		b->entry[b->entry_len] = 0;
	} else {
		return;
	}
	draw_dialog(server);
	damage(server);
}

static bool bt_pair_action(struct zt_server *server, const char *act) {
	if (!strcmp(act, "bt_pair_ok")) { bt_pair_answer(server, true); return true; }
	if (!strcmp(act, "bt_pair_cancel")) { bt_pair_answer(server, false); return true; }
	return false;
}

/* The space the sheet leaves between its text and its buttons. */
static double bt_pair_field_h(struct zt_server *server) {
	return bt_pair_entry(&server->bt_pair) ? 90 :
		server->bt_pair.req.mode == BT_PAIR_SHOW_PIN || server->bt_pair.req.mode == BT_PAIR_SHOW_PASSKEY ||
		server->bt_pair.req.mode == BT_PAIR_CONFIRM ? 76 : 0;
}

static void bt_pair_draw(struct zt_server *server, cairo_t *cr, double x, double y, double w) {
	struct bt_pair *b = &server->bt_pair;
	if (bt_pair_entry(b)) {
		cairo_set_source_rgb(cr, .3, .3, .3);
		cairo_rectangle(cr, x, y, w, 42); cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, b->entry, FONT_REGULAR, 20, x + 12, y, w - 24, 42, false);
		cairo_set_source_rgb(cr, .8, .8, .8);
		draw_text(cr, b->status, FONT_REGULAR, 13, x, y + 50, w, 24, false);
	} else if (bt_pair_field_h(server) > 0) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, b->req.value, FONT_SEMIBOLD, 34, x, y, w, 46, false);
		if (b->req.mode == BT_PAIR_SHOW_PASSKEY && b->entered > 0) {
			char typed[40];
			snprintf(typed, sizeof(typed), "%d of %d typed", b->entered, BT_PAIR_PASSKEY_LEN);
			cairo_set_source_rgb(cr, .8, .8, .8);
			draw_text(cr, typed, FONT_REGULAR, 13, x, y + 48, w, 24, false);
		}
	}
}

/* The agent went quiet and never closed it (it stopped, say): nothing would
 * ever take the sheet away. It should close it itself long before this. */
static int bt_pair_expired(void *data) {
	struct zt_server *server = data;
	wlr_log(WLR_INFO, "bluetooth pairing: no word from the agent, closing");
	bt_pair_end(server);
	return 0;
}

/* The control pipe's bt-pair commands, with their "bt-pair" still on. */
static void bt_pair_ctl(struct zt_server *server, char *line) {
	struct bt_pair *b = &server->bt_pair;
	if (!strncmp(line, "bt-pair-close ", 14)) {
		if (b->active && !strcmp(line + 14, b->req.id)) bt_pair_end(server);
	} else if (!strncmp(line, "bt-pair-update ", 15)) {
		char *bar = strchr(line + 15, '|');
		if (!bar) return;
		*bar = 0;
		int entered = atoi(bar + 1);
		if (b->active && !strcmp(line + 15, b->req.id) && entered >= 0 && entered <= BT_PAIR_PASSKEY_LEN) {
			b->entered = entered;
			if (b->shown) { draw_dialog(server); damage(server); }
		}
	} else if (!strncmp(line, "bt-pair ", 8)) {
		struct bt_pair_request req;
		if (!bt_pair_parse(line + 8, &req)) {
			wlr_log(WLR_ERROR, "bluetooth pairing: a request that makes no sense");
			return;
		}
		/* the agent asks one thing at a time: a new one means the old one is over */
		if (b->active) bt_pair_end(server);
		b->req = req;
		b->entered = 0;
		b->active = true;
		explicit_bzero(&req, sizeof(req));
		if (!b->deadline) b->deadline = wl_event_loop_add_timer(server->loop, bt_pair_expired, server);
		if (b->deadline) wl_event_source_timer_update(b->deadline, (b->req.seconds + 5) * 1000);
		wlr_log(WLR_INFO, "bluetooth pairing: request %s", b->req.id);
		if (server->output && !server->screen_on) {
			screen_set(server, true);
			activity(server);
		}
		bt_pair_sync(server);
	}
}
