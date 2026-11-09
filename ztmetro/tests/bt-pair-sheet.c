/* The Bluetooth pairing sheet with no screen, as tests/auth-prompt.c: check-bt-pair-sheet */
#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <assert.h>

static int agent_fd;

/* the next answer the shell sent to its "agent" */
static void expect(struct zt_server *s, const char *want) {
	int peer = accept(agent_fd, NULL, NULL);
	assert(peer >= 0);
	char got[100] = {0};
	assert(read(peer, got, sizeof(got) - 1) > 0);
	close(peer);
	if (strcmp(got, want)) { printf("got %s, wanted %s\n", got, want); assert(0); }
}

static void ctl(struct zt_server *s, const char *text) {
	char line[300];
	snprintf(line, sizeof(line), "%s", text);
	ctl_command(s, line);
}

static void tap_key(struct zt_server *s, uint32_t code, const char *text) {
	auth_input(s, code, text);
}

int main(void) {
	char dir[] = "/tmp/ztbtpairXXXXXX", path[120];
	assert(mkdtemp(dir));
	setenv("XDG_RUNTIME_DIR", dir, 1);
	snprintf(path, sizeof(path), "%s/ztos-bt-agent.sock", dir);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	agent_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(agent_fd >= 0 && !bind(agent_fd, (struct sockaddr *)&addr, sizeof(addr)) && !listen(agent_fd, 4));

	struct zt_server s = { .width=360, .height=740, .scale=1, .key_height=56, .screen_on=true,
		.osk_pressed=-1, .auth_fd=-1 };
	s.wl_display = wl_display_create();
	s.loop = wl_display_get_event_loop(s.wl_display);

	/* a PIN: shown as typed, digits page up, only letters and digits taken */
	ctl(&s, "bt-pair 1|pin|55|00:0D:44:EB:50:54||Speaker|x\n");
	assert(s.bt_pair.shown && s.auth_active && s.dialog_open && s.osk_visible && s.osk_mode == OSK_MODE_NUMBER);
	assert(strstr(s.dialog_text, "Speaker/x") && strstr(s.dialog_text, "00:0D:44:EB:50:54"));
	dialog_close(&s); assert(s.dialog_open);		/* a tap outside can't close it */
	dialog_show(&s, "x", "y", "power off", "power_off", "", "");
	assert(!strcmp(s.dialog_title, "Bluetooth pairing"));
	dialog_handle_action(&s, "bt_pair_ok");			/* nothing typed yet */
	assert(s.bt_pair.active && s.dialog_open);
	tap_key(&s, KEY_1, "1"); tap_key(&s, KEY_A, "a"); tap_key(&s, KEY_SPACE, " "); tap_key(&s, KEY_3, "|");
	tap_key(&s, KEY_F13, ""); tap_key(&s, KEY_E, "\xc3\xa9");
	assert(s.bt_pair.entry_len == 2 && !strcmp(s.bt_pair.entry, "1a"));
	tap_key(&s, KEY_BACKSPACE, ""); assert(!strcmp(s.bt_pair.entry, "1"));
	for (int i = 0; i < 30; i++) tap_key(&s, KEY_7, "7");
	assert(s.bt_pair.entry_len == 16);
	tap_key(&s, KEY_ENTER, "");
	expect(&s, "1\tOK\t1777777777777777\n");
	assert(!s.bt_pair.active && !s.bt_pair.shown && !s.auth_active && !s.dialog_open && !s.osk_visible);
	assert(!s.bt_pair.entry[0] && !s.bt_pair.entry_len && s.osk_shell_mode == OSK_MODE_FULL);

	/* a passkey: exactly six, on its own keypad */
	ctl(&s, "bt-pair 2|passkey|55|||Phone");
	assert(s.bt_pair.shown && s.osk_mode == OSK_MODE_DIGITS && s.n_keys == 12);
	tap_key(&s, KEY_A, "a"); assert(!s.bt_pair.entry_len);
	for (int i = 0; i < 5; i++) tap_key(&s, KEY_4, "4");
	dialog_handle_action(&s, "bt_pair_ok"); assert(s.bt_pair.active);
	tap_key(&s, KEY_0, "0"); tap_key(&s, KEY_0, "0"); assert(s.bt_pair.entry_len == 6);
	/* the tick box: off each time, a tap on its row turns it on and off; the
	 * answer carries it. BT_SHEET_DUMP=DIR saves the sheet, for a look. */
	g_dump_dir = getenv("BT_SHEET_DUMP");
	g_dump_name = "bt-pair-passkey";
	draw_dialog(&s);
	g_dump_name = NULL;
	assert(!s.bt_pair.remember && s.bt_pair.check_y > 0);
	assert(!bt_pair_touch(&s, 40, s.bt_pair.check_y - 20));	/* the field, not the box */
	assert(bt_pair_touch(&s, 40, s.bt_pair.check_y + 10) && s.bt_pair.remember);
	assert(bt_pair_touch(&s, 300, s.bt_pair.check_y + 30) && !s.bt_pair.remember);
	assert(bt_pair_touch(&s, 40, s.bt_pair.check_y + 10) && s.bt_pair.remember);
	g_dump_name = "bt-pair-passkey-ticked";
	draw_dialog(&s);
	g_dump_dir = g_dump_name = NULL;
	dialog_handle_action(&s, "bt_pair_ok");
	expect(&s, "2\tOK\t444440\tremember\n");
	ctl(&s, "bt-pair 2a|pin|55|||Phone");
	assert(s.bt_pair.shown && !s.bt_pair.remember);
	dialog_handle_action(&s, "bt_pair_cancel");
	expect(&s, "2a\tCANCEL\n");
	assert(!s.bt_pair.active && s.osk_shell_mode == OSK_MODE_FULL);

	/* a number to compare: yes and no are the buttons, no key is a yes */
	ctl(&s, "bt-pair 3|confirm|55|||Phone");		/* no number: no sheet */
	assert(!s.bt_pair.active);
	ctl(&s, "bt-pair 3|confirm|55||012345|Phone");
	assert(s.bt_pair.shown && !s.osk_visible && !strcmp(s.dialog_btn1, "yes") && !strcmp(s.dialog_btn2, "no"));
	tap_key(&s, KEY_ENTER, ""); assert(s.bt_pair.active);
	tap_key(&s, KEY_ESC, "");
	expect(&s, "3\tCANCEL\n");
	assert(!s.bt_pair.shown);
	ctl(&s, "bt-pair 4|confirm|55||012345|Phone");
	dialog_handle_action(&s, "bt_pair_ok");
	expect(&s, "4\tOK\t\n");

	/* a number to type elsewhere: stays until told, one button */
	ctl(&s, "bt-pair 5|show-passkey|60||000042|Keyboard");
	assert(s.bt_pair.shown && !s.dialog_btn2[0] && !strcmp(s.dialog_btn1, "cancel"));
	ctl(&s, "bt-pair-update 5|3"); assert(s.bt_pair.entered == 3);
	ctl(&s, "bt-pair-update 9|4"); assert(s.bt_pair.entered == 3);
	ctl(&s, "bt-pair-update 5|99"); assert(s.bt_pair.entered == 3);
	ctl(&s, "bt-pair-close 9"); assert(s.bt_pair.shown);	/* someone else's */
	tap_key(&s, KEY_ENTER, ""); assert(s.bt_pair.shown);
	ctl(&s, "bt-pair-close 5");
	assert(!s.bt_pair.active && !s.bt_pair.shown && !s.auth_active && !s.dialog_open);
	ctl(&s, "bt-pair 6|show-pin|60||0000|Keyboard");
	dialog_handle_action(&s, "bt_pair_cancel");
	expect(&s, "6\tCANCEL\n");

	/* asked for permission */
	ctl(&s, "bt-pair 7|authorize|55|||Laptop");
	assert(s.bt_pair.shown && strstr(s.dialog_text, "Laptop wants to pair"));
	ctl(&s, "bt-pair-close 7");
	ctl(&s, "bt-pair 8|service|55||audio|Laptop");
	assert(strstr(s.dialog_text, "Laptop wants to use audio") && !strcmp(s.dialog_btn1, "allow"));
	ctl(&s, "bt-pair-close 8");

	/* the agent says nothing more */
	ctl(&s, "bt-pair a|pin|5|||Phone");
	bt_pair_expired(&s);
	assert(!s.bt_pair.active && !s.dialog_open && !s.osk_visible);

	/* locked: held, with a note, until unlocked; answered on the sheet, not before */
	s.locked = true;
	ctl(&s, "bt-pair b|confirm|55||123456|Phone");
	assert(s.bt_pair.active && !s.bt_pair.shown && !s.dialog_open && !s.auth_active);
	assert(s.n_notes == 1 && !strcmp(s.notes[0].title, "Pairing request") && strstr(s.notes[0].text, "Phone"));
	ctl(&s, "bt-pair-close b");				/* over before anyone looked */
	assert(!s.bt_pair.active && !s.n_notes);
	ctl(&s, "bt-pair c|pin|55|||Phone");
	assert(s.n_notes == 1);
	s.locked = false;
	bt_pair_sync(&s);
	assert(s.bt_pair.shown && s.dialog_open && s.osk_visible && !s.n_notes);
	/* to sleep with a half-typed PIN: gone from the screen and memory, still asked */
	tap_key(&s, KEY_9, "9");
	bt_pair_hide(&s);
	assert(s.bt_pair.active && !s.bt_pair.shown && !s.auth_active && !s.bt_pair.entry_len && !s.bt_pair.entry[0]);
	bt_pair_sync(&s);
	assert(s.bt_pair.shown);
	ctl(&s, "bt-pair-close c");

	/* a polkit prompt comes first and is not overwritten; the other way, it is told to go away */
	s.auth_active = true;
	ctl(&s, "bt-pair d|confirm|55||123456|Phone");
	assert(s.bt_pair.active && !s.bt_pair.shown);
	s.auth_active = false;
	bt_pair_sync(&s);
	assert(s.bt_pair.shown);
	int peers[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peers) == 0);
	s.auth_fd = peers[0];
	char prompt[] = "PROMPT\tTest"; auth_line(&s, prompt);
	assert(s.bt_pair.shown && !strcmp(s.dialog_title, "Bluetooth pairing"));
	close(peers[1]);
	ctl(&s, "bt-pair-close d");

	/* a request for something new takes over */
	ctl(&s, "bt-pair e|pin|55|||One");
	tap_key(&s, KEY_1, "1");
	ctl(&s, "bt-pair f|confirm|55||123456|Two");
	assert(s.bt_pair.shown && !strcmp(s.bt_pair.req.id, "f") && !s.bt_pair.entry_len && !s.osk_visible);
	ctl(&s, "bt-pair-close f");

	unlink(path);
	rmdir(dir);
	puts("PASS: every pairing sheet, its keys and answers, Cancel, lock, sleep, polkit, no agent");
	return 0;
}
