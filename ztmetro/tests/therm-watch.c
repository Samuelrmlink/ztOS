// SPDX-License-Identifier: GPL-2.0-or-later
/* ztnotifyd's battery-temperature-sensor watch (notifyd/therm-watch.h), in a
 * scratch directory: shown at start when the mark is there, shown when it
 * appears, closed when it goes, the parent directory made after the start,
 * and said again quietly when the shell restarts.
 *
 *   make check-therm-watch
 */
#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the watched paths, in a scratch directory */
static char base[64], dir[96], failed[128];
#define TW_PARENT base
#define TW_DIR dir
#define TW_FILE failed
#include "../notifyd/therm-watch.h"

static char last[512];
static int lines;

static void tell(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last, sizeof(last), fmt, ap);
	va_end(ap);
	lines++;
}

static int starts(const char *s, const char *with)
{
	return !strncmp(s, with, strlen(with));
}

static void settle(struct therm_watch *w)
{
	struct pollfd p = { .fd = w->fd, .events = POLLIN };
	while (poll(&p, 1, 100) > 0)
		tw_readable(w);
}

int main(void)
{
	snprintf(base, sizeof(base), "/tmp/therm-watch-XXXXXX");
	assert(mkdtemp(base));
	snprintf(dir, sizeof(dir), "%s/zrn-therm", base);
	snprintf(failed, sizeof(failed), "%s/load-failed", dir);

	struct therm_watch w;
	tw_init(&w, tell);
	assert(lines == 0);				/* nothing there: nothing said */

	assert(mkdir(dir, 0755) == 0);			/* the directory comes later */
	settle(&w);
	assert(lines == 0);
	FILE *f = fopen(failed, "w");
	fputs("1794460000\n", f);
	fclose(f);
	settle(&w);
	assert(lines == 1 && starts(last, "notekey zrn-therm|-|-1||System|Battery temperature sensor off|"));
	tw_look(&w, true);				/* the shell restarted */
	assert(lines == 2 && starts(last, "notekey zrn-therm|q|"));
	unlink(failed);
	settle(&w);
	assert(lines == 3 && !strcmp(last, "noteclose zrn-therm"));

	f = fopen(failed, "w");				/* at start, already there */
	fclose(f);
	struct therm_watch w2;
	lines = 0;
	tw_init(&w2, tell);
	assert(lines == 1 && starts(last, "notekey zrn-therm|-|"));
	unlink(failed);
	rmdir(dir);
	rmdir(base);
	puts("PASS: the failed-load mark is a notification while it is there");
	return 0;
}
