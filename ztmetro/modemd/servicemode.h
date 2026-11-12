// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung's ServiceMode (the *#0011#, *#2263#, *#197328640#, *#9090# menus),
 * read-only, for ztoscorenetworkingd. Included by ztoscorenetworkingd.c after
 * ipc_send(); uses its STATUS_DIR and ms().
 *
 * The menus are the CP's own: the AP enters a mode, presses keys, and the CP
 * sends the screen back as lines of text. The IPC messages, as libsec-ril
 * builds them (Ipc41PacketBuilder::BuildIpcSvcMode*, read from its code) and
 * as libsamsung-ipc names them (include/svc.h):
 *
 *	0b01 SVC_ENTER set, 3 bytes: mode, sub type, magic. libsec-ril sends
 *		the sub type and magic 0x10 for mode 1 (test manual) only; for
 *		any other mode both are 0.
 *	0b02 SVC_END set, 1 byte: the mode entered (stock: 4 for the monitor,
 *		1 for everything else)
 *	0b03 SVC_PRO_KEYCODE set, 1 byte: a key. The stock app sends a menu
 *		line's character from its "[N]" ('1' is 0x31), '\' (0x5c) for back,
 *		and for typed values each character then 'S' (0x53).
 *	0b05 SVC_DISPLAY_SCREEN, from the CP: count, then count x 34 bytes,
 *		2 bytes (an index) and 32 characters, NUL padded. The whole screen
 *		each time. libsec-ril drops a screen of count 1 whose index bytes
 *		are 00 01.
 *	0b16 the same, wide: count x 42 bytes, 2 and 40 characters.
 *	0b06 SVC_CHANGE_SVC_MODE, from the CP: 1 byte, the mode now.
 *
 * What is allowed here (the owner's rule: look, never change):
 *  - entering only the modes in svc_codes below;
 *  - back ('\'), and a digit only where the screen has that "[N]" line, the
 *    line passes svc_line_denied(), the mode allows keys at all, and the
 *    screen the last key asked for has come;
 *  - never typed values ('S'), never '*' or letters;
 *  - leaving: on "svc end", 90 s after the last command, when the screen goes
 *    off, and before entering another mode. A modem start forgets it.
 *
 * Screens go to STATUS_DIR/svc (tmpfs) for the ServiceMode app; the journal
 * only ever gets counts, never a screen's text (cell identities are in it).
 */
#ifndef SERVICEMODE_H
#define SERVICEMODE_H

#ifndef SVC_FILE
#define SVC_FILE	STATUS_DIR "/svc"	/* (a test puts it elsewhere) */
#endif
#define SVC_IDLE_MS	90000
#define SVC_LINES	100
#define SVC_LINE	41
#define SVC_KEY_GAP_MS	300

struct svc_code {
	const char *code;	/* what is dialled between *# and # */
	unsigned char mode, sub;	/* as the stock app enters it */
	bool keys;		/* digits may be pressed (back always may) */
	const char *what;
};

/* Mode and sub type from the stock ServiceModeApp_RIL (ModemServiceMode.apk,
 * ServiceModeApp.StartServiceMode). The sub type is sent as one byte. */
static const struct svc_code svc_codes[] = {
	{ "0011", 4, 0x00, true, "field test" },
	{ "197328640", 1, 0x00, true, "service menu" },
	{ "2263", 1, 0x16, false, "band selection (look only)" },
	{ "9090", 1, 0x13, false, "diag control (look only)" },
};

static struct {
	const struct svc_code *code;	/* NULL: not in ServiceMode */
	unsigned char mode;
	long last_cmd, last_key;
	bool waiting;		/* entered or a key sent, and no screen since */
	int n;
	char lines[SVC_LINES][SVC_LINE];
	unsigned screens;	/* screens since entering */
	char note[96];		/* the last thing that happened, for the app */
	char net_mode[8], ext_band[136], pci_lock[32];	/* "svc netinfo": GET answers, hex */
} svc;

static void svc_write(void)
{
	mkdir(STATUS_DIR, 0755);
	FILE *f = fopen(SVC_FILE ".new", "w");
	if (!f)
		return;
	fprintf(f, "state=%s\ncode=%s\nwhat=%s\nmode=%d\nsub=%d\nkeys=%d\nwaiting=%d\nscreens=%u\nnote=%s\n",
		svc.code ? "on" : "off", svc.code ? svc.code->code : "", svc.code ? svc.code->what : "",
		svc.code ? svc.mode : 0, svc.code ? svc.code->sub : 0, svc.code && svc.code->keys,
		svc.waiting, svc.screens, svc.note);
	fprintf(f, "net_mode=%s\next_band=%s\npci_lock=%s\n", svc.net_mode, svc.ext_band, svc.pci_lock);
	for (int i = 0; i < svc.n; i++)
		fprintf(f, "line=%s\n", svc.lines[i]);
	fclose(f);
	chmod(SVC_FILE ".new", 0644);
	rename(SVC_FILE ".new", SVC_FILE);
}

static void svc_note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(svc.note, sizeof(svc.note), fmt, ap);
	va_end(ap);
}

/* A line that is not to be selected: one that names something the owner
 * ruled out (identity, NV, calibration, unlocking, factory reset) or that
 * looks like a setting rather than a way into a menu. Whole words, any case.
 * `word` gets the word that matched. */
static bool svc_line_denied(const char *text, char *word, size_t size)
{
	static const char *const deny[] = {
		/* identity, NV, calibration, unlocking, factory: off limits for good */
		"IMEI", "MEID", "ESN", "SERIAL", "NV", "NVM", "EFS", "CAL", "CALIBRATION", "RF", "FACTORY",
		"RESET", "CLEAR", "ERASE", "FORMAT", "REBUILD", "BACKUP", "RESTORE", "UNLOCK", "LOCK",
		"LOCKING", "PERSO", "SIMLOCK", "NETLOCK", "NCK", "OTP", "KEY", "PASSWORD", "SECURE", "FUSE",
		"NAM", "OTA", "PRL",
		/* a setting, not a menu */
		"ON", "OFF", "ENABLE", "ENABLED", "DISABLE", "DISABLED", "SET", "SETTING", "SETTINGS",
		"CHANGE", "APPLY", "SAVE", "WRITE", "DELETE", "SELECT", "SELECTION", "AUTO", "AUTOMATIC",
		"MANUAL", "OK", "YES", "NO", "CONFIG", "CONFIGURATION", "CONTROL", "PREF", "PREFERENCE",
		"PRIORITY", "MODE", "BAND", "RAT", "FREQ", "FREQUENCY", "EARFCN", "UARFCN", "ARFCN", "PCI",
		"BARRING", "BAR", "FORCE", "FIX", "FIXED",
		/* things that act on the phone or the modem */
		"AUDIO", "LOOPBACK", "VIBRATOR", "MELODY", "RAMDUMP", "DUMP", "CRASH", "SILENT", "LOG",
		"LOGGING", "DIAG", "USB", "UART", "ATTACH", "DETACH", "CALL", "ANSWER", "SMS", "SELLOUT",
		"GCF", "TEST", "BACKOFF", "PRECONFIG", "ANT", "ANTENNA", "SAR", "TX", "POWER", "HOSTLINK",
		"UPGRADE", "UPDATE", "FOTA",
	};
	char up[SVC_LINE + 1];
	size_t n = 0;
	for (const char *p = text; *p && n < sizeof(up) - 1; p++)
		up[n++] = (char)toupper((unsigned char)*p);
	up[n] = 0;
	for (char *p = up; *p;) {
		while (*p && !isalnum((unsigned char)*p))
			p++;
		char *w = p;
		while (isalnum((unsigned char)*p))
			p++;
		size_t len = (size_t)(p - w);
		if (!len)
			continue;
		for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
			if (strlen(deny[i]) == len && !strncmp(w, deny[i], len)) {
				snprintf(word, size, "%s", deny[i]);
				return true;
			}
		}
	}
	return false;
}

