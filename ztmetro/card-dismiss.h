// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A recents card thrown away (ROADMAP 8.74): what is ended, and how. Plain C,
 * so tests/card-dismiss.c checks it on the PC.
 *
 * - An Android (Waydroid) window, app_id "waydroid.PKG": the package is
 *   force-stopped (`ztos-android close PKG` -> sudo ztos-waydroid-recover
 *   close PKG), which ends its activities, its services and any playback.
 *   Never a signal: the window's pid is the shared Waydroid container.
 * - Any other window: its own process. It is asked to close (xdg close); if
 *   it is still there DISMISS_CLOSE_MS later it gets SIGTERM, and if it is
 *   still there DISMISS_TERM_MS after that, SIGKILL. A process is known by
 *   its pid and its start time (/proc/PID/stat field 22), so a pid that was
 *   reused in the meantime is never signalled.
 * - Nothing at all for the shell's own pid, pid 0 or 1.
 *
 * A package that Android shows again on its own shortly after a dismiss (a
 * stale task re-mapped behind Start) is kept hidden: dismiss_was_recent().
 */
#ifndef ZT_CARD_DISMISS_H
#define ZT_CARD_DISMISS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define DISMISS_CLOSE_MS	1500	/* xdg close, then SIGTERM after this */
#define DISMISS_TERM_MS		2000	/* SIGTERM, then SIGKILL after this */
#define DISMISS_REMAP_MS	15000	/* a dismissed package's own window back this soon: hidden */
#define DISMISS_PKG_MAX		128
#define DISMISS_PROCS		8
#define DISMISS_RECENT		4

/* The switcher opened with nothing open (ROADMAP 8.75). */
#define SWITCH_EMPTY_TEXT	"No recent apps"

/* Whether a window gets a recents card: a real, drawn app window that has
 * not been hidden (a background app sent Home) or thrown away. Not the
 * Waydroid boot surface, nor the container's own "Waydroid" UI, nor a
 * window with no buffer yet (a task Android re-maps behind Start after a
 * force-stop would list as a blank card). */
static inline bool card_listable(bool mapped, bool background_hidden, bool dismissed,
		bool waydroid_boot, bool drawn, const char *app_id) {
	return mapped && !background_hidden && !dismissed && !waydroid_boot && drawn &&
		!(app_id && !strcmp(app_id, "Waydroid"));
}

enum dismiss_kind {
	DISMISS_NONE,		/* only the window: nothing to stop */
	DISMISS_ANDROID,	/* force-stop the package */
	DISMISS_PROCESS,	/* close, then end the client's process */
};

/* An Android package name as the root helper takes it: [a-zA-Z0-9._]+,
 * starting with a letter, not ending in a dot, no empty segment. */
