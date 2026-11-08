// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "shots.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "jobs.h"
#include "util.h"

static struct {
	char *url;
	cairo_surface_t *surface;	/* NULL while loading, or when it failed */
	enum shot_state state;
	unsigned used;			/* when last drawn: the oldest goes first */
} slot[SHOT_KEEP * 3];
#define N_SLOTS	((int)(sizeof(slot) / sizeof(slot[0])))
static unsigned clock_;
static void (*on_ready)(void);
static bool metered, allowed;

void shots_init(void (*ready)(void)) {
	on_ready = ready;
}

void shot_allow(bool allow) {
	allowed = allow;
}

void shot_set_metered(bool m) {
	metered = m;
}

void shot_file(const char *url, char *out, size_t size) {
	uint64_t h = 1469598103934665603ull;	/* FNV-1a */
	for (const unsigned char *c = (const unsigned char *)url; *c; c++) {
		h = (h ^ *c) * 1099511628211ull;
	}
	char name[64];
	snprintf(name, sizeof(name), "shots/%016llx-%d.png", (unsigned long long)h, SHOT_MAX_PX);
	cache_path(out, size, name);
}

struct load {
	char *url;
	cairo_surface_t *surface;
	bool may_fetch;
};

/* `s` made no wider than SHOT_MAX_PX */
static cairo_surface_t *shrink(cairo_surface_t *s) {
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	if (w <= SHOT_MAX_PX) {
		return s;
	}
	double k = (double)SHOT_MAX_PX / w;
	int nw = SHOT_MAX_PX, nh = (int)(h * k + 0.5);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, nw, nh > 0 ? nh : 1);
	cairo_t *cr = cairo_create(out);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	return out;
}

