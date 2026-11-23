/* SPDX-License-Identifier: GPL-2.0 */
/*
 * When a reading still counts (zrn-therm.c, test-decode.c). A reading is good
 * only if the last attempt succeeded and it is no older than three sampling
 * periods and two seconds: a failed read, a stopped sampler or a long sleep
 * make it unknown, never an old "fine". Times in milliseconds.
 */
#ifndef THERM_FRESH_H
#define THERM_FRESH_H

struct therm_fresh {
	long long at_ms;	/* when the last good reading was taken */
	int ok;			/* the last attempt gave a good reading */
};

static inline void therm_fresh_set(struct therm_fresh *f, long long now_ms, int ok)
{
	f->ok = ok;
	if (ok)
		f->at_ms = now_ms;
}

static inline int therm_fresh_ok(const struct therm_fresh *f, long long now_ms, long long period_ms)
{
	return f->ok && f->at_ms > 0 && now_ms - f->at_ms <= 3 * period_ms + 2000;
}
#endif