static inline bool dismiss_package_ok(const char *pkg) {
	size_t n = pkg ? strlen(pkg) : 0;
	if (n == 0 || n >= DISMISS_PKG_MAX) return false;
	if (!((pkg[0] >= 'a' && pkg[0] <= 'z') || (pkg[0] >= 'A' && pkg[0] <= 'Z'))) return false;
	for (size_t i = 0; i < n; i++) {
		char c = pkg[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '.';
		if (!ok) return false;
		if (c == '.' && (i + 1 == n || pkg[i + 1] == '.')) return false;
	}
	return true;
}

/* What a dismiss of the window `app_id` (may be NULL) owned by `pid` does;
 * for DISMISS_ANDROID the package is copied to pkg. */
static inline enum dismiss_kind dismiss_kind_of(const char *app_id, pid_t pid, pid_t self,
		char *pkg, size_t pkg_size) {
	if (pkg_size) pkg[0] = 0;
	if (app_id && !strncmp(app_id, "waydroid.", 9)) {
		if (!dismiss_package_ok(app_id + 9) || strlen(app_id + 9) >= pkg_size) {
			return DISMISS_NONE;	/* never the container's pid */
		}
		snprintf(pkg, pkg_size, "%s", app_id + 9);
		return DISMISS_ANDROID;
	}
	if (pid <= 1 || pid == self) return DISMISS_NONE;
	return DISMISS_PROCESS;
}

/* /proc/PID/stat's start time (field 22) and state (field 3), from the text
 * after the last ')' (the name may hold anything). False if malformed. */
static inline bool dismiss_parse_stat(const char *stat, unsigned long long *start, char *state) {
	const char *p = stat ? strrchr(stat, ')') : NULL;
	if (!p || p[1] != ' ') return false;
	p += 2;
	if (state) *state = *p;
	/* p is at field 3; field 22 is 19 fields on */
	for (int field = 3; field < 22; field++) {
		p = strchr(p, ' ');
		if (!p) return false;
		p++;
	}
	char *end;
	unsigned long long v = strtoull(p, &end, 10);
	if (end == p) return false;
	*start = v;
	return true;
}

/* The start time of `pid` under proc_dir ("/proc"); false if it is gone or
 * a zombie. */
static inline bool dismiss_proc_start(const char *proc_dir, pid_t pid, unsigned long long *start) {
	char path[256], buf[1024];
	snprintf(path, sizeof(path), "%s/%d/stat", proc_dir, (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	char state = 0;
	if (!dismiss_parse_stat(buf, start, &state)) return false;
	return state != 'Z' && state != 'X';
}

/* One process being ended. */
struct dismiss_proc {
	pid_t pid;			/* 0: a free slot */
	unsigned long long start;	/* its start time, when it was dismissed */
	uint32_t closed_ms;		/* when it was asked to close */
	uint32_t term_ms;		/* when it got SIGTERM, or 0 */
};

enum dismiss_act { DA_WAIT, DA_TERM, DA_KILL, DA_DONE };

/* What to do now for `d`, given whether its process (the same one: pid and
 * start time) is still alive. DA_TERM records the SIGTERM; DA_DONE frees
 * the slot. */
static inline enum dismiss_act dismiss_step(struct dismiss_proc *d, uint32_t now, bool alive) {
	if (!d->pid) return DA_DONE;
	if (!alive) {
		d->pid = 0;
		return DA_DONE;
	}
	if (!d->term_ms) {
		if (now - d->closed_ms < DISMISS_CLOSE_MS) return DA_WAIT;
		d->term_ms = now ? now : 1;
		return DA_TERM;
	}
	if (now - d->term_ms < DISMISS_TERM_MS) return DA_WAIT;
	d->pid = 0;
	return DA_KILL;
}

/* Add `pid` (started at `start`) to `procs`; false if full or already there
 * (that one keeps its own clock). */
static inline bool dismiss_add(struct dismiss_proc *procs, int n, pid_t pid,
		unsigned long long start, uint32_t now) {
	struct dismiss_proc *free_slot = NULL;
	for (int i = 0; i < n; i++) {
		if (procs[i].pid == pid && procs[i].start == start) return false;
		if (!procs[i].pid && !free_slot) free_slot = &procs[i];
	}
	if (!free_slot) return false;
	*free_slot = (struct dismiss_proc){ .pid = pid, .start = start, .closed_ms = now };
	return true;
}

static inline bool dismiss_pending(const struct dismiss_proc *procs, int n) {
	for (int i = 0; i < n; i++) {
		if (procs[i].pid) return true;
	}
	return false;
}

/* Android packages lately dismissed, to keep a window of theirs that Android
 * shows again by itself from coming back as a card. */
struct dismiss_recent {
	char pkg[DISMISS_PKG_MAX];
	uint32_t at;
};

static inline void dismiss_remember(struct dismiss_recent *r, int n, const char *pkg, uint32_t now) {
	int slot = 0;
	for (int i = 0; i < n; i++) {
		if (!strcmp(r[i].pkg, pkg)) { slot = i; break; }
		if (r[i].at < r[slot].at) slot = i;	/* (the oldest goes) */
	}
	snprintf(r[slot].pkg, sizeof(r[slot].pkg), "%s", pkg);
	r[slot].at = now ? now : 1;
}

/* Was the Android window `app_id` ("waydroid.PKG") dismissed within
 * DISMISS_REMAP_MS of `now`? */
static inline bool dismiss_was_recent(const struct dismiss_recent *r, int n, const char *app_id, uint32_t now) {
	if (!app_id || strncmp(app_id, "waydroid.", 9)) return false;
	for (int i = 0; i < n; i++) {
		if (r[i].at && !strcmp(r[i].pkg, app_id + 9) && now - r[i].at < DISMISS_REMAP_MS) {
			return true;
		}
	}
	return false;
}

/* Forget a package: the owner opened it again on purpose. */
static inline void dismiss_forget(struct dismiss_recent *r, int n, const char *app_id) {
	if (!app_id || strncmp(app_id, "waydroid.", 9)) return;
	for (int i = 0; i < n; i++) {
		if (!strcmp(r[i].pkg, app_id + 9)) r[i].at = 0;
	}
}

#endif
