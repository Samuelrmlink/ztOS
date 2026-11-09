// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What ztacctd, the ztacct command and the "battery + data" app share: where
 * the daemon listens and how to ask it.
 *
 * /run/ztacct/sock, a stream socket (mode 0660, the phone user's group; the
 * daemon also checks SO_PEERCRED: root or the phone's user). One request line,
 * one JSON answer, then the daemon closes:
 *
 *	top [WINDOW]		every app (group) with its totals and its live
 *				processes' shares, sorted by energy
 *	app NAME [WINDOW]	one group: totals, processes (live and exited),
 *				and the union of its processes' open files
 *	proc PID		one process: memory, threads, I/O, open files,
 *				CPU, data, energy
 *	status			the daemon: its own cost, the gauge, calibration
 *	save			write the state file now
 *
 * WINDOW: "charge" (since the last full charge, the default), "today",
 * "24h", or "day:N" (N days ago, 0..6).
 */
#pragma once

#define ACCT_RUN	"/run/ztacct"
#define ACCT_SOCK	ACCT_RUN "/sock"
#define ACCT_STATUS	ACCT_RUN "/status"
#define ACCT_LIB	"/var/lib/ztacct"
#define ACCT_STATE	ACCT_LIB "/state"
#define ACCT_CALIB	ACCT_LIB "/calib.csv"
#define ACCT_CONF	"/etc/ztacct/power_profile"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>

/* Ask the daemon; returns a malloc'd answer (NUL-terminated) or NULL. */
static inline char *acct_ask(const char *request) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return NULL;
	}
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", ACCT_SOCK);
	struct timeval tv = { .tv_sec = 5 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return NULL;
	}
	char line[256];
	int n = snprintf(line, sizeof(line), "%s\n", request);
	if (write(fd, line, n) != n) {
		close(fd);
		return NULL;
	}
	size_t cap = 1 << 16, len = 0;
	char *buf = malloc(cap);
	for (;;) {
		if (len + 4096 > cap) {
			cap *= 2;
			char *more = realloc(buf, cap);
			if (!more) {
				break;
			}
			buf = more;
		}
		ssize_t got = read(fd, buf + len, cap - len - 1);
		if (got <= 0) {
			break;
		}
		len += got;
	}
	close(fd);
	buf[len] = 0;
	if (!len) {
		free(buf);
		return NULL;
	}
	return buf;
}
