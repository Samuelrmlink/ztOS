/* vt: play rumble pulses on zrn-haptic. vt GAP_MS LEN:PCT [LEN:PCT ...] */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int open_dev(void) {
	for (int i = 0; i < 16; i++) {
		char path[96], name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (!strcmp(name, "zrn-haptic")) {
			snprintf(path, sizeof(path), "/dev/input/event%d", i);
			return open(path, O_RDWR);
		}
	}
	return -1;
}

int main(int argc, char **argv) {
	int fd = open_dev();
	if (fd < 0 || argc < 3) { fprintf(stderr, "no device / usage\n"); return 1; }
	int gap = atoi(argv[1]);
	usleep(400000);
	for (int i = 2; i < argc; i++) {
		int len = 0, pct = 0;
		sscanf(argv[i], "%d:%d", &len, &pct);
		struct ff_effect e = { .type = FF_RUMBLE, .id = -1, .replay = { .length = len } };
		e.u.rumble.strong_magnitude = e.u.rumble.weak_magnitude = 0xffff * pct / 100;
		if (ioctl(fd, EVIOCSFF, &e) < 0) { perror("EVIOCSFF"); return 1; }
		struct input_event ev = { .type = EV_FF, .code = e.id, .value = 1 };
		struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
		if (write(fd, &ev, sizeof(ev)) < 0) perror("write");
		printf("%ld.%06ld play %d ms %d%%\n", t.tv_sec, t.tv_nsec / 1000, len, pct);
		usleep(gap * 1000);
		ioctl(fd, EVIOCRMFF, e.id);
	}
	return 0;
}
