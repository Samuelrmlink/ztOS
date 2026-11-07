// SPDX-License-Identifier: GPL-3.0-or-later
/* $XDG_RUNTIME_DIR/ztos-pods, which ztos-podsd replaces whole on any change:
 *   known=MAC     (repeated) a pair that has connected here: Settings' button
 *   link=0|1      an AAP link is open
 *   left= right= case=   percent; absent: not known
 *   charging=0|1
 *   charge_left=0|1 charge_right=0|1   that pod is charging
 *   single=left|right   the one pod out of the case, the other in it; absent: both, or not known
 * The shell and Settings read it; neither links a JSON parser. */
#ifndef PODS_BAR_STATE_H
#define PODS_BAR_STATE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PODS_BAR_KNOWN 6

struct pods_bar {
	bool link, charging;
	bool charge_left, charge_right;
	int single;			/* -1 both or unknown, 0 left, 1 right */
	int left, right, kase;		/* percent, -1 unknown */
	int n_known;
	char known[PODS_BAR_KNOWN][18];
};

static inline void pods_bar_parse(FILE *f, struct pods_bar *b) {
	char line[64];
	memset(b, 0, sizeof(*b));
	b->left = b->right = b->kase = -1;
	b->single = -1;
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "link=", 5)) b->link = line[5] == '1';
		else if (!strncmp(line, "left=", 5)) b->left = atoi(line + 5);
		else if (!strncmp(line, "right=", 6)) b->right = atoi(line + 6);
		else if (!strncmp(line, "case=", 5)) b->kase = atoi(line + 5);
		else if (!strncmp(line, "charging=", 9)) b->charging = line[9] == '1';
		else if (!strncmp(line, "charge_left=", 12)) b->charge_left = line[12] == '1';
		else if (!strncmp(line, "charge_right=", 13)) b->charge_right = line[13] == '1';
		else if (!strcmp(line, "single=left")) b->single = 0;
		else if (!strcmp(line, "single=right")) b->single = 1;
		else if (!strncmp(line, "known=", 6) && strlen(line + 6) == 17 && b->n_known < PODS_BAR_KNOWN) {
			snprintf(b->known[b->n_known++], 18, "%s", line + 6);
		}
	}
}

/* Reads the file; true if what the status bar shows of it changed. */
static inline bool pods_bar_read(struct pods_bar *b) {
	char path[256];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-pods", runtime ? runtime : "/tmp");
	FILE *f = fopen(path, "r");
	struct pods_bar was = *b;
	pods_bar_parse(f, b);
	if (f) fclose(f);
	return was.single != b->single || was.charge_left != b->charge_left || was.charge_right != b->charge_right || was.link != b->link || was.charging != b->charging || was.left != b->left || was.right != b->right;
}

static inline bool pods_bar_knows(const struct pods_bar *b, const char *mac) {
	for (int i = 0; i < b->n_known; i++) {
		if (!strcmp(b->known[i], mac)) return true;
	}
	return false;
}

#endif