/* The screen's line for key k: "[k]" or "[ k]", as the stock app reads it
 * (the character inside the brackets, after one space). NULL: none. */
static const char *svc_line_for(char k)
{
	for (int i = 0; i < svc.n; i++) {
		const char *open = strchr(svc.lines[i], '['), *close = open ? strchr(open, ']') : NULL;
		if (!close)
			continue;
		const char *in = open + 1;
		while (in < close && *in == ' ')
			in++;
		if (close - in == 1 && *in == k)
			return svc.lines[i];
	}
	return NULL;
}

/* Whether key k may go, and why not. */
static bool svc_key_ok(char k, char *why, size_t size)
{
	if (!svc.code) {
		snprintf(why, size, "not in ServiceMode");
		return false;
	}
	if (k == '\\')
		return true;
	if (!svc.code->keys) {
		snprintf(why, size, "*#%s#: look only", svc.code->code);
		return false;
	}
	if (k < '0' || k > '9') {
		snprintf(why, size, "only digits and back");
		return false;
	}
	if (svc.waiting) {
		snprintf(why, size, "the last screen has not come");
		return false;
	}
	const char *line = svc_line_for(k);
	if (!line) {
		snprintf(why, size, "no [%c] on this screen", k);
		return false;
	}
	char word[16];
	if (svc_line_denied(line, word, sizeof(word))) {
		snprintf(why, size, "[%c] says %s", k, word);
		return false;
	}
	return true;
}

