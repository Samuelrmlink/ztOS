// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "search.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct hit {
	struct pkg *p;
	int rank;
};

static int rank_of(const struct pkg *p, const char *term) {
	int r;
	/* (an app's name counts as its package's: Flathub's packages are IDs) */
	if (!strcasecmp(p->name, term) || (p->app_name && !strcasecmp(p->app_name, term))) {
		r = 0;
	} else if (!strncasecmp(p->name, term, strlen(term)) ||
			(p->app_name && !strncasecmp(p->app_name, term, strlen(term)))) {
		r = 1;
	} else if (contains_nocase(p->name, term) || contains_nocase(p->app_name, term)) {
		r = 2;
	} else if (contains_nocase(p->summary, term) || contains_nocase(p->desc, term) ||
			contains_nocase(p->keywords, term)) {
		r = 3;
	} else {
		return -1;
	}
	return r * 2 + (p->app_name ? 0 : 1);
}

static int by_rank(const void *a, const void *b) {
	const struct hit *x = a, *y = b;
	if (x->rank != y->rank) {
		return x->rank - y->rank;
	}
	return strcmp(x->p->name, y->p->name);
}

void search_repo(const struct repo_index *ri, const char *term, int max, struct vec *out) {
	search_list(&ri->sync, term, max, out);
}

void search_list(const struct vec *pkgs, const char *term, int max, struct vec *out) {
	if (!term || strlen(term) < 2) {
		return;
	}
	struct hit *hits = malloc(sizeof(*hits) * (pkgs->n ? pkgs->n : 1));
	int n = 0;
	for (int i = 0; i < pkgs->n; i++) {
		int r = rank_of(pkgs->v[i], term);
		if (r >= 0) {
			hits[n++] = (struct hit){ pkgs->v[i], r };
		}
	}
	qsort(hits, n, sizeof(*hits), by_rank);
	for (int i = 0; i < n && i < max; i++) {
		vec_push(out, pkg_copy(hits[i].p));
	}
	free(hits);
}
