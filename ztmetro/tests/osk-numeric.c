#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../osk-numeric.h"

static const enum osk_mode pads[] = { OSK_MODE_DIGITS, OSK_MODE_NUMBER, OSK_MODE_PHONE };

/* every purpose in text-input-unstable-v3 */
static void test_purposes(void) {
	enum { NORMAL, ALPHA, DIGITS, NUMBER, PHONE, URL, EMAIL, NAME, PASSWORD, PIN, DATE, TIME,
		DATETIME, TERMINAL };
	assert(DIGITS == OSK_PURPOSE_DIGITS && NUMBER == OSK_PURPOSE_NUMBER &&
		PHONE == OSK_PURPOSE_PHONE && PIN == OSK_PURPOSE_PIN);
	assert(osk_mode_for_purpose(DIGITS) == OSK_MODE_DIGITS);
	assert(osk_mode_for_purpose(PIN) == OSK_MODE_DIGITS);
	assert(osk_mode_for_purpose(NUMBER) == OSK_MODE_NUMBER);
	assert(osk_mode_for_purpose(PHONE) == OSK_MODE_PHONE);
	static const int full[] = { NORMAL, ALPHA, URL, EMAIL, NAME, PASSWORD, DATE, TIME, DATETIME,
		TERMINAL, 99 };
	for (size_t i = 0; i < sizeof(full) / sizeof(full[0]); i++) {
		assert(osk_mode_for_purpose(full[i]) == OSK_MODE_FULL);
	}
}

static void test_names(void) {
	static const enum osk_mode all[] = { OSK_MODE_FULL, OSK_MODE_DIGITS, OSK_MODE_NUMBER, OSK_MODE_PHONE };
	for (size_t i = 0; i < 4; i++) {
		assert(osk_mode_from_name(osk_mode_name(all[i])) == all[i]);
	}
	assert(osk_mode_from_name("nonsense") == OSK_MODE_FULL);
	assert(osk_mode_from_name(NULL) == OSK_MODE_FULL);
}

static void test_choice(void) {
	/* a shell field with no request: letters, whatever the app has focused.
	 * This is the password prompt. */
	assert(osk_mode_choose(true, OSK_MODE_FULL, OSK_MODE_DIGITS, OSK_MODE_PHONE) == OSK_MODE_FULL);
	/* the PIN prompt asks */
	assert(osk_mode_choose(true, OSK_MODE_DIGITS, OSK_MODE_FULL, OSK_MODE_FULL) == OSK_MODE_DIGITS);
	assert(osk_mode_choose(false, OSK_MODE_DIGITS, OSK_MODE_PHONE, OSK_MODE_NUMBER) == OSK_MODE_DIGITS);
	/* the app's field, then what the app asked for */
	assert(osk_mode_choose(false, OSK_MODE_FULL, OSK_MODE_NUMBER, OSK_MODE_PHONE) == OSK_MODE_NUMBER);
	assert(osk_mode_choose(false, OSK_MODE_FULL, OSK_MODE_FULL, OSK_MODE_PHONE) == OSK_MODE_PHONE);
	assert(osk_mode_choose(false, OSK_MODE_FULL, OSK_MODE_FULL, OSK_MODE_FULL) == OSK_MODE_FULL);
	/* letters only where the pad offers them */
	assert(osk_mode_shown(OSK_MODE_NUMBER, true) == OSK_MODE_FULL);
	assert(osk_mode_shown(OSK_MODE_PHONE, true) == OSK_MODE_FULL);
	assert(osk_mode_shown(OSK_MODE_DIGITS, true) == OSK_MODE_DIGITS);
	assert(osk_mode_shown(OSK_MODE_NUMBER, false) == OSK_MODE_NUMBER);
}

