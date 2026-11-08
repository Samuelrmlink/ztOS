/* core/zt-clock.h: the time of day in both formats, and the setting read
 * from a config file. make check-clock */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/zt-clock.h"

static void expect(int hour, int min, bool h24, const char *want) {
	struct tm tm = { .tm_hour = hour, .tm_min = min };
	char out[32];
	zt_clock_text(&tm, h24, out, sizeof(out));
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "%02d:%02d %s: got \"%s\", want \"%s\"\n", hour, min, h24 ? "24h" : "12h", out, want);
		exit(1);
	}
}

static void write_config(const char *home, const char *text) {
	char path[512];
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home);
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
	/* a later mtime than the last read, even within the same tick */
	struct timespec t[2] = { { .tv_nsec = UTIME_NOW }, { .tv_sec = time(NULL) + rand() % 1000 + 1 } };
	utimensat(AT_FDCWD, path, t, 0);
}

int main(void) {
	expect(21, 41, true, "21:41");
	expect(21, 41, false, "9:41 PM");
	expect(9, 5, true, "09:05");
	expect(9, 5, false, "9:05 AM");
	expect(0, 0, false, "12:00 AM");
	expect(0, 0, true, "00:00");
	expect(12, 59, false, "12:59 PM");
	expect(11, 59, false, "11:59 AM");
	expect(13, 0, false, "1:00 PM");

	char ampm[4], hm[16];
	struct tm tm = { .tm_hour = 23, .tm_min = 7 };
	zt_clock_parts(&tm, true, hm, sizeof(hm), ampm, sizeof(ampm));
	assert(strcmp(hm, "23:07") == 0 && ampm[0] == 0);
	zt_clock_parts(&tm, false, hm, sizeof(hm), ampm, sizeof(ampm));
	assert(strcmp(hm, "11:07") == 0 && strcmp(ampm, "PM") == 0);

	char home[] = "/tmp/zt-clock-test-XXXXXX";
	assert(mkdtemp(home));
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0700);
	snprintf(dir, sizeof(dir), "%s/.config/ztmetro", home);
	mkdir(dir, 0700);
	setenv("HOME", home, 1);
	write_config(home, "accent = red\n");
	assert(zt_clock_24h() == true);		/* absent: 24-hour */
	write_config(home, "accent = red\nclock_24h = off\n");
	assert(zt_clock_24h() == false);
	write_config(home, "clock_24h = on\n");
	assert(zt_clock_24h() == true);
	printf("zt-clock: ok\n");
	return 0;
}
