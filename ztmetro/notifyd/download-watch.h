/* App downloads as progress notifications, for apps that download without
 * telling anyone. Each app is a line in dl_sources[]: where it keeps a file per
 * download, where the bytes go, and how to read its file. Included by ztnotifyd
 * and by tests/download-watch.c; no D-Bus here. Needs _GNU_SOURCE.
 *
 * It costs nothing while nothing downloads: inotify says when something changes,
 * there is no timer, and no file is read. Only while a download is shown does it
 * look every DL_POLL_MS. It opens only what is under the directories its table
 * names, doesn't follow symlinks, and writes nothing.
 *
 * Grayjay is why this measures bytes rather than reading a progress field. Going
 * by Grayjay.Desktop's source (StateDownloads.DownloadVideo, master branch, read
 * 2026-11-07), downloads_ongoing/<uuid> (no extension) is written when a download
 * is asked for, with State 0 and Progress 0, and once more when its bytes are all
 * in. In between, Progress lives in the app and goes out over its own WebSocket.
 * So the file says what is wanted and the disk says how far it has got:
 *
 *  - most sources are written to downloads/<video id> [...].ext as they come;
 *  - HLS goes to temp_files/segment-* and is only joined at the end. Those names
 *    say nothing of whose they are, and a killed app leaves them behind (one
 *    phone had 341 of them, 159 MB), so only ones newer than the download's start
 *    are counted, for the oldest download that could be HLS. Going by the source
 *    it downloads one at a time, oldest first.
 *
 * The whole size isn't on disk either, so it is estimated from the bitrate and
 * the duration: the percent is an estimate, and says so ("About 42%"). A real
 * Progress in the file, should a later version keep one, is taken over it.
 *
 * Its file also stays behind when the app quits, so a file alone doesn't mean a
 * download. One is shown while its file is new or its bytes are moving. */
#ifndef ZTOS_DOWNLOAD_WATCH_H
#define ZTOS_DOWNLOAD_WATCH_H

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DL_POLL_MS	3000	/* how often a download being shown is looked at */
#define DL_EVENT_GAP_MS	1000	/* and however busy the directories, no more than this */
#define DL_RETRY_MS	60000	/* a directory that isn't there yet */
#define DL_STALE_S	45	/* nothing moved for this long: it isn't downloading */
#define DL_MAX		8	/* downloads followed at a time */
#define DL_FILE_MAX	(1024 * 1024)	/* a bigger file isn't one of these */
#define DL_SPOOL_MAX	20000	/* pieces looked at, at most */
#define DL_BUSY		(-2)	/* NOTE_BUSY in note-logic.h: no number to show */

/* ---- reading JSON -----------------------------------------------------------
 * These files hold whole descriptions and lists of sources, with the same keys at
 * several depths ("Name" is the video's, its author's and each source's), so
 * values are found by walking one object's members, not by searching. Files are
 * written in place with no rename: one read mid-write ends early, and is refused
 * by dl_json_complete rather than half believed. */

static inline const char *dl_json_ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* Past the value at `p`; NULL if it doesn't end. Counts depth instead of
 * recursing, so a deep file can't use up the stack. */
