/* The lock screen's "what keeps it awake" reply, read into its struct
 * (make check-lock-awake): the lines zrn-sleep-why --shell writes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../lock-awake-parse.h"

static void test_reply(void) {
	struct la_data d;
	la_parse(&d,
		"line\tAwake: Brave playing \xc2\xb7 2 more\n"
		"item\taudio\tBrave playing\tbrave \xc2\xb7 pid 2345 \xc2\xb7 sound keeps the phone awake\n"
		"item\tgps\tLocation (GPS)\tztos-gnss-gate \xc2\xb7 pid 812\n"
		"item\taod\tAlways-on display\t\n"
		"item\tdisplay\tScreen on\tztmetro\n"
		"woke\tWoke 4 times in the last hour, mostly by the modem (umts_ipc0)\n"
		"window\tlast hour\n"
		"waker\tbrave\t412 wakeups a minute\n"
		"irq\tarch_timer\t900 interrupts a minute\n");
	assert(d.valid && la_line_shown(&d));
	assert(!strcmp(d.line, "Awake: Brave playing \xc2\xb7 2 more"));
	assert(d.n_items == 4);
	assert(!strcmp(d.items[0].kind, "audio") && !strcmp(d.items[0].label, "Brave playing"));
	assert(!strcmp(d.items[1].detail, "ztos-gnss-gate \xc2\xb7 pid 812"));
	assert(d.items[2].detail[0] == 0);
	assert(!strcmp(d.items[3].kind, "display"));
	assert(!strncmp(d.woke, "Woke 4 times", 12));
	assert(d.wakers_known && !strcmp(d.window, "last hour"));
	assert(d.n_wakers == 2 && !d.wakers[0].irq && d.wakers[1].irq);
	assert(!strcmp(d.wakers[1].rate, "900 interrupts a minute"));
}

static void test_nothing_to_say(void) {
	struct la_data d;
	la_parse(&d, "line\t\nitem\tdisplay\tScreen on\tztmetro\nwoke\tNo wakes from sleep in the last hour\n");
	assert(d.valid && !la_line_shown(&d));
	assert(d.n_items == 1 && !d.wakers_known);
	la_parse(&d, NULL);
	assert(!d.valid && !la_line_shown(&d));
	la_parse(&d, "");
	assert(d.valid && !la_line_shown(&d) && d.n_items == 0);
}

static void test_bad_lines(void) {
	struct la_data d;
	/* no label, no fields, an unknown tag, CRLF, a waker with no name */
	la_parse(&d, "item\taudio\n" "item\n" "item\tinhibit\t\tx\n" "whatever\tline\n"
		"line\tAwake: X\r\n" "waker\t\t5\n" "item\tcall\tPhone call\tztoscorenetworkingd");
	assert(!strcmp(d.line, "Awake: X"));
	assert(d.n_items == 1 && !strcmp(d.items[0].label, "Phone call"));
	assert(!strcmp(d.items[0].detail, "ztoscorenetworkingd"));
	assert(d.n_wakers == 0);
}

static void test_limits(void) {
	char text[8192] = "", line[256];
	for (int i = 0; i < LA_ITEMS + 5; i++) {
		snprintf(line, sizeof(line), "item\tinhibit\tHold %d\t\n", i);
		strcat(text, line);
	}
	struct la_data d;
	la_parse(&d, text);
	assert(d.n_items == LA_ITEMS);
	assert(!strcmp(d.items[LA_ITEMS - 1].label, "Hold 11"));
	/* a long label is cut, and not inside a character (the middle dot) */
	char longer[400] = "item\taudio\t";
	for (int i = 0; i < 39; i++) strcat(longer, "ab");
	strcat(longer, "\xc2\xb7\xc2\xb7\xc2\xb7tail\tdetail\n");
	la_parse(&d, longer);
	assert(d.n_items == 1);
	size_t n = strlen(d.items[0].label);
	assert(n < sizeof(d.items[0].label));
	assert(((unsigned char)d.items[0].label[n - 1] & 0xc0) != 0xc0);	/* no lead byte left alone */
	assert(!strcmp(d.items[0].detail, "detail"));
}

int main(void) {
	test_reply();
	test_nothing_to_say();
	test_bad_lines();
	test_limits();
	puts("lock-awake: ok");
	return 0;
}
