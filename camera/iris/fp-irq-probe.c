// SPDX-License-Identifier: GPL-2.0
/*
 * fp-irq-probe: the fingerprint experiment of camera/iris/FINGERPRINT.md.
 * Approved by the owner, attended, 2026-11-12. Does the Egis ET510 raise its
 * data-ready line on a finger with no SPI set-up at all?
 *
 *   1. gpa1-3 (data-ready) requested as input with both-edge events
 *      (falls back to reading its level every millisecond if the line has
 *      no interrupt); its level is printed.
 *   2. gpm43-0 (sensor LDO) driven high.
 *   3. at least 1.1 ms later, expander 96-0022 line 6 (sleep/reset) high.
 *   4. edges on gpa1-3 printed for 30 s while the owner touches and lifts.
 *   5. line 6 low, then gpm43-0 low (stock's off order), lines released.
 * No SPI, no chip select, no SMC; gpp0 is not touched. Ctrl-C or SIGTERM
 * still runs step 5.
 *
 *   sudo ./fp-irq-probe            run (seconds of watching: --watch N, max 60)
 *   ./fp-irq-probe --dry-run       find the chips and print the plan, no request
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* /dev/gpiochipN whose label is `label`, or -1 */
static int find_chip(const char *label, char *path, size_t n)
{
	DIR *d = opendir("/dev");
	struct dirent *e;
	int found = -1;

	if (!d)
		return -1;
	while ((e = readdir(d)) && found < 0) {
		struct gpiochip_info info;
		int fd;

		if (strncmp(e->d_name, "gpiochip", 8))
			continue;
		snprintf(path, n, "/dev/%s", e->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) && !strcmp(info.label, label))
			found = fd;
		else
			close(fd);
	}
	closedir(d);
	return found;
}

static int request(int chip, unsigned int line, unsigned long long flags, int value, const char *who)
{
	struct gpio_v2_line_request req;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = line;
	req.num_lines = 1;
	req.config.flags = flags;
	if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = value ? 1 : 0;
		req.config.attrs[0].mask = 1;
	}
	snprintf(req.consumer, sizeof(req.consumer), "fp-irq-probe %s", who);
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
		return -errno;
	return req.fd;
}

static int set_value(int fd, int v)
{
	struct gpio_v2_line_values lv = { .bits = v ? 1 : 0, .mask = 1 };

	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &lv) < 0 ? -errno : 0;
}

static int get_value(int fd)
{
	struct gpio_v2_line_values lv = { .mask = 1 };

	return ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &lv) < 0 ? -errno : (int)(lv.bits & 1);
}

int main(int argc, char **argv)
{
	char p_ldo[300], p_exp[300], p_irq[300];
	int c_ldo, c_exp, c_irq, ldo = -1, slp = -1, irq = -1, polled = 0, dry = 0;
	int watch = 30, edges = 0, rising = 0, falling = 0, last, i, rc = 0;
	double t_on = 0, t_end, next_prompt;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--watch") && i + 1 < argc)
			watch = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [--dry-run] [--watch SECONDS]\n", argv[0]);
			return 2;
		}
	}
	if (watch < 1 || watch > 60)
		watch = 30;

	c_ldo = find_chip("gpm43", p_ldo, sizeof(p_ldo));
	c_exp = find_chip("96-0022", p_exp, sizeof(p_exp));
	c_irq = find_chip("gpa1", p_irq, sizeof(p_irq));
	printf("plan: request gpa1-3 (%s) input, both edges; gpm43-0 (%s) high; >=1.1 ms; "
	       "96-0022 line 6 (%s) high; watch %d s; line 6 low; gpm43-0 low\n",
	       c_irq >= 0 ? p_irq : "not found", c_ldo >= 0 ? p_ldo : "not found",
	       c_exp >= 0 ? p_exp : "not found", watch);
	if (dry)
		return 0;
	if (c_ldo < 0 || c_exp < 0 || c_irq < 0) {
		fprintf(stderr, "a GPIO chip is missing; nothing done\n");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* 1 */
	irq = request(c_irq, 3, GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
		      GPIO_V2_LINE_FLAG_EDGE_FALLING, 0, "drdy");
	if (irq < 0) {
		printf("gpa1-3 edge request: %s; reading its level every 1 ms instead\n", strerror(-irq));
		irq = request(c_irq, 3, GPIO_V2_LINE_FLAG_INPUT, 0, "drdy");
		polled = 1;
	}
	if (irq < 0) {
		fprintf(stderr, "gpa1-3: %s; nothing powered\n", strerror(-irq));
		return 1;
	}
	last = get_value(irq);
	printf("%.3f gpa1-3 = %d before power\n", 0.0, last);

	/* 2 */
	ldo = request(c_ldo, 0, GPIO_V2_LINE_FLAG_OUTPUT, 1, "ldo");
	if (ldo < 0) {
		fprintf(stderr, "gpm43-0: %s\n", strerror(-ldo));
		rc = 1;
		goto off;
	}
	t_on = now();
	printf("0.000 gpm43-0 high (sensor LDO)\n");
	/* 3 */
	usleep(1100);
	while (now() - t_on < 0.0011)
		;
	slp = request(c_exp, 6, GPIO_V2_LINE_FLAG_OUTPUT, 1, "sleep");
	if (slp < 0) {
		fprintf(stderr, "96-0022 line 6: %s\n", strerror(-slp));
		rc = 1;
		goto off;
	}
	printf("%.4f 96-0022 line 6 high (sleep/reset released)\n", now() - t_on);

	/* 4 */
	t_end = now() + watch;
	next_prompt = now() + 3;
	while (!stop && now() < t_end) {
		if (now() >= next_prompt) {
			static int n;

			printf("%.1f >>> %s\n", now() - t_on, n++ % 2 ? "LIFT the finger" : "TOUCH the sensor");
			fflush(stdout);
			next_prompt += 4;
		}
		if (polled) {
			int v = get_value(irq);

			if (v >= 0 && v != last) {
				edges++;
				v ? rising++ : falling++;
				printf("%.4f gpa1-3 %s (polled)\n", now() - t_on, v ? "rose" : "fell");
				last = v;
			}
			usleep(1000);
		} else {
			struct pollfd pf = { .fd = irq, .events = POLLIN };
			struct gpio_v2_line_event ev;

			if (poll(&pf, 1, 100) > 0 && read(irq, &ev, sizeof(ev)) == sizeof(ev)) {
				int up = ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE;

				edges++;
				up ? rising++ : falling++;
				printf("%.4f gpa1-3 %s\n", ev.timestamp_ns / 1e9 - t_on, up ? "rose" : "fell");
			}
		}
		fflush(stdout);
	}
	printf("%.1f watch over: %d edges (%d rising, %d falling), level now %d%s\n", now() - t_on,
	       edges, rising, falling, get_value(irq), stop ? " (stopped early)" : "");

off:
	/* 5: stock's off order, sleep pin then LDO */
	if (slp >= 0) {
		if (set_value(slp, 0))
			rc = 1;
		printf("96-0022 line 6 low\n");
		close(slp);
	}
	if (ldo >= 0) {
		if (set_value(ldo, 0))
			rc = 1;
		printf("gpm43-0 low\n");
		close(ldo);
	}
	if (irq >= 0)
		close(irq);
	close(c_ldo);
	close(c_exp);
	close(c_irq);
	return rc;
}
