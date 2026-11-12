// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of zrn-therm's decoding (therm-decode.h) against stock's tables
 * and stock's arithmetic, and of its freshness rule (therm-fresh.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -o /tmp/test-decode test-decode.c && /tmp/test-decode
 */
#include <assert.h>
#include <stdio.h>

#include "therm-tables.h"
#include "therm-fresh.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* stock's sec_bat_get_value_by_adc(), copied as written (clamping ends) */
static int stock_decode(const struct therm_table *t, int adc)
{
	int low = 0, high, mid, temp;

	if (t->adc[0] >= adc)
		return t->deci[0];
	if (t->adc[t->n - 1] <= adc)
		return t->deci[t->n - 1];
	high = t->n - 1;
	while (low <= high) {
		mid = (low + high) / 2;
		if (t->adc[mid] > adc)
			high = mid - 1;
		else if (t->adc[mid] < adc)
			low = mid + 1;
		else
			return t->deci[mid];
	}
	temp = t->deci[high];
	temp += ((t->deci[low] - t->deci[high]) * (adc - t->adc[high])) / (t->adc[low] - t->adc[high]);
	return temp;
}

int main(void)
{
	const struct therm_table *bt = &therm_channels[0].t;
	int d, i, c;

	/* the channels and their tables, as stock's rev26 tree has them */
	CHECK(THERM_N_CHANNELS == 6, "%d channels", THERM_N_CHANNELS);
	CHECK(therm_channels[0].ch == 4 && therm_channels[1].ch == 1 && therm_channels[2].ch == 3 &&
	      therm_channels[3].ch == 0 && therm_channels[4].ch == 2 && therm_channels[5].ch == 10, "channel numbers");
	CHECK(bt->n == 23 && bt->adc[0] == 275 && bt->deci[0] == 900 && bt->adc[22] == 3676 && bt->deci[22] == -200,
	      "battery table ends");
	CHECK(therm_channels[5].t.n == 111 && therm_channels[5].t.deci[0] == -200, "PA table");

	/* every table point decodes to itself */
	for (c = 0; c < THERM_N_CHANNELS; c++) {
		const struct therm_table *t = &therm_channels[c].t;
		for (i = 0; i < t->n; i++)
			CHECK(therm_decode(t, t->adc[i], &d) == 0 && d == t->deci[i], "%s point %d", therm_channels[c].name, i);
	}
	/* between points: stock's integer interpolation, for every ADC value in the table */
	for (c = 0; c < THERM_N_CHANNELS; c++) {
		const struct therm_table *t = &therm_channels[c].t;
		for (i = t->adc[0]; i <= t->adc[t->n - 1]; i++)
			if (therm_decode(t, i, &d) != 0 || d != stock_decode(t, i)) {
				CHECK(0, "%s adc %d: %d, stock %d", therm_channels[c].name, i, d, stock_decode(t, i));
				break;
			}
	}
	/* a known point by hand: battery ADC 1888 lies between 1769 (30.0 C) and 2007 (25.0 C) */
	CHECK(therm_decode(bt, 1888, &d) == 0 && d == 300 + (250 - 300) * (1888 - 1769) / (2007 - 1769),
	      "1888 -> %d", d);
	/* past either end: unknown, never stock's clamped 90.0 or -20.0 */
	CHECK(therm_decode(bt, 274, &d) == -1, "below the table");
	CHECK(therm_decode(bt, 3677, &d) == -1, "above the table");
	CHECK(therm_decode(bt, 0, &d) == -1, "0 (shorted)");
	CHECK(therm_decode(bt, 4095, &d) == -1, "4095 (open)");

	/* the trimmed mean: stock's, including its else-if */
	{
		int a[5] = { 1500, 1490, 1510, 1495, 1505 };
		CHECK(therm_trim_mean(a, 5) == (1500 + 1495 + 1505) / 3, "trim %d", therm_trim_mean(a, 5));
		int b[5] = { 1000, 1000, 1000, 1000, 4095 };	/* one wild sample is dropped */
		CHECK(therm_trim_mean(b, 5) == 1000, "trim wild %d", therm_trim_mean(b, 5));
		int e[5] = { 4095, 1000, 1001, 999, 1000 };	/* the first one the highest */
		CHECK(therm_trim_mean(e, 5) == 1000, "trim first high %d", therm_trim_mean(e, 5));
		int f[5] = { 10, 20, 30, 40, 50 };	/* rising: stock's else-if never sees a new min */
		CHECK(therm_trim_mean(f, 5) == (10 + 20 + 30 + 40 + 50 - 50 - 10) / 3, "trim rising %d", therm_trim_mean(f, 5));
	}

	/* the gauge's Temp register, as stock writes it, read back as mainline does */
	CHECK(therm_gauge_reg(333) == 0x214c, "33.3 C -> %04x (stock writes 21 4c)", therm_gauge_reg(333));
	CHECK(therm_gauge_read(0x214c) == 332, "0x214c reads %d (mainline truncates)", therm_gauge_read(0x214c));
	for (d = -200; d <= 900; d++) {
		int back = therm_gauge_read(therm_gauge_reg(d));
		if (back < d - 1 || back > d + 1) {
			CHECK(0, "%d written, %d read back", d, back);
			break;
		}
	}
	CHECK(therm_gauge_reg(0) == 0, "0 C");
	CHECK(therm_gauge_reg(-50) == 0xfb00, "-5.0 C -> %04x", therm_gauge_reg(-50));

	/* freshness: a reading counts only while young enough, and never after a failure */
	{
		struct therm_fresh f = { 0 };
		CHECK(!therm_fresh_ok(&f, 1000, 5000), "never read: unknown");
		therm_fresh_set(&f, 1000, 1);
		CHECK(therm_fresh_ok(&f, 1000, 5000), "just read");
		CHECK(therm_fresh_ok(&f, 1000 + 3 * 5000 + 2000, 5000), "at the limit");
		CHECK(!therm_fresh_ok(&f, 1000 + 3 * 5000 + 2001, 5000), "stale");
		therm_fresh_set(&f, 2000, 0);
		CHECK(!therm_fresh_ok(&f, 2000, 5000), "failed read: unknown at once");
		therm_fresh_set(&f, 3000, 1);
		CHECK(therm_fresh_ok(&f, 3000, 60000), "good again");
		CHECK(!therm_fresh_ok(&f, 3000 + 3 * 60000 + 2001, 60000), "stale on the slow period");
	}

	printf(fails ? "test-decode: %d failures\n" : "test-decode: all ok\n", fails);
	return fails != 0;
}