static void svc_end(const char *why)
{
	if (!svc.code)
		return;
	{
		/* stock: SVC_END with 4 after the monitor, 1 after anything else */
		unsigned char mode = svc.mode == 4 ? 4 : 1;
		ipc_send(0x0b, 0x02, 3, &mode, 1);
		fprintf(stderr, "ztoscorenetworkingd: svc: left *#%s# (%s)\n", svc.code->code, why);
	}
	svc.code = NULL;
	svc.waiting = false;
	svc_note("left: %s", why);
	svc_write();
}

static void svc_enter(const char *code)
{
	const struct svc_code *c = NULL;
	for (size_t i = 0; i < sizeof(svc_codes) / sizeof(svc_codes[0]); i++)
		if (!strcmp(code, svc_codes[i].code))
			c = &svc_codes[i];
	if (!c) {
		svc_note("*#%.20s# is not one of these", code);
		svc_write();
		return;
	}
	if (svc.code)
		svc_end("another mode");
	unsigned char enter[3] = { c->mode, c->mode == 1 ? c->sub : 0, c->mode == 1 ? 0x10 : 0 };
	ipc_send(0x0b, 0x01, 3, enter, sizeof(enter));
	svc.code = c;
	svc.mode = c->mode;
	svc.n = 0;
	svc.screens = 0;
	svc.waiting = true;
	svc.last_cmd = svc.last_key = ms();
	svc_note("entering");
	fprintf(stderr, "ztoscorenetworkingd: svc: entering *#%s# (mode %d, sub 0x%02x)\n", c->code, c->mode, c->sub);
	svc_write();
}

static void svc_key(char k)
{
	char why[64];
	if (!svc_key_ok(k, why, sizeof(why))) {
		fprintf(stderr, "ztoscorenetworkingd: svc: key 0x%02x refused: %s\n", (unsigned char)k, why);
		svc_note("refused: %s", why);
		svc_write();
		return;
	}
	if (ms() - svc.last_key < SVC_KEY_GAP_MS)
		return;		/* a double tap */
	unsigned char key = (unsigned char)k;
	ipc_send(0x0b, 0x03, 3, &key, 1);
	svc.waiting = true;
	svc.last_key = ms();
	svc_note(k == '\\' ? "back" : "key %c", k);
	fprintf(stderr, "ztoscorenetworkingd: svc: key 0x%02x\n", key);
	svc_write();
}

/* read-only questions, for the approval list: the RAT mask (080a), the LTE
 * band table (0830) and the PCI/EARFCN lock (0833), each a GET */
static void svc_netinfo(void)
{
	ipc_send(0x08, 0x0a, 2, NULL, 0);
	ipc_send(0x08, 0x30, 2, NULL, 0);
	ipc_send(0x08, 0x33, 2, NULL, 0);
	svc_note("asked the network settings");
	svc_write();
}

static void svc_hex(char *out, size_t size, const unsigned char *data, size_t len)
{
	out[0] = 0;
	for (size_t i = 0; i < len && 2 * i + 2 < size; i++)
		sprintf(out + 2 * i, "%02x", data[i]);
}

/* A screen from the CP: count, then count x (2 + width) bytes */
static void svc_screen(const unsigned char *data, size_t len, size_t width)
{
	if (len < 1)
		return;
	size_t count = data[0], entry = 2 + width;
	if (count == 1 && len >= 3 && data[1] == 0 && data[2] == 1) {
		svc_note("an empty screen (00 01)");	/* libsec-ril drops these too */
		svc_write();
		return;
	}
	if (count > SVC_LINES)
		count = SVC_LINES;
	if (1 + count * entry > len)
		count = (len - 1) / entry;
	svc.n = 0;
	bool ended = false;
	for (size_t i = 0; i < count; i++) {
		const unsigned char *p = data + 1 + i * entry + 2;
		char *l = svc.lines[svc.n++];
		size_t o = 0;
		for (size_t j = 0; j < width && j < SVC_LINE - 1 && p[j]; j++)
			l[o++] = isprint(p[j]) ? (char)p[j] : ' ';
		while (o && l[o - 1] == ' ')
			o--;
		l[o] = 0;
		if (strcasestr(l, "End service mode"))
			ended = true;
	}
	svc.screens++;
	svc.waiting = false;
	if (svc.code && svc.screens == 1)
		fprintf(stderr, "ztoscorenetworkingd: svc: *#%s#: first screen, %d lines\n", svc.code->code, svc.n);
	if (ended && svc.code) {
		fprintf(stderr, "ztoscorenetworkingd: svc: the CP ended *#%s#\n", svc.code->code);
		svc.code = NULL;
		svc_note("the modem ended it");
	} else {
		svc_note("screen %u", svc.screens);
	}
	svc_write();
}