static inline const char *dl_json_skip(const char *p) {
	long depth = 0;
	p = dl_json_ws(p);
	do {
		if (*p == '"') {
			for (p++; *p != '"'; p++) {
				if (!*p || (*p == '\\' && !*++p))
					return NULL;
			}
			p++;
		} else if (*p == '{' || *p == '[') {
			depth++;
			p++;
		} else if (*p == '}' || *p == ']') {
			if (--depth < 0)
				return NULL;
			p++;
		} else if (depth && (*p == ',' || *p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
			p++;
		} else if (*p) {
			const char *from = p;
			while (*p && !strchr(",:]}\" \t\r\n{[", *p))
				p++;
			if (p == from)
				return NULL;
		} else {
			return NULL;
		}
	} while (depth);
	return p;
}

/* All of the text is one whole object. */
static inline bool dl_json_complete(const char *json) {
	json = dl_json_ws(json);
	if (*json != '{')
		return false;
	const char *end = dl_json_skip(json);
	return end && !*dl_json_ws(end);
}

/* The value of `key` among the members of the object at `obj`, or NULL. */
static inline const char *dl_json_get(const char *obj, const char *key) {
	if (!obj)
		return NULL;
	const char *p = dl_json_ws(obj);
	if (*p != '{')
		return NULL;
	size_t len = strlen(key);
	p = dl_json_ws(p + 1);
	while (*p == '"') {
		const char *name = p + 1, *after = dl_json_skip(p);
		if (!after)
			return NULL;
		bool same = (size_t)(after - 1 - name) == len && !strncmp(name, key, len);
		p = dl_json_ws(after);
		if (*p != ':')
			return NULL;
		p = dl_json_ws(p + 1);
		if (same)
			return p;
		p = dl_json_skip(p);
		if (!p)
			return NULL;
		p = dl_json_ws(p);
		if (*p != ',')
			return NULL;
		p = dl_json_ws(p + 1);
	}
	return NULL;
}

static inline bool dl_json_number(const char *value, double *out) {
	if (!value || !(*value == '-' || isdigit((unsigned char)*value)))
		return false;
	char *end;
	double d = strtod(value, &end);
	if (end == value || d != d)
		return false;
	*out = d;
	return true;
}

/* A string value, unescaped, as UTF-8. false for anything but a string. */
static inline bool dl_json_string(const char *value, char *out, size_t size) {
	size_t o = 0;
	out[0] = 0;
	if (!value || *value != '"')
		return false;
	for (const char *p = value + 1; *p && *p != '"' && o + 4 < size; p++) {
		if (*p != '\\') {
			out[o++] = *p;
			continue;
		}
		p++;
		if (!*p) {
			break;
		} else if (*p == 'u') {
			unsigned c = 0;
			if (strnlen(p + 1, 4) < 4 || sscanf(p + 1, "%4x", &c) != 1)
				break;
			p += 4;
			if (c >= 0xd800 && c < 0xe000) {
				out[o++] = '?';		/* half of a pair: not worth the code */
			} else if (c < 0x80) {
				out[o++] = c ? (char)c : ' ';
			} else if (c < 0x800) {
				out[o++] = (char)(0xc0 | c >> 6);
				out[o++] = (char)(0x80 | (c & 0x3f));
			} else {
				out[o++] = (char)(0xe0 | c >> 12);
				out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
				out[o++] = (char)(0x80 | (c & 0x3f));
			}
		} else {
			out[o++] = *p == 'n' || *p == 't' || *p == 'r' ? ' ' : *p;
		}
	}
	out[o] = 0;
	return true;
}

/* ---- what one app's file says ----------------------------------------------- */

struct dl_info {
	char name[120];		/* what to call it */
	char id[200];		/* what its data files are named after */
	double progress;	/* 0..1 when the file has it, else 0 */
	long long estimate;	/* whole size in bytes, 0: unknown */
	bool spooled;		/* its bytes may be in the spool directory */
	bool finishing;		/* all in: being checked or moved */
	bool failed;		/* the app has given up on it */
};

/* VideoDownload.DownloadState in Grayjay's source. 0 isn't in it: not started. */
enum { GRAYJAY_VALIDATING = 4, GRAYJAY_FINALIZING = 5, GRAYJAY_COMPLETED = 6, GRAYJAY_ERROR = 7 };

static inline bool grayjay_parse(const char *json, struct dl_info *out) {
	memset(out, 0, sizeof(*out));
	if (!dl_json_complete(json))
		return false;
	double state;
	if (!dl_json_number(dl_json_get(json, "State"), &state))
		return false;	/* not the file this is written for */
	const char *video = dl_json_get(json, "Video"), *details = dl_json_get(json, "VideoDetails");
	if (!dl_json_string(dl_json_get(video, "Name"), out->name, sizeof(out->name)) || !out->name[0])
		dl_json_string(dl_json_get(details, "Name"), out->name, sizeof(out->name));
	const char *from = dl_json_get(video, "ID") ? video : details;
	dl_json_string(dl_json_get(dl_json_get(from, "ID"), "Value"), out->id, sizeof(out->id));
	double progress = 0, duration = 0, bitrate;
	if (dl_json_number(dl_json_get(json, "Progress"), &progress) && progress > 0)
		out->progress = progress > 1 ? 1 : progress;
	dl_json_number(dl_json_get(from, "Duration"), &duration);
	/* no source chosen yet (a playlist's download): anything could come of it,
	 * and there is no bitrate to go on */
	const char *sources[2] = { dl_json_get(json, "VideoSource"), dl_json_get(json, "AudioSource") };
	double rate = 0;
	bool any = false, unknown = false;
	for (int i = 0; i < 2; i++) {
		char container[64];
		if (!sources[i] || *sources[i] != '{')
			continue;
		any = true;
		if (dl_json_string(dl_json_get(sources[i], "Container"), container, sizeof(container)) &&
				!strcmp(container, "application/vnd.apple.mpegurl"))
			out->spooled = true;
		if (dl_json_number(dl_json_get(sources[i], "Bitrate"), &bitrate) && bitrate > 0)
			rate += bitrate;
		else
			unknown = true;
	}
	if (!any)
		out->spooled = true;
	else if (!unknown && duration > 0)
		out->estimate = (long long)(rate * duration / 8);
	out->finishing = state == GRAYJAY_VALIDATING || state == GRAYJAY_FINALIZING ||
		state == GRAYJAY_COMPLETED;
	out->failed = state == GRAYJAY_ERROR;
	return true;
}

/* ---- the apps --------------------------------------------------------------- */

struct dl_source {
	const char *slug;	/* for the notification's key */
	const char *app, *who;	/* its name in the list; its desktop entry, for the glyph */
	const char *list_dir;	/* under $HOME: one file per download */
	const char *data_dir;	/* under $HOME: files named after the download's id */
	const char *spool_dir;	/* under $HOME, or NULL: pieces with no owner in their name */
	const char *spool_prefix;
	bool (*parse)(const char *json, struct dl_info *out);
};

static const struct dl_source dl_sources[] = {
	{
		.slug = "grayjay", .app = "Grayjay", .who = "app.grayjay.Grayjay",
		.list_dir = ".var/app/app.grayjay.Grayjay/data/Grayjay/downloads_ongoing",
		.data_dir = ".var/app/app.grayjay.Grayjay/data/Grayjay/downloads",
		.spool_dir = ".var/app/app.grayjay.Grayjay/data/Grayjay/temp_files",
		.spool_prefix = "segment-",
		.parse = grayjay_parse,
	},
};
#define DL_N_SOURCES (sizeof(dl_sources) / sizeof(dl_sources[0]))

/* ---- what to show for one ---------------------------------------------------- */

struct dl_view {
	int percent;		/* 0..100, or DL_BUSY */
	char text[64];
};

/* From what its file says and how many bytes have been seen. */
static inline void dl_view_of(const struct dl_info *info, long long bytes, struct dl_view *out) {
	char size[24];
	if (bytes >= 1000000000LL)
		snprintf(size, sizeof(size), "%.1f GB", (double)bytes / 1e9);
	else
		snprintf(size, sizeof(size), "%lld MB", bytes / 1000000);
	out->percent = DL_BUSY;
	if (info->finishing || info->progress >= 1.0) {
		snprintf(out->text, sizeof(out->text), "Finishing");
	} else if (info->progress > 0) {
		out->percent = (int)(info->progress * 100);
		snprintf(out->text, sizeof(out->text), "%d%%", out->percent);
	} else if (bytes <= 0) {
		snprintf(out->text, sizeof(out->text), "Starting");
	} else if (info->estimate > 0) {
		/* never 100: the estimate is often low, and 100 would say it is done */
		long long at = bytes / (info->estimate / 100 + 1);
		out->percent = at > 99 ? 99 : (int)at;
		snprintf(out->text, sizeof(out->text), "About %d%% (%s)", out->percent, size);
	} else {
		snprintf(out->text, sizeof(out->text), "Downloading, %s", size);
	}
}

/* Letters and digits only, in lower case: Grayjay cleans its file names by rules
 * of its own, so an id and a file name are compared after both are cleaned. */
static inline void dl_squash(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (; *in && o + 1 < size; in++)
		if (isalnum((unsigned char)*in))
			out[o++] = (char)tolower((unsigned char)*in);
	out[o] = 0;
}

/* ---- following them ---------------------------------------------------------- */

struct dl_entry {
	bool used, shown, seen, parsed;
	char file[128];
	char key[72];
	struct dl_info info;
	time_t file_mtime;
	long file_nsec;		/* (a rewrite can come within the same second) */
	long long file_size;
	time_t since;		/* when it started, as far as can be known; 0: it hasn't */
	char told[500];
};

struct dl_watch {
	const struct dl_source *src;
	const char *home;
	int fd;			/* inotify */
	int wd_list, wd_data, wd_spool;
	long scan_at;		/* a look is due at this time (ms); 0: none is */
	long last_scan;
	long retry_at;
	struct dl_entry e[DL_MAX];
	void (*tell)(const char *fmt, ...);
};

static inline bool dl_is_list_name(const char *name) {
	size_t n = strlen(name);
	return name[0] != '.' && n < sizeof(((struct dl_entry *)0)->file) &&
		!(n > 4 && !strcmp(name + n - 4, ".bak"));
}

/* A field of a line to the shell: nothing in it may end the field or the line. */
static inline void dl_field(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (; *in && o + 1 < size; in++)
		out[o++] = *in == '|' ? '/' : (unsigned char)*in < ' ' ? ' ' : *in;
	out[o] = 0;
}

/* Reads a download's file, if it has changed. A regular file of sane size only;
 * one that can't be read as a whole leaves what was known, for the next look. */
static inline void dl_read_entry(struct dl_watch *w, int dir, struct dl_entry *e, const struct stat *st) {
	if ((e->parsed && e->file_mtime == st->st_mtime && e->file_nsec == st->st_mtim.tv_nsec &&
			e->file_size == st->st_size) || st->st_size <= 0 || st->st_size > DL_FILE_MAX)
		return;
	int fd = openat(dir, e->file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	char *buf = malloc((size_t)st->st_size + 1);
	ssize_t n = buf ? read(fd, buf, (size_t)st->st_size) : -1;
	close(fd);
	if (n > 0) {
		struct dl_info info;
		buf[n] = 0;
		if (w->src->parse(buf, &info)) {
			e->info = info;
			e->parsed = true;
			e->file_mtime = st->st_mtime;
			e->file_nsec = st->st_mtim.tv_nsec;
			e->file_size = st->st_size;
		}
	}
	free(buf);
}

/* What a file really holds on disk: a source fetched in ranges might have set its
 * length before filling it. */
static inline long long dl_disk(const struct stat *st) {
	long long blocks = (long long)st->st_blocks * 512;
	return blocks < st->st_size ? blocks : (long long)st->st_size;
}

static inline int dl_open_under(const struct dl_watch *w, const char *rel) {
	char path[PATH_MAX];
	if (!rel || snprintf(path, sizeof(path), "%s/%s", w->home, rel) >= (int)sizeof(path))
		return -1;
	return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

/* Sets inotify's marks. Writes and the spool are only listened to while a
 * download's file is here and nothing is being shown: that is when the first byte
 * matters (an app back from being closed, taking up what it left). While one is
 * shown there is the timer, and each write waking this would be the very polling
 * it is here to avoid. */
static inline void dl_watch_update(struct dl_watch *w, bool want_writes) {
	const uint32_t base = IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |
		IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
	char path[PATH_MAX];
	if (w->fd < 0)
		return;
	const struct { int *wd; const char *rel; uint32_t mask; } dirs[3] = {
		{ &w->wd_list, w->src->list_dir, base },
		{ &w->wd_data, w->src->data_dir, want_writes ? base | IN_MODIFY : base },
		{ &w->wd_spool, w->src->spool_dir, want_writes ? base : 0 },
	};
	for (int i = 0; i < 3; i++) {
		if (!dirs[i].rel)
			continue;
		if (!dirs[i].mask) {
			if (*dirs[i].wd >= 0)
				inotify_rm_watch(w->fd, *dirs[i].wd);
			*dirs[i].wd = -1;
		} else if (snprintf(path, sizeof(path), "%s/%s", w->home, dirs[i].rel) < (int)sizeof(path)) {
			*dirs[i].wd = inotify_add_watch(w->fd, path, dirs[i].mask);
		}
	}
}

static inline void dl_close_entry(struct dl_watch *w, struct dl_entry *e) {
	if (e->shown)
		w->tell("noteclose %s", e->key);
	e->shown = false;
	e->told[0] = 0;
	e->since = 0;
}

/* Finds or makes the entry for a file in the list directory. */
static inline struct dl_entry *dl_entry_for(struct dl_watch *w, const char *file) {
	struct dl_entry *spare = NULL;
	for (int i = 0; i < DL_MAX; i++) {
		if (w->e[i].used && !strcmp(w->e[i].file, file))
			return &w->e[i];
		if (!w->e[i].used && !spare)
			spare = &w->e[i];
	}
	if (spare) {
		char clean[128];
		size_t o = 0;
		memset(spare, 0, sizeof(*spare));
		spare->used = true;
		snprintf(spare->file, sizeof(spare->file), "%s", file);
		for (const char *c = file; *c && o + 1 < sizeof(clean); c++)
			clean[o++] = isalnum((unsigned char)*c) || *c == '-' ? *c : '_';
		clean[o] = 0;
		snprintf(spare->key, sizeof(spare->key), "dl:%s:%.50s", w->src->slug, clean);
	}
	return spare;
}

/* One look at whole directories, so it doesn't matter what inotify ran together.
 * `now` is on the clock of the files' times. Returns whether any is being shown. */
static inline bool dl_scan(struct dl_watch *w, time_t now) {
	bool fresh[DL_MAX] = { false }, data_new[DL_MAX] = { false };
	long long bytes[DL_MAX] = { 0 };
	struct dirent *de;
	int any = 0, list = dl_open_under(w, w->src->list_dir);
	DIR *d = list >= 0 ? fdopendir(dup(list)) : NULL;
	for (int i = 0; i < DL_MAX; i++)
		w->e[i].seen = false;
	while (d && (de = readdir(d))) {
		struct stat st;
		struct dl_entry *e;
		if (!dl_is_list_name(de->d_name) ||
				fstatat(list, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISREG(st.st_mode) ||
				!(e = dl_entry_for(w, de->d_name)))
			continue;
		e->seen = true;
		any++;
		dl_read_entry(w, list, e, &st);
		/* a file written just now is a download just asked for */
		if (!e->since && e->parsed && now - st.st_mtime < DL_STALE_S)
			e->since = st.st_mtime;
	}
	if (d)
		closedir(d);
	if (list >= 0)
		close(list);

	/* gone from the list: finished, or cancelled */
	for (int i = 0; i < DL_MAX; i++) {
		if (w->e[i].used && !w->e[i].seen) {
			dl_close_entry(w, &w->e[i]);
			w->e[i].used = false;
		}
		fresh[i] = w->e[i].used && w->e[i].parsed && now - w->e[i].file_mtime < DL_STALE_S;
	}

	if (any) {
		/* the data files, by the id at the front of their names */
		char ids[DL_MAX][200];
		int data = dl_open_under(w, w->src->data_dir);
		for (int i = 0; i < DL_MAX; i++)
			dl_squash(ids[i], sizeof(ids[i]), w->e[i].used && w->e[i].parsed ? w->e[i].info.id : "");
		d = data >= 0 ? fdopendir(dup(data)) : NULL;
		while (d && (de = readdir(d))) {
			struct stat st;
			char name[300];
			if (de->d_name[0] == '.' || fstatat(data, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
					!S_ISREG(st.st_mode))
				continue;
			dl_squash(name, sizeof(name), de->d_name);
			for (int i = 0; i < DL_MAX; i++) {
				size_t n = strlen(ids[i]);
				if (n < 4 || strncmp(name, ids[i], n))
					continue;
				bytes[i] += dl_disk(&st);
				if (now - st.st_mtime < DL_STALE_S)
					data_new[i] = true;
			}
		}
		if (d)
			closedir(d);
		if (data >= 0)
			close(data);
		for (int i = 0; i < DL_MAX; i++) {
			if (data_new[i]) {
				fresh[i] = true;
				if (!w->e[i].since)
					w->e[i].since = now;
			} else {
				bytes[i] = 0;	/* an earlier copy of it is not this download */
			}
		}

		/* the pieces: the oldest download that could be theirs */
		int owner = -1;
		for (int i = 0; i < DL_MAX; i++) {
			if (w->e[i].used && w->e[i].parsed && w->e[i].info.spooled &&
					(owner < 0 || w->e[i].file_mtime < w->e[owner].file_mtime))
				owner = i;
		}
		int spool = owner >= 0 ? dl_open_under(w, w->src->spool_dir) : -1;
		d = spool >= 0 ? fdopendir(dup(spool)) : NULL;
		if (d) {
			struct dl_entry *e = &w->e[owner];
			/* not known to have started, and pieces moving: an app that came
			 * back and took it up again */
			time_t from = e->since ? e->since : now - DL_STALE_S;
			size_t prefix = strlen(w->src->spool_prefix);
			long long total = 0;
			bool spool_new = false;
			int looked = 0;
			while (looked < DL_SPOOL_MAX && (de = readdir(d))) {
				struct stat st;
				if (strncmp(de->d_name, w->src->spool_prefix, prefix))
					continue;
				looked++;
				if (fstatat(spool, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
						!S_ISREG(st.st_mode) || st.st_mtime < from)
					continue;
				total += dl_disk(&st);
				if (now - st.st_mtime < DL_STALE_S)
					spool_new = true;
			}
			closedir(d);
			if (spool_new) {
				e->since = from;
				fresh[owner] = true;
				/* joining them writes the data file while they are still here:
				 * the same bytes twice, so the larger, not both */
				if (total > bytes[owner])
					bytes[owner] = total;
			}
		}
		if (spool >= 0)
			close(spool);
	}

	bool any_shown = false, any_waiting = false;
	for (int i = 0; i < DL_MAX; i++) {
		struct dl_entry *e = &w->e[i];
		if (!e->used)
			continue;
		if (!fresh[i] || e->info.failed) {
			dl_close_entry(w, e);
			any_waiting = true;
			continue;
		}
		struct dl_view v;
		char line[500], title[120];
		dl_view_of(&e->info, bytes[i], &v);
		dl_field(title, sizeof(title), e->info.name[0] ? e->info.name : "Download");
		snprintf(line, sizeof(line), "notekey %s|q|%d|%s|%s|%s|%s", e->key, v.percent, w->src->who,
			w->src->app, title, v.text);
		if (strcmp(line, e->told)) {
			snprintf(e->told, sizeof(e->told), "%s", line);
			w->tell("%s", line);
		}
		e->shown = true;
		any_shown = true;
	}
	dl_watch_update(w, any_waiting && !any_shown);
	return any_shown;
}

/* ---- for the daemon's loop --------------------------------------------------- */

static inline void dl_start(struct dl_watch *w, const struct dl_source *src, const char *home,
		void (*tell)(const char *fmt, ...)) {
	memset(w, 0, sizeof(*w));
	w->src = src;
	w->home = home;
	w->tell = tell;
	w->wd_list = w->wd_data = w->wd_spool = -1;
	w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	w->scan_at = 1;		/* a first look, for what is already going on */
}

/* Takes what inotify has, and asks for a look (not at once: a busy directory has
 * many things to say). */
static inline void dl_readable(struct dl_watch *w, long now_ms) {
	char buf[4096];
	long at = w->last_scan + DL_EVENT_GAP_MS;
	while (read(w->fd, buf, sizeof(buf)) > 0) {
	}
	if (at < now_ms)
		at = now_ms;
	if (!w->scan_at || w->scan_at > at)
		w->scan_at = at;
}

/* Runs a look if one is due; returns ms until the next one, or -1 for none. An app
 * not installed yet, or never run, has no directory: that is checked for only when
 * the loop is awake anyway, and no more than each DL_RETRY_MS. */
static inline long dl_tick(struct dl_watch *w, long now_ms) {
	if (w->wd_list < 0 && w->retry_at && now_ms >= w->retry_at)
		w->scan_at = now_ms;
	if (w->scan_at && now_ms >= w->scan_at) {
		w->last_scan = now_ms;
		w->scan_at = dl_scan(w, time(NULL)) ? now_ms + DL_POLL_MS : 0;
		w->retry_at = w->wd_list < 0 ? now_ms + DL_RETRY_MS : 0;
	}
	if (!w->scan_at)
		return -1;
	return w->scan_at > now_ms ? w->scan_at - now_ms : 0;
}

/* The shell started again and knows nothing of what it was told. */
static inline void dl_resend(struct dl_watch *w, long now_ms) {
	for (int i = 0; i < DL_MAX; i++)
		w->e[i].told[0] = 0;
	w->scan_at = now_ms;
}

#endif
