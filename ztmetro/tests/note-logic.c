/* What a swipe on a notification decides, and the lines that change one in place
 * (make check-notify). */
#include <assert.h>
#include <stdio.h>
#include "../note-logic.h"

#define W 360.0

static void test_which_gesture(void) {
	/* plainly sideways, either way */
	assert(note_swipe_wants(12, 0, true, true, true));
	assert(note_swipe_wants(-12, 3, true, true, true));
	/* plainly up or down: the list's scroll or the panel's close, untouched */
	assert(!note_swipe_wants(0, 12, true, true, true));
	assert(!note_swipe_wants(2, -40, true, true, true));
	/* a scroll that leans: on the diagonal, and just short of the ratio */
	assert(!note_swipe_wants(12, 12, true, true, true));
	assert(!note_swipe_wants(15, 10, true, true, true));
	assert(note_swipe_wants(16, 10, true, true, true));
	/* not on a notification (its app's name, a gap, a stopped coast) */
	assert(!note_swipe_wants(40, 0, false, true, true));
	/* the panel on its way down or up */
	assert(!note_swipe_wants(40, 0, true, false, true));
	/* a second finger, while one is swiping */
	assert(!note_swipe_wants(40, 0, true, true, false));
}

static void test_letting_go(void) {
	double line = W * NOTE_SWIPE_PAST;
	/* short of the line, slowly: back */
	assert(note_swipe_release(line - 1, 0, W, false) == NOTE_SWIPE_SETTLE);
	assert(note_swipe_release(-(line - 1), 0, W, false) == NOTE_SWIPE_SETTLE);
	assert(note_swipe_release(40, 0.2, W, false) == NOTE_SWIPE_SETTLE);
	/* past it: gone, the way it was going */
	assert(note_swipe_release(line, 0, W, false) == NOTE_SWIPE_FLY_RIGHT);
	assert(note_swipe_release(-line, 0, W, false) == NOTE_SWIPE_FLY_LEFT);
	/* a throw takes it from short of the line */
	assert(note_swipe_release(40, 0.8, W, false) == NOTE_SWIPE_FLY_RIGHT);
	assert(note_swipe_release(-40, -0.8, W, false) == NOTE_SWIPE_FLY_LEFT);
	/* but a flick that has hardly gone anywhere does not */
	assert(note_swipe_release(10, 2.0, W, false) == NOTE_SWIPE_SETTLE);
	/* thrown back toward its place: back, even from past the line */
	assert(note_swipe_release(line + 30, -0.8, W, false) == NOTE_SWIPE_SETTLE);
	assert(note_swipe_release(-(line + 30), 0.8, W, false) == NOTE_SWIPE_SETTLE);
	/* the wrong way, short of the line: back */
	assert(note_swipe_release(40, -0.8, W, false) == NOTE_SWIPE_SETTLE);
	/* a cancelled touch never dismisses */
	assert(note_swipe_release(W, 3.0, W, true) == NOTE_SWIPE_SETTLE);
	/* nowhere: nothing */
	assert(note_swipe_release(0, 3.0, W, false) == NOTE_SWIPE_SETTLE);
}

static void test_motion(void) {
	/* it arrives, exactly, in steps of a frame, and never overshoots */
	double x = 120;
	int frames = 0;
	while (!note_swipe_step(&x, 0, 16, 55, 0.05)) {
		assert(x > 0 && x < 120);
		assert(++frames < 60);
	}
	assert(x == 0);
	x = -120;
	frames = 0;
	while (!note_swipe_step(&x, -W, 16, 70, 1.0)) {
		assert(x < 0 && x > -W);
		assert(++frames < 30);
	}
	assert(x == -W);
	/* no time has passed: nothing moves, and nothing divides by it */
	x = 50;
	assert(!note_swipe_step(&x, 0, 0, 55, 0.05));
	assert(x == 50);
	assert(note_swipe_alpha(0, W) == 1.0f);
	assert(note_swipe_alpha(W / 2, W) == note_swipe_alpha(-W / 2, W));
	assert(note_swipe_alpha(W, W) > 0.0f);
	assert(note_swipe_alpha(5 * W, W) == note_swipe_alpha(W, W));
}