/* Everything from the CP with main 0x0b, and its answers to ours. True if
 * it was ServiceMode's. */
static bool svc_message(int main_cmd, int sub, int type, const unsigned char *data, size_t len)
{
	if (main_cmd == 0x80 && sub == 0x01 && len >= 5 && data[0] == 0x0b) {
		int result = data[3] | data[4] << 8;
		if (result != 0x8000) {
			fprintf(stderr, "ztoscorenetworkingd: svc: 0b%02x answered 0x%04x\n", data[1], result);
			svc_note("0b%02x answered 0x%04x", data[1], result);
			if (data[1] == 0x01 && svc.code) {
				svc.code = NULL;	/* not entered: nothing to end */
				svc.waiting = false;
			}
			svc_write();
		}
		return true;
	}
	if (main_cmd == 0x80 && sub == 0x01 && len >= 5 && data[0] == 0x08 &&
	    (data[1] == 0x30 || data[1] == 0x33) && (data[3] | data[4] << 8) != 0x8000) {
		/* a GET refused: say so (080a's answer is the daemon's own) */
		snprintf(data[1] == 0x30 ? svc.ext_band : svc.pci_lock, 16, "refused %04x", data[3] | data[4] << 8);
		svc_write();
		return true;
	}
	if (main_cmd == 0x08 && type == 2 && sub == 0x0a && len >= 1) {
		snprintf(svc.net_mode, sizeof(svc.net_mode), "0x%02x", data[0]);
		svc_write();
		return false;	/* the daemon may look too */
	}
	if (main_cmd == 0x08 && type == 2 && sub == 0x30) {
		svc_hex(svc.ext_band, sizeof(svc.ext_band), data, len);
		svc_write();
		return true;
	}
	if (main_cmd == 0x08 && type == 2 && sub == 0x33) {
		svc_hex(svc.pci_lock, sizeof(svc.pci_lock), data, len);
		svc_write();
		return true;
	}
	if (main_cmd != 0x0b)
		return false;
	if (sub == 0x05) {
		svc_screen(data, len, 32);
	} else if (sub == 0x16) {
		svc_screen(data, len, 40);
	} else if (sub == 0x06 && len >= 1) {
		svc.mode = data[0];
		svc_note("the modem changed to mode %d", data[0]);
		svc_write();
	} else if (sub == 0x02) {
		if (svc.code)
			fprintf(stderr, "ztoscorenetworkingd: svc: the CP ended *#%s#\n", svc.code->code);
		svc.code = NULL;
		svc.waiting = false;
		svc_note("the modem ended it");
		svc_write();
	} else {
		fprintf(stderr, "ztoscorenetworkingd: svc: 0b%02x t%d, %zu bytes (not read)\n", sub, type, len);
	}
	return true;
}

/* "svc ..." on the control pipe */
static void svc_command(const char *l)
{
	if (svc.code)
		svc.last_cmd = ms();
	if (!strncmp(l, "enter ", 6)) {
		svc_enter(l + 6);
	} else if (!strncmp(l, "key ", 4) && l[4] && !l[5]) {
		svc_key(l[4]);
	} else if (!strcmp(l, "back")) {
		svc_key('\\');
	} else if (!strcmp(l, "end")) {
		svc_end("asked");
	} else if (!strcmp(l, "ping")) {
		/* the app is still open: last_cmd moved */
	} else if (!strcmp(l, "netinfo")) {
		svc_netinfo();
	}
}

/* from the main loop: how long until it must be left (ms), or -1 */
static long svc_idle_left(void)
{
	if (!svc.code)
		return -1;
	long left = svc.last_cmd + SVC_IDLE_MS - ms();
	if (left <= 0) {
		svc_end("nothing asked for 90 s");
		return -1;
	}
	return left;
}

/* the modem started again: whatever mode it was in, it is not now */
static void svc_modem_started(void)
{
	if (svc.code)
		fprintf(stderr, "ztoscorenetworkingd: svc: the modem restarted in *#%s#\n", svc.code->code);
	svc.code = NULL;
	svc.waiting = false;
	svc_note("the modem restarted");
	svc_write();
}
#endif
