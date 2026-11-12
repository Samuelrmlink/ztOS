/* ServiceMode (servicemode.h) with no modem: umts_ipc0 is a socket pair.
 * What goes to the CP, which keys are let through, and that a screen's text
 * never reaches the journal.
 *
 *	cc -std=c11 -Wall -Wno-unused-parameter -o servicemode servicemode.c && ./servicemode
 */
#define SVC_FILE "/tmp/ztos-servicemode-test.svc"
#define main networking_daemon_main
#include "../ztoscorenetworkingd.c"
#undef main
#include <assert.h>
#include <sys/socket.h>

static int ipc_peer = -1;
static char errlog[64];

/* the next message the daemon sent to the CP: main, sub, type, data */
static bool sent_msg(int *m, int *s, int *t, unsigned char *data, size_t *len)
{
	unsigned char b[600];
	ssize_t n = recv(ipc_peer, b, sizeof(b), MSG_DONTWAIT);
	if (n < 7)
		return false;
	*m = b[4], *s = b[5], *t = b[6];
	*len = (size_t)n - 7;
	memcpy(data, b + 7, *len);
	return true;
}

static void drain(void)
{
	unsigned char b[600];
	while (recv(ipc_peer, b, sizeof(b), MSG_DONTWAIT) > 0)
		;
}

static void expect(int m, int s, int t, const unsigned char *want, size_t want_len)
{
	int gm, gs, gt;
	unsigned char data[600];
	size_t len;
	assert(sent_msg(&gm, &gs, &gt, data, &len));
	assert(gm == m && gs == s && gt == t);
	assert(len == want_len && !memcmp(data, want, len));
}

static void nothing_sent(void)
{
	int m, s, t;
	unsigned char data[600];
	size_t len;
	assert(!sent_msg(&m, &s, &t, data, &len));
}

static void ctl(const char *line)
{
	char l[128];
	snprintf(l, sizeof(l), "%s", line);
	command(l);
}

/* a 0b05 from the CP with these lines */
static void screen(const char *const *lines, int n)
{
	unsigned char data[1 + 100 * 34] = { (unsigned char)n };
	for (int i = 0; i < n; i++) {
		data[1 + i * 34] = (unsigned char)i;
		strncpy((char *)data + 1 + i * 34 + 2, lines[i], 32);
	}
	ipc_message(0x0b, 0x05, 3, 1, data, 1 + (size_t)n * 34);
}

static bool file_has(const char *needle)
{
	FILE *f = fopen(SVC_FILE, "r");
	char l[256];
	bool found = false;
	while (f && fgets(l, sizeof(l), f))
		found = found || strstr(l, needle);
	if (f)
		fclose(f);
	return found;
}

static bool log_has(const char *needle)
{
	fflush(stderr);
	FILE *f = fopen(errlog, "r");
	char l[512];
	bool found = false;
	while (f && fgets(l, sizeof(l), f))
		found = found || strstr(l, needle);
	if (f)
		fclose(f);
	return found;
}

static void key_refused(const char *cmd)
{
	ctl(cmd);
	nothing_sent();
	assert(file_has("note=refused"));
}

static void test_field_test(void)
{
	ctl("svc enter 0011");
	static const unsigned char enter[] = { 4, 0, 0 };	/* the monitor: no sub type, no magic */
	expect(0x0b, 0x01, 3, enter, 3);
	assert(file_has("state=on") && file_has("code=0011") && file_has("waiting=1"));
	static const char *const lines[] = { "LTE B3 EARFCN 1300", "PCI 123 RSRP -98", "Cell ID 0123ABCD" };
	screen(lines, 3);
	assert(file_has("line=Cell ID 0123ABCD") && file_has("waiting=0"));
	assert(!log_has("0123ABCD") && !log_has("EARFCN"));	/* never in the journal */
	assert(log_has("svc: *#0011#: first screen, 3 lines"));
	key_refused("svc key 1");		/* no [1] on that screen */
	ctl("svc end");
	static const unsigned char end4[] = { 4 };
	expect(0x0b, 0x02, 3, end4, 1);
	assert(file_has("state=off"));
	ctl("svc end");				/* not in it: nothing */
	nothing_sent();
}