static void test_pads(void) {
	int n, columns;
	assert(osk_pad_keys(OSK_MODE_FULL, &n, &columns) == NULL && n == 0);
	for (size_t p = 0; p < sizeof(pads) / sizeof(pads[0]); p++) {
		const struct osk_pad_key *keys = osk_pad_keys(pads[p], &n, &columns);
		assert(keys);
		int covered[OSK_PAD_ROWS][4] = {{0}};
		int digits = 0, backspace = 0, enter = 0, letters = 0;
		for (int i = 0; i < n; i++) {
			const struct osk_pad_key *k = &keys[i];
			assert((k->label != NULL) != (k->icon != NULL));
			for (int r = k->row; r < k->row + k->rows; r++) {
				for (int c = k->col; c < k->col + k->cols; c++) {
					assert(r < OSK_PAD_ROWS && c < columns);
					covered[r][c]++;
				}
			}
			if (k->label && k->label[0] >= '0' && k->label[0] <= '9') {
				/* phone-keypad order, 0 under 8 */
				int d = k->label[0] - '0';
				assert(k->keycode == (d ? (uint32_t)KEY_1 + d - 1 : KEY_0) && !k->shift);
				assert(d ? (k->col == (d - 1) % 3 && k->row == (d - 1) / 3)
					: (k->col == 1 && k->row == 3));
				digits++;
			}
			backspace += k->keycode == KEY_BACKSPACE;
			enter += k->keycode == KEY_ENTER;
			letters += k->page;
		}
		/* the grid is filled exactly, so touches always land on a key */
		for (int r = 0; r < OSK_PAD_ROWS; r++) {
			for (int c = 0; c < columns; c++) {
				assert(covered[r][c] == 1);
			}
		}
		assert(digits == 10 && backspace == 1 && enter == 1);
		assert(letters == (pads[p] == OSK_MODE_DIGITS ? 0 : 1));
		/* only the keys that field can use */
		assert(n == (pads[p] == OSK_MODE_DIGITS ? 12 : pads[p] == OSK_MODE_NUMBER ? 15 : 16));
	}
}

static const struct osk_pad_key *find(const char *label, enum osk_mode mode) {
	int n, columns;
	const struct osk_pad_key *keys = osk_pad_keys(mode, &n, &columns);
	for (int i = 0; i < n; i++) {
		if (keys[i].label && !strcmp(keys[i].label, label)) return &keys[i];
	}
	return NULL;
}

static void test_symbols(void) {
	static const char *extra[] = { "-", ".", "*", "#", "+", "abc" };
	for (size_t i = 0; i < 6; i++) {
		assert(!find(extra[i], OSK_MODE_DIGITS));
	}
	assert(find("-", OSK_MODE_NUMBER)->keycode == KEY_MINUS && !find("-", OSK_MODE_NUMBER)->shift);
	assert(find(".", OSK_MODE_NUMBER)->keycode == KEY_DOT && !find(".", OSK_MODE_NUMBER)->shift);
	assert(!find("*", OSK_MODE_NUMBER) && !find("+", OSK_MODE_NUMBER));
	assert(find("*", OSK_MODE_PHONE)->keycode == KEY_8 && find("*", OSK_MODE_PHONE)->shift);
	assert(find("#", OSK_MODE_PHONE)->keycode == KEY_3 && find("#", OSK_MODE_PHONE)->shift);
	assert(find("+", OSK_MODE_PHONE)->keycode == KEY_EQUAL && find("+", OSK_MODE_PHONE)->shift);
	assert(!find("-", OSK_MODE_PHONE) && !find(".", OSK_MODE_PHONE));
}

/* Every key height in Settings: the pad fills the width and its four rows, with
 * the same gaps as the letters, and no two keys touch. */
static void test_geometry(void) {
	const double gap = 4, width = 360, top = gap;
	for (int height = 42; height <= 72; height++) {
		for (size_t p = 0; p < sizeof(pads) / sizeof(pads[0]); p++) {
			int n, columns;
			const struct osk_pad_key *keys = osk_pad_keys(pads[p], &n, &columns);
			double right = 0, bottom = 0;
			for (int i = 0; i < n; i++) {
				double x, y, w, h;
				osk_pad_place(&keys[i], columns, width, height, gap, top, &x, &y, &w, &h);
				assert(x >= gap - 1e-9 && y >= top - 1e-9 && w >= 80 && h >= height - gap - 1e-9);
				if (x + w > right) right = x + w;
				if (y + h > bottom) bottom = y + h;
				for (int j = 0; j < i; j++) {
					double ox, oy, ow, oh;
					osk_pad_place(&keys[j], columns, width, height, gap, top, &ox, &oy, &ow, &oh);
					assert(x + w <= ox + 1e-9 || ox + ow <= x + 1e-9 ||
						y + h <= oy + 1e-9 || oy + oh <= y + 1e-9);
				}
			}
			assert(fabs(right - (width - gap)) < 1e-9);
			/* the same height as the letters' four rows */
			assert(fabs(bottom - (top + OSK_PAD_ROWS * height - gap)) < 1e-9);
		}
	}
}

int main(void) {
	test_purposes();
	test_names();
	test_choice();
	test_pads();
	test_symbols();
	test_geometry();
	puts("PASS: purposes, shell fields over app fields, pad contents and geometry");
	return 0;
}
