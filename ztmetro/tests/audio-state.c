#include <assert.h>
#include <stdlib.h>
#include "../audio-state.h"

static const char REAL[] =
	"{\"version\": 1, \"active\": {\"name\": \"bluez_output.00_0D_44_EB_50_54.1\", \"desc\": \"UE MINI BOOM\", "
	"\"kind\": \"bluetooth\", \"percent\": 6, \"mute\": false}, \"outputs\": ["
	"{\"name\": \"bluez_output.00_0D_44_EB_50_54.1\", \"desc\": \"UE MINI BOOM\", \"kind\": \"bluetooth\", "
	"\"percent\": 6, \"mute\": false, \"current\": true}, "
	"{\"name\": \"ztos-speaker\", \"desc\": \"Speaker\", \"kind\": \"speaker\", \"percent\": 70, \"mute\": true, "
	"\"current\": false}], \"apps\": [{\"key\": \"Output/Audio:application.name:pw-play\", \"name\": \"pw-play\", "
	"\"running\": false, \"streams\": [1, 2], \"percent\": null, \"mute\": false, \"playing_on\": null, "
	"\"output\": \"default\", \"output_present\": true}]}";

static bool parse(const char *text, struct audio_state *st) {
	return audio_state_parse(text, strlen(text), st);
}

static void test_real(void) {
	struct audio_state st;
	assert(parse(REAL, &st));
	assert(st.n == 2);
	assert(!strcmp(st.out[0].name, "bluez_output.00_0D_44_EB_50_54.1"));
	assert(!strcmp(st.out[0].desc, "UE MINI BOOM"));
	assert(st.out[0].percent == 6 && st.out[0].current && !st.out[0].mute);
	assert(st.out[1].percent == 70 && !st.out[1].current && st.out[1].mute);
	assert(audio_current(&st) == 0);
	assert(audio_find(&st, "ztos-speaker") == 1);
	assert(audio_find(&st, "nope") == -1);
}

/* the file cut short anywhere, as if the write had stopped there */
static void test_truncated(void) {
	struct audio_state st = { .n = 5 };
	size_t len = strlen(REAL);
	for (size_t i = 0; i < len; i++) {
		char *copy = malloc(i ? i : 1);	/* exactly i bytes: a read past them is caught */
		memcpy(copy, REAL, i);
		assert(!audio_state_parse(copy, i, &st));
		assert(st.n == 5);	/* the last good state stands */
		free(copy);
	}
}

/* no byte, changed to anything, reads out of bounds or hangs */
static void test_flipped(void) {
	static const char odd[] = "\"\\{}[],:0-eu\x01\x7f\x80\xc0\xff";
	size_t len = strlen(REAL);
	for (size_t i = 0; i < len; i++) {
		for (size_t k = 0; k + 1 < sizeof(odd); k++) {
			char *copy = malloc(len);
			memcpy(copy, REAL, len);
			copy[i] = odd[k];
			struct audio_state st;
			if (audio_state_parse(copy, len, &st)) {
				assert(st.n >= 0 && st.n <= AUDIO_MAX_OUTPUTS);
				for (int n = 0; n < st.n; n++) {
					assert(st.out[n].percent >= -1 && st.out[n].percent <= 100);
					assert(st.out[n].name[0]);
				}
			}
			free(copy);
		}
	}
}

static void test_malformed(void) {
	static const char *const bad[] = {
		"", " ", "null", "[]", "{}", "{", "}", "{\"outputs\"", "{\"outputs\":", "{\"outputs\":[",
		"{\"outputs\":[}", "{\"outputs\":{}}", "{\"outputs\":[1]}", "{\"outputs\":[]} x", "{\"outputs\":[],}",
		"{\"apps\":[]}",	/* no outputs at all */
		"{\"version\":2,\"outputs\":[]}", "{\"version\":\"1\",\"outputs\":[]}",
		"{\"outputs\":[{\"name\":\"a\"]}", "{\"outputs\":[{\"name\":\"a\",}]}", "{\"outputs\":[{\"name\"}]}",
		"{\"outputs\":[{\"name\":\"a\\q\"}]}", "{\"outputs\":[{\"name\":\"a\\u12\"}]}",
		"{\"outputs\":[{\"name\":\"a\nb\"}]}", "{\"outputs\":[{\"name\":\"\xff\"}]}",
		"{\"outputs\":[{\"name\":\"\xc3\"}]}", "{\"outputs\":[{\"name\":\"\xed\xa0\x80\"}]}",
		"{\"outputs\":[{\"name\":\"a\",\"percent\":-}]}", "{\"outputs\":[{\"name\":\"a\",\"percent\":01}]}",
		"{\"outputs\":[{\"name\":\"a\",\"percent\":1.}]}", "{\"outputs\":[{\"name\":\"a\",\"percent\":1e}]}",
		"{\"outputs\":[{\"name\":\"a\",\"mute\":tru}]}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct audio_state st = { .n = 7 };
		assert(!parse(bad[i], &st));
		assert(st.n == 7);
	}
	/* nesting past what is allowed fails, and doesn't use up the stack */
	static char deep[60100];
	memcpy(deep, "{\"apps\":", 8);
	memset(deep + 8, '[', 60000);
	struct audio_state st;
	assert(!parse(deep, &st));
	/* bigger than this daemon writes */
	static char huge[AUDIO_STATE_MAX_BYTES + 2];
	memset(huge, ' ', sizeof(huge) - 1);
	huge[0] = '{';
	assert(!parse(huge, &st));
}