static void test_menu_keys(void)
{
	ctl("svc enter 197328640");
	static const unsigned char enter[] = { 1, 0, 0x10 };
	expect(0x0b, 0x01, 3, enter, 3);
	key_refused("svc key 1");		/* the first screen has not come */
	static const char *const lines[] = {
		"[1] DEBUG SCREEN", "[2] VERSION INFORMATION", "[3] UMTS RF NV", "[4] Phone Control",
		"[ 5] COMMON", "[6] IMEI", "[7] FACTORY RESET", "[8] LTE BAND SELECTION", "[*] Back",
	};
	screen(lines, 9);
	assert(file_has("allow=125\n"));	/* DEBUG SCREEN, VERSION INFORMATION, COMMON */
	key_refused("svc key 3");		/* RF NV */
	key_refused("svc key 4");		/* CONTROL */
	key_refused("svc key 6");		/* IMEI */
	key_refused("svc key 7");		/* FACTORY */
	key_refused("svc key 8");		/* BAND */
	key_refused("svc key 9");		/* no such line */
	key_refused("svc key *");
	key_refused("svc key S");		/* never a typed value */
	assert(svc_line_denied("[2] NV Rebuild", (char[16]){0}, 16));
	assert(!svc_line_denied("[1] Debug Screen", (char[16]){0}, 16));
	assert(!svc_line_denied("[2] NOVEL", (char[16]){0}, 16));	/* whole words only */
	svc.last_key -= 1000;
	ctl("svc key 5");			/* "[ 5]" as the stock app reads it */
	static const unsigned char five[] = { '5' };
	expect(0x0b, 0x03, 3, five, 1);
	key_refused("svc key 1");		/* waiting for the screen of [5] */
	screen(lines, 2);
	svc.last_key -= 1000;
	ctl("svc key 1");
	static const unsigned char one[] = { '1' };
	expect(0x0b, 0x03, 3, one, 1);
	svc.last_key -= 1000;
	ctl("svc back");			/* back goes even while waiting */
	static const unsigned char back[] = { '\\' };
	expect(0x0b, 0x03, 3, back, 1);

	/* another mode: this one is left first */
	ctl("svc enter 2263");
	static const unsigned char end1[] = { 1 };
	expect(0x0b, 0x02, 3, end1, 1);
	static const unsigned char band[] = { 1, 0x16, 0x10 };
	expect(0x0b, 0x01, 3, band, 3);
	static const char *const top[] = { "[1] LTE BAND", "[2] Something" };
	screen(top, 2);
	svc.last_key -= 1000;
	key_refused("svc key 2");		/* look only, whatever the line says */
	ctl("svc back");
	expect(0x0b, 0x03, 3, back, 1);

	/* nobody asked for 90 s: left */
	svc.last_cmd = ms() - SVC_IDLE_MS - 1;
	assert(svc_idle_left() == -1);
	expect(0x0b, 0x02, 3, end1, 1);
	assert(file_has("state=off"));
	key_refused("svc back");		/* not in it */
}

static void test_cp_side(void)
{
	/* the CP refuses the enter */
	ctl("svc enter 9090");
	static const unsigned char diag[] = { 1, 0x13, 0x10 };
	expect(0x0b, 0x01, 3, diag, 3);
	unsigned char refused[] = { 0x0b, 0x01, 3, 0x02, 0x80 };
	ipc_message(0x80, 0x01, 2, 1, refused, sizeof(refused));
	assert(file_has("state=off") && file_has("0b01 answered 0x8002"));
	ctl("svc end");
	nothing_sent();

	/* the CP ends it with its last screen */
	ctl("svc enter 0011");
	drain();
	static const char *const bye[] = { "End service mode" };
	screen(bye, 1);
	assert(file_has("state=off"));

	/* the modem restarts: off, and no END goes to a CP that has forgotten */
	ctl("svc enter 0011");
	drain();
	ipc_message(0x01, 0x01, 3, 1, NULL, 0);
	assert(file_has("state=off") && file_has("note=the modem restarted"));
	drain();
	ctl("svc end");
	nothing_sent();

	/* the screen goes off: left */
	ctl("svc enter 0011");
	drain();
	ctl("screen off");
	int m, s, t;
	unsigned char data[600];
	size_t len;
	bool ended = false;
	while (sent_msg(&m, &s, &t, data, &len))
		ended = ended || (m == 0x0b && s == 0x02);
	assert(ended && file_has("state=off"));
	ctl("screen on");
	drain();

	/* codes not in the list are not entered */
	ctl("svc enter 33725327");		/* EFS clear */
	nothing_sent();
	assert(file_has("is not one of these"));
	ctl("svc enter 2767*2878");		/* NV reset */
	nothing_sent();

	/* a wide screen, a short one, an empty one */
	ctl("svc enter 0011");
	drain();
	unsigned char wide[1 + 42] = { 1, 0, 0 };
	memcpy(wide + 3, "0123456789012345678901234567890123456789", 40);
	ipc_message(0x0b, 0x16, 3, 1, wide, sizeof(wide));
	assert(file_has("line=0123456789012345678901234567890123456789\n"));
	unsigned char shortscr[10] = { 5, 0, 0, 'a', 'b' };
	ipc_message(0x0b, 0x05, 3, 1, shortscr, sizeof(shortscr));	/* count past the data */
	unsigned char empty[35] = { 1, 0, 1 };
	ipc_message(0x0b, 0x05, 3, 1, empty, sizeof(empty));
	assert(file_has("an empty screen"));
	ctl("svc end");
	drain();
}

static void test_netinfo(void)
{
	ctl("svc netinfo");
	expect(0x08, 0x0a, 2, NULL, 0);
	expect(0x08, 0x30, 2, NULL, 0);
	expect(0x08, 0x33, 2, NULL, 0);
	unsigned char mask = 0x0b;
	ipc_message(0x08, 0x0a, 2, 1, &mask, 1);
	unsigned char lock[7] = { 0, 1, 0, 2, 0, 0, 0 };
	ipc_message(0x08, 0x33, 2, 1, lock, 7);
	assert(file_has("net_mode=0x0b") && file_has("pci_lock=00010002000000"));
	unsigned char refused[] = { 0x08, 0x30, 2, 0x05, 0x80 };
	ipc_message(0x80, 0x01, 2, 1, refused, sizeof(refused));
	assert(file_has("ext_band=refused 8005"));
	nothing_sent();
}

int main(void)
{
	int sv[2];
	snprintf(errlog, sizeof(errlog), "/tmp/ztos-servicemode-test.log");
	assert(freopen(errlog, "w", stderr));
	setvbuf(stderr, NULL, _IOLBF, 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	ipc_fd = sv[0];
	ipc_peer = sv[1];
	test_field_test();
	test_menu_keys();
	test_cp_side();
	test_netinfo();
	unlink(SVC_FILE);
	unlink(errlog);
	printf("servicemode: all passed\n");
	return 0;
}
