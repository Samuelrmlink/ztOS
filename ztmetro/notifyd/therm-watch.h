// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztnotifyd: the battery temperature sensor's failed-load mark, as a standing
 * notification (power/thermistors/README.md).
 *
 * zrn-therm-load leaves /var/lib/zrn-therm/load-failed when a load of the
 * thermistor module never finished (the phone froze or reset during it), and
 * then does not load it again: the battery temperature is unknown and
 * zrn-charge keeps charging at 5 V. While that file is there the shell shows
 * "Battery temperature sensor off", a plain notification so that the lock
 * screen lists it too (ongoing ones are left off it); swiped away, it comes
 * back when the shell or ztnotifyd starts again while the mark is there.
 * Tapping it opens Settings > Battery, where
 * "Try again" clears the mark and loads the module. When the file goes, the
 * notification goes.
 *
 * inotify on /var/lib/zrn-therm (and on /var/lib until that exists), so it
 * costs nothing while nothing changes: no polling.
 */
#ifndef ZT_THERM_WATCH_H
#define ZT_THERM_WATCH_H
#include <stdbool.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#ifndef TW_DIR
#define TW_DIR		"/var/lib/zrn-therm"
#endif
#ifndef TW_PARENT
#define TW_PARENT	"/var/lib"
#endif
#ifndef TW_FILE
#define TW_FILE		TW_DIR "/load-failed"
#endif
#define TW_KEY		"zrn-therm"	/* the notification's name (ztmetro.c opens Settings > Battery for it) */

struct therm_watch {
	int fd;			/* inotify, or -1 */
	int dir_wd;		/* on TW_DIR, or -1 */
	int shown;		/* -1 not told yet, 0 closed, 1 shown */
	void (*tell)(const char *fmt, ...);
};

static inline void tw_mark(struct therm_watch *w)
{
	if (w->fd < 0 || w->dir_wd >= 0)
		return;
	w->dir_wd = inotify_add_watch(w->fd, TW_DIR, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
				      IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
}

/* Says what the file says now. `again`: the shell restarted, so say it once more, quietly. */
static inline void tw_look(struct therm_watch *w, bool again)
{
	int now = access(TW_FILE, F_OK) == 0;

	if (now == w->shown && !again)
		return;
	if (now)
		w->tell("notekey " TW_KEY "|%s|-1||System|Battery temperature sensor off|"
			"Charging is limited to 5 V: the sensor's driver did not load at the last start. "
			"Tap for Settings > Battery > Try again.", w->shown == 1 || again ? "q" : "-");
	else if (w->shown == 1 || again)
		w->tell("noteclose " TW_KEY);
	w->shown = now;
}

static inline void tw_init(struct therm_watch *w, void (*tell)(const char *fmt, ...))
{
	w->tell = tell;
	w->shown = -1;
	w->dir_wd = -1;
	w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (w->fd >= 0)
		inotify_add_watch(w->fd, TW_PARENT, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
	tw_mark(w);
	tw_look(w, false);
}

/* inotify has something: drain it, look again. */
static inline void tw_readable(struct therm_watch *w)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool dir_gone = false;
	ssize_t n;

	while ((n = read(w->fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n; ) {
			struct inotify_event *e = (struct inotify_event *)p;

			if (e->wd == w->dir_wd && (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)))
				dir_gone = true;
			p += sizeof(*e) + e->len;
		}
	}
	if (dir_gone)
		w->dir_wd = -1;
	tw_mark(w);
	tw_look(w, false);
}
#endif
