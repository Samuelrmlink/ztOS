/* What the shell accepts from ztos-bt-agent: make check-bt-pair */
#include <assert.h>
#include "../auth/btpair-proto.h"

static bool parse(const char *text, struct bt_pair_request *r) {
	char line[512];
	snprintf(line, sizeof(line), "%s", text);
	return bt_pair_parse(line, r);
}

static void test_requests(void) {
	struct bt_pair_request r;
	assert(parse("1|pin|55|00:0D:44:EB:50:54||UE MINI BOOM", &r));
	assert(r.mode == BT_PAIR_PIN && r.seconds == 55);
	assert(!strcmp(r.id, "1") && !strcmp(r.addr, "00:0D:44:EB:50:54") && !strcmp(r.name, "UE MINI BOOM"));
	assert(parse("a|confirm|55||000042|Phone", &r) && !strcmp(r.value, "000042"));
	assert(parse("a|passkey|55|||Phone", &r) && r.mode == BT_PAIR_PASSKEY);
	assert(parse("a|show-pin|60||12ab|Phone", &r) && r.mode == BT_PAIR_SHOW_PIN);
	assert(parse("a|show-passkey|60||123456|Phone", &r) && r.mode == BT_PAIR_SHOW_PASSKEY);
	assert(parse("a|authorize|55|||Phone", &r) && r.mode == BT_PAIR_AUTHORIZE);
	assert(parse("a|service|55||audio and calls|Phone", &r) && !strcmp(r.value, "audio and calls"));
	assert(parse("a|service|55|||Phone", &r) && !r.value[0]);

	/* no name: the address, or something to read */
	assert(parse("a|pin|55|00:0D:44:EB:50:54||", &r) && !strcmp(r.name, "00:0D:44:EB:50:54"));
	assert(parse("a|pin|55|||", &r) && !strcmp(r.name, "A device"));

	static const char *bad[] = {
		"", "1", "1|pin|55", "1|pin|55|00:0D:44:EB:50:54|",	/* too few */
		"|pin|55|||x", "1 2|pin|55|||x", "123456789012|pin|55|||x", "1;id|pin|55|||x",	/* id */
		"1|sing|55|||x", "1||55|||x",				/* mode */
		"1|pin|0|||x", "1|pin|301|||x", "1|pin|ab|||x", "1|pin||||x", "1|pin|-5|||x", "1|pin|5x|||x",	/* seconds */
		"1|pin|55|00:0D:44|||x", "1|pin|55|00-0D-44-EB-50-54||x", "1|pin|55|00:0D:44:EB:50:5G||x",	/* address */
		"1|pin|55||1234|x",					/* nothing to carry */
		"1|confirm|55||12345|x", "1|confirm|55||1234567|x", "1|confirm|55||12a456|x", "1|confirm|55|||x",
		"1|show-passkey|55|||x", "1|show-pin|55|||x", "1|show-pin|55||12 34|x",
		"1|show-pin|55||12345678901234567|x",			/* too long for a PIN */
		"1|service|55||aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|x",	/* too long for the field */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (parse(bad[i], &r)) {
			printf("should not parse: %s\n", bad[i]);
			assert(0);
		}
	}
}

static void test_names(void) {
	char out[97];
	struct bt_pair_request r;

	/* a separator can't make a field */
	assert(parse("1|pin|55|||a|b|c", &r) && !strcmp(r.name, "a/b/c"));
	/* a newline, an escape: gone */
	bt_pair_clean(out, sizeof(out), "a\nb\rc\033[2Jd\x7f" "e");
	assert(!strcmp(out, "abc[2Jde"));
	/* the text that hides or reverses what is near it */
	bt_pair_clean(out, sizeof(out), "ab\xe2\x80\xae" "cd\xe2\x80\x8b" "e\xef\xbb\xbf" "f\xc2\x85" "g\xe2\x81\xa6h\xe2\x80\xa8i");
	assert(!strcmp(out, "abcdefghi"));
	/* broken UTF-8 goes, the rest stays */
	bt_pair_clean(out, sizeof(out), "a\xff" "b\xc3" "c\xed\xa0\x80" "d\xc0\xaf" "e\xf5\x80\x80\x80" "f");
	assert(!strcmp(out, "abcdef"));
	bt_pair_clean(out, sizeof(out), "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\xb5");
	assert(!strcmp(out, "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\xb5"));
	/* cut at a whole character, always terminated */
	char tiny[4];
	bt_pair_clean(tiny, sizeof(tiny), "a\xc3\xa9\xc3\xa9");
	assert(!strcmp(tiny, "a\xc3\xa9"));
	bt_pair_clean(tiny, sizeof(tiny), "ab\xc3\xa9");
	assert(!strcmp(tiny, "ab"));

	char big[600];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	char line[800];
	snprintf(line, sizeof(line), "1|pin|55|||%s", big);
	assert(bt_pair_parse(line, &r) && strlen(r.name) == sizeof(r.name) - 1);
}

static void test_entry(void) {
	assert(bt_pair_entry_takes(BT_PAIR_PIN, 0, "1") && bt_pair_entry_takes(BT_PAIR_PIN, 15, "a"));
	assert(!bt_pair_entry_takes(BT_PAIR_PIN, 16, "1"));
	assert(!bt_pair_entry_takes(BT_PAIR_PIN, 0, " ") && !bt_pair_entry_takes(BT_PAIR_PIN, 0, "|"));
	assert(!bt_pair_entry_takes(BT_PAIR_PIN, 0, "\t") && !bt_pair_entry_takes(BT_PAIR_PIN, 0, "\n"));
	assert(!bt_pair_entry_takes(BT_PAIR_PIN, 0, "\xc3\xa9") && !bt_pair_entry_takes(BT_PAIR_PIN, 0, ""));
	assert(!bt_pair_entry_takes(BT_PAIR_PIN, 0, NULL));
	assert(bt_pair_entry_takes(BT_PAIR_PASSKEY, 5, "0"));
	assert(!bt_pair_entry_takes(BT_PAIR_PASSKEY, 6, "0") && !bt_pair_entry_takes(BT_PAIR_PASSKEY, 0, "a"));
	assert(!bt_pair_entry_takes(BT_PAIR_CONFIRM, 0, "1"));	/* no field to type in */

	assert(!bt_pair_entry_done(BT_PAIR_PIN, 0) && bt_pair_entry_done(BT_PAIR_PIN, 1) && bt_pair_entry_done(BT_PAIR_PIN, 16));
	assert(!bt_pair_entry_done(BT_PAIR_PASSKEY, 5) && bt_pair_entry_done(BT_PAIR_PASSKEY, 6));
	assert(!bt_pair_entry_done(BT_PAIR_CONFIRM, 6));
}

static void test_reply(void) {
	char out[64];
	assert(bt_pair_reply_line(out, sizeof(out), "1f", true, "0000", false) == 11 && !strcmp(out, "1f\tOK\t0000\n"));
	assert(bt_pair_reply_line(out, sizeof(out), "1f", true, "0000", true) == 20 &&
		!strcmp(out, "1f\tOK\t0000\tremember\n"));
	assert(bt_pair_reply_line(out, sizeof(out), "1f", false, NULL, true) == 10 && !strcmp(out, "1f\tCANCEL\n"));
	assert(bt_pair_reply_line(out, 4, "1f", false, NULL, false) == -1);
}

int main(void) {
	test_requests();
	test_names();
	test_entry();
	test_reply();
	puts("PASS: bluetooth pairing requests, names, typed entry and replies");
	return 0;
}