static cairo_surface_t *read_png(const char *path) {
	cairo_surface_t *s = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

static void load_work(void *arg) {
	struct load *l = arg;
	char path[4096];
	shot_file(l->url, path, sizeof(path));
	char dir[4096];
	cache_path(dir, sizeof(dir), "shots");
	mkdir_p(dir, 0700);
	l->surface = read_png(path);
	if (l->surface) {
		utime(path, NULL);	/* seen now: kept the longest */
		return;
	}
	if (!l->may_fetch) {
		return;
	}
	char part[4200];
	snprintf(part, sizeof(part), "%s.download", path);
	char *argv[] = { "curl", "-fsSL", "--max-time", "60", "--max-filesize", "20000000", "-o", part, l->url, NULL };
	int st;
	free(run_capture(argv, &st));
	cairo_surface_t *s = st == 0 ? read_png(part) : NULL;
	unlink(part);
	if (!s) {
		return;	/* not reachable, or not a PNG (cairo reads no other kind) */
	}
	s = shrink(s);
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (cairo_surface_write_to_png(s, tmp) == CAIRO_STATUS_SUCCESS) {
		rename(tmp, path);
	} else {
		unlink(tmp);
	}
	l->surface = s;
}

static int find(const char *url) {
	for (int i = 0; i < N_SLOTS; i++) {
		if (slot[i].url && !strcmp(slot[i].url, url)) {
			return i;
		}
	}
	return -1;
}

/* keep at most SHOT_KEEP decoded: the least recently drawn are let go */
static void trim_memory(void) {
	for (;;) {
		int n = 0, oldest = -1;
		for (int i = 0; i < N_SLOTS; i++) {
			if (slot[i].surface) {
				n++;
				if (oldest < 0 || slot[i].used < slot[oldest].used) {
					oldest = i;
				}
			}
		}
		if (n <= SHOT_KEEP) {
			return;
		}
		cairo_surface_destroy(slot[oldest].surface);
		free(slot[oldest].url);
		memset(&slot[oldest], 0, sizeof(slot[oldest]));
	}
}

static void load_done(void *arg) {
	struct load *l = arg;
	int i = find(l->url);
	if (i >= 0) {
		slot[i].surface = l->surface;
		slot[i].state = l->surface ? SHOT_READY : (l->may_fetch ? SHOT_FAILED : SHOT_HELD);
		slot[i].used = ++clock_;
		l->surface = NULL;
	}
	if (l->surface) {
		cairo_surface_destroy(l->surface);
	}
	free(l->url);
	free(l);
	trim_memory();
	if (on_ready) {
		on_ready();
	}
}

cairo_surface_t *shot_get(const char *url, enum shot_state *state) {
	int i = find(url);
	bool may_fetch = !metered || allowed;
	if (i >= 0 && !(slot[i].state == SHOT_HELD && may_fetch) && slot[i].state != SHOT_FAILED) {
		slot[i].used = ++clock_;
		*state = slot[i].state;
		return slot[i].surface;
	}
	if (i >= 0 && slot[i].state == SHOT_FAILED && slot[i].used + 40 > clock_) {
		*state = SHOT_FAILED;	/* (tried again only after a while of drawing others) */
		return NULL;
	}
	if (i < 0) {
		/* a free slot, or the least recently used that is not loading */
		for (int k = 0; k < N_SLOTS && i < 0; k++) {
			if (!slot[k].url) {
				i = k;
			}
		}
		for (int k = 0; k < N_SLOTS && i < 0; k++) {
			if (slot[k].state != SHOT_LOADING && (i < 0 || slot[k].used < slot[i].used)) {
				i = k;
			}
		}
		if (i < 0) {
			*state = SHOT_LOADING;	/* everything is loading: later */
			return NULL;
		}
		if (slot[i].surface) {
			cairo_surface_destroy(slot[i].surface);
		}
		free(slot[i].url);
		memset(&slot[i], 0, sizeof(slot[i]));
		slot[i].url = xstrdup(url);
	}
	slot[i].state = SHOT_LOADING;
	slot[i].used = ++clock_;
	struct load *l = calloc(1, sizeof(*l));
	l->url = xstrdup(url);
	l->may_fetch = may_fetch;
	job_run(load_work, load_done, l);
	*state = SHOT_LOADING;
	return NULL;
}

void shot_fetch_now(const char *url) {
	struct load *l = calloc(1, sizeof(*l));
	l->url = xstrdup(url);
	l->may_fetch = true;
	load_work(l);
	int i = find(url);
	for (int k = 0; i < 0 && k < N_SLOTS; k++) {
		if (!slot[k].url) {
			i = k;
			slot[k].url = xstrdup(url);
		}
	}
	if (i >= 0) {
		slot[i].state = SHOT_LOADING;
		load_done(l);	/* (frees l) */
	} else {
		if (l->surface) {
			cairo_surface_destroy(l->surface);
		}
		free(l->url);
		free(l);
	}
}

struct entry {
	char *path;
	long mtime;
	long size;
};

static int by_age(const void *a, const void *b) {
	const struct entry *x = a, *y = b;
	return (x->mtime > y->mtime) - (x->mtime < y->mtime);
}

void shots_prune(void) {
	char dir[4096];
	cache_path(dir, sizeof(dir), "shots");
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct entry *e = NULL;
	int n = 0, cap = 0;
	long total = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[4400];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		/* a download left by a run that was stopped */
		if (strstr(de->d_name, ".download") || strstr(de->d_name, ".new")) {
			if (time(NULL) - st.st_mtime > 3600) {
				unlink(path);
			}
			continue;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			e = realloc(e, cap * sizeof(*e));
		}
		e[n++] = (struct entry){ xstrdup(path), (long)st.st_mtime, (long)st.st_size };
		total += st.st_size;
	}
	closedir(d);
	qsort(e, n, sizeof(*e), by_age);
	for (int i = 0; i < n; i++) {
		if (total > (long)SHOT_CACHE_MB * 1024 * 1024) {
			unlink(e[i].path);
			total -= e[i].size;
		}
		free(e[i].path);
	}
	free(e);
}