static void test_values(void) {
	struct audio_state st;
	/* percent: null, past the ends, rounded; other types are as good as absent */
	assert(parse("{\"outputs\":[{\"name\":\"a\",\"percent\":null},{\"name\":\"b\",\"percent\":140},"
		"{\"name\":\"c\",\"percent\":-3},{\"name\":\"d\",\"percent\":42.6},{\"name\":\"e\",\"percent\":\"9\"},"
		"{\"name\":\"f\",\"percent\":99999999999999999999},{\"name\":\"g\",\"percent\":1e2},{\"name\":\"h\"}]}", &st));
	assert(st.n == 8);
	assert(st.out[0].percent == -1 && st.out[1].percent == 100 && st.out[2].percent == 0);
	assert(st.out[3].percent == 43 && st.out[4].percent == -1 && st.out[5].percent == 100);
	assert(st.out[6].percent == -1 && st.out[7].percent == -1);
	/* more than fit: the rest ignored, not an error */
	assert(parse("{\"outputs\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"},{\"name\":\"d\"},{\"name\":\"e\"},"
		"{\"name\":\"f\"},{\"name\":\"g\"},{\"name\":\"h\"},{\"name\":\"i\"}]}", &st));
	assert(st.n == AUDIO_MAX_OUTPUTS && audio_find(&st, "i") == -1);
	/* no name, or a wrong one: dropped */
	assert(parse("{\"outputs\":[{\"desc\":\"x\"},{},{\"name\":\"\"},{\"name\":5},{\"name\":\"ok\"}]}", &st));
	assert(st.n == 1 && !strcmp(st.out[0].name, "ok"));
	/* a name too long to keep whole would be some other output: dropped */
	char big[400], text[600];
	memset(big, 'n', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	snprintf(text, sizeof(text), "{\"outputs\":[{\"name\":\"%s\"},{\"name\":\"ok\"}]}", big);
	assert(parse(text, &st));
	assert(st.n == 1 && !strcmp(st.out[0].name, "ok"));
	/* a long description is only shortened, and on a whole character */
	char wide[300] = "";
	for (int i = 0; i < 90; i++) strcat(wide, "\xc3\xa9");
	snprintf(text, sizeof(text), "{\"outputs\":[{\"name\":\"a\",\"desc\":\"%s\"}]}", wide);
	assert(parse(text, &st));
	assert(st.n == 1 && strlen(st.out[0].desc) == 94);
	/* only one is current; escapes, which is how Python writes anything not ASCII */
	assert(parse("{\"outputs\":[{\"name\":\"a\",\"current\":true},{\"name\":\"b\",\"current\":true,"
		"\"desc\":\"caf\\u00e9 \\ud83d\\udd0a \\ud83d x\\ty\\u0001\\\"\"}]}", &st));
	assert(st.out[0].current && !st.out[1].current);
	assert(!strcmp(st.out[1].desc, "caf\xc3\xa9 \xf0\x9f\x94\x8a \xef\xbf\xbd x y \""));
	/* nobody connected */
	assert(parse("{\"version\":1,\"active\":null,\"outputs\":[],\"apps\":[]}", &st));
	assert(st.n == 0 && audio_current(&st) == -1);
}

static const char *label(const char *kind, const char *detail) {
	struct audio_output o = { .percent = 1 };
	snprintf(o.kind, sizeof(o.kind), "%s", kind);
	snprintf(o.detail, sizeof(o.detail), "%s", detail);
	return audio_type_of(&o)->label;
}

static void test_types(void) {
	assert(!strcmp(label("speaker", ""), "Phone Speaker"));
	assert(!strcmp(label("bluetooth", ""), "A2DP"));
	assert(!strcmp(label("bluetooth", "later"), "A2DP"));
	/* says what the daemon knows, and claims no jack or USB */
	assert(!strcmp(label("headphones", ""), "Headphones"));
	assert(!strcmp(label("", ""), "Audio Output"));
	assert(!strcmp(label("something-new", ""), "Audio Output"));
	for (const struct audio_type *t = AUDIO_TYPES;; t++) {
		assert(t->label[0] && t->icon[0]);
		if (!t->kind) break;
	}
	struct audio_state st;
	assert(parse("{\"outputs\":[{\"name\":\"a\",\"kind\":\"headphones\",\"detail\":\"x\"}]}", &st));
	assert(!strcmp(st.out[0].detail, "x"));
	char caps[32];
	audio_label_caps(caps, sizeof(caps), "Phone Speaker");
	assert(!strcmp(caps, "PHONE SPEAKER"));
	audio_label_caps(caps, 4, "A2DP");
	assert(!strcmp(caps, "A2D"));
}

static void test_selection(void) {
	struct audio_state st;
	assert(parse(REAL, &st));
	assert(audio_selected(&st, "") == 0);
	assert(audio_selected(&st, "ztos-speaker") == 1);
	assert(audio_selected(&st, "gone") == 0);
	assert(!audio_pending_over(&st, "ztos-speaker"));
	assert(audio_pending_over(&st, "bluez_output.00_0D_44_EB_50_54.1"));
	assert(audio_pending_over(&st, "gone"));
}

static void test_commands(void) {
	char line[200];
	assert(audio_cmd_output(line, sizeof(line), "ztos-speaker"));
	assert(!strcmp(line, "output 'ztos-speaker'"));
	assert(audio_cmd_volume(line, sizeof(line), 42, "bluez_output.AA.1"));
	assert(!strcmp(line, "volume 42 'bluez_output.AA.1'"));
	assert(!audio_cmd_output(line, sizeof(line), "it's"));
	assert(!audio_cmd_volume(line, sizeof(line), 1, "a\nrefresh"));
	assert(!audio_cmd_output(line, sizeof(line), ""));
	assert(!audio_cmd_output(line, 8, "too long for it"));
	/* the longest name there can be still fits the line the shell uses */
	char longest[sizeof(((struct audio_output *)0)->name)];
	memset(longest, 'n', sizeof(longest) - 1);
	longest[sizeof(longest) - 1] = 0;
	assert(audio_cmd_volume(line, sizeof(line), 100, longest));
}

static void test_slider(void) {
	/* the ends of the track and past them, on the daemon's scale */
	assert(audio_slider_percent(24, 24, 312) == 0);
	assert(audio_slider_percent(336, 24, 312) == 100);
	assert(audio_slider_percent(180, 24, 312) == 50);
	assert(audio_slider_percent(-50, 24, 312) == 0);
	assert(audio_slider_percent(900, 24, 312) == 100);
	assert(audio_slider_percent(100, 24, 0) == 0);

	/* the first goes at once, the next waits its turn, and no more go than that */
	struct audio_slider s = { 0 };
	assert(audio_slider_due(&(struct audio_slider){ .percent = 3, .sent = 3 }, 100) == -1);
	audio_slider_begin(&s, "ztos-speaker", 40);
	assert(s.dragging && audio_slider_due(&s, 1000) == 0);
	audio_slider_mark_sent(&s, 1000);
	assert(audio_slider_due(&s, 1001) == -1);
	s.percent = 41;
	assert(audio_slider_due(&s, 1020) == AUDIO_SLIDER_GAP_MS - 20);
	assert(audio_slider_due(&s, 1000 + AUDIO_SLIDER_GAP_MS) == 0);
	int sends = 0;
	uint32_t now = 2000;
	s.sent_ms = 0;
	for (int i = 0; i < 100; i++, now += 8) {	/* a sweep at 125 moves a second */
		s.percent = i;
		if (audio_slider_due(&s, now) == 0) {
			audio_slider_mark_sent(&s, now);
			sends++;
		}
	}
	assert(sends <= 8);
	/* a clock wrap is no matter */
	s.percent = 7;
	s.sent = 6;
	s.sent_ms = UINT32_MAX - 10;
	assert(audio_slider_due(&s, 20) == AUDIO_SLIDER_GAP_MS - 31);

	/* the device's echo doesn't move what's held, and does after */
	struct audio_state st;
	assert(parse(REAL, &st));
	audio_slider_begin(&s, "ztos-speaker", 30);
	audio_state_hold(&st, &s, 5000);
	assert(st.out[1].percent == 30 && !st.out[1].mute);	/* unmutes with it */
	assert(st.out[0].percent == 6);			/* the other one is left alone */
	audio_slider_mark_sent(&s, 5000);
	s.dragging = false;
	assert(parse(REAL, &st));
	audio_state_hold(&st, &s, 5000 + AUDIO_SYNC_QUIET_MS - 1);
	assert(st.out[1].percent == 30);
	assert(parse(REAL, &st));
	audio_state_hold(&st, &s, 5000 + AUDIO_SYNC_QUIET_MS);
	assert(st.out[1].percent == 70 && st.out[1].mute);
	/* to zero, its mute is left as it was; for an output gone, nothing */
	audio_slider_begin(&s, "ztos-speaker", 0);
	assert(parse(REAL, &st));
	audio_state_hold(&st, &s, 9000);
	assert(st.out[1].percent == 0 && st.out[1].mute);
	audio_slider_begin(&s, "gone", 50);
	assert(parse(REAL, &st));
	audio_state_hold(&st, &s, 9000);
	assert(st.out[0].percent == 6 && st.out[1].percent == 70);
	/* never touched: nothing held */
	struct audio_slider none = { 0 };
	assert(parse(REAL, &st));
	audio_state_hold(&st, &none, 9000);
	assert(st.out[1].percent == 70);
}

int main(void) {
	test_real();
	test_truncated();
	test_flipped();
	test_malformed();
	test_values();
	test_types();
	test_selection();
	test_commands();
	test_slider();
	puts("audio-state: ok");
	return 0;
}