static void test_line(void) {
	struct note_line n;
	char a[] = "dl:grayjay:ab-12|q|42|app.grayjay.Grayjay|Grayjay|A title|About 42% (9 MB)";
	assert(note_line_parse(a, &n));
	assert(!strcmp(n.key, "dl:grayjay:ab-12") && n.quiet && n.percent == 42);
	assert(!strcmp(n.who, "app.grayjay.Grayjay") && !strcmp(n.app, "Grayjay"));
	assert(!strcmp(n.title, "A title") && !strcmp(n.text, "About 42% (9 MB)"));

	char b[] = "bus:7|-|-1|||Title|";
	assert(note_line_parse(b, &n));
	assert(!n.quiet && n.percent == NOTE_PLAIN && !n.who[0] && !n.app[0] && !n.text[0]);

	char c[] = "k|q|-2|w|a|t|has | in its text";
	assert(note_line_parse(c, &n) && n.percent == NOTE_BUSY && !strcmp(n.text, "has | in its text"));

	/* lines it should not believe */
	char d1[] = "k|q|42|w|a|t", d2[] = "|q|42|w|a|t|x", d3[] = "k|q|101|w|a|t|x", d4[] = "k|q|-3|w|a|t|x",
		d5[] = "k|q|4x|w|a|t|x", d6[] = "k|q||w|a|t|x", d7[] = "";
	assert(!note_line_parse(d1, &n));
	assert(!note_line_parse(d2, &n));
	assert(!note_line_parse(d3, &n));
	assert(!note_line_parse(d4, &n));
	assert(!note_line_parse(d5, &n));
	assert(!note_line_parse(d6, &n));
	assert(!note_line_parse(d7, &n));
	char big[NOTE_KEY_LEN + 20];
	memset(big, 'k', sizeof(big));
	strcpy(big + NOTE_KEY_LEN, "|q|1|w|a|t|x");
	assert(!note_line_parse(big, &n));
}

static void test_muting(void) {
	struct note_muted m = { 0 };
	/* nothing dismissed: everything through */
	assert(!note_muted_drops(&m, "a", 50));
	assert(!note_muted_drops(&m, "a", NOTE_PLAIN));
	/* swiped away: its progress stays away, with a number or without */
	note_muted_add(&m, "a");
	assert(note_muted_drops(&m, "a", 51));
	assert(note_muted_drops(&m, "a", NOTE_BUSY));
	assert(!note_muted_drops(&m, "b", 51));
	/* news gets through, and ends it */
	assert(!note_muted_drops(&m, "a", NOTE_PLAIN));
	assert(!note_muted_has(&m, "a"));
	assert(!note_muted_drops(&m, "a", 52));
	/* its sender closes it: over; the same name later is a new one */
	note_muted_add(&m, "a");
	note_muted_forget(&m, "a");
	assert(!note_muted_drops(&m, "a", 1));
	/* twice is once */
	note_muted_add(&m, "a");
	note_muted_add(&m, "a");
	assert(m.n == 1);
	note_muted_forget(&m, "a");
	assert(m.n == 0);
	/* one with no name can't be muted, nor one that can't be kept whole */
	note_muted_add(&m, "");
	assert(m.n == 0);
	char big[NOTE_KEY_LEN + 1];
	memset(big, 'k', NOTE_KEY_LEN);
	big[NOTE_KEY_LEN] = 0;
	note_muted_add(&m, big);
	assert(m.n == 0);
	/* full: the oldest goes, the rest stay in order */
	char key[8];
	for (int i = 0; i < NOTE_MUTED_MAX + 2; i++) {
		snprintf(key, sizeof(key), "k%d", i);
		note_muted_add(&m, key);
	}
	assert(m.n == NOTE_MUTED_MAX);
	assert(!note_muted_has(&m, "k0") && !note_muted_has(&m, "k1"));
	assert(note_muted_has(&m, "k2") && note_muted_has(&m, "k9"));
	/* out of the middle */
	note_muted_forget(&m, "k5");
	assert(m.n == NOTE_MUTED_MAX - 1 && !note_muted_has(&m, "k5") && note_muted_has(&m, "k6") && note_muted_has(&m, "k9"));
}

int main(void) {
	test_which_gesture();
	test_letting_go();
	test_motion();
	test_line();
	test_muting();
	puts("PASS: notification swipe decisions, keyed lines and muting");
	return 0;
}
