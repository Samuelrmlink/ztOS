// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "flathub.h"

#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xml.h"

/* ---- reading the catalogue -------------------------------------------------------- */

/* The catalogue is 45 MB of XML, mostly translations. It is mapped, not read:
 * the kernel keeps it in the page cache and nothing of it is on the heap. */
struct text {
	char *s;
	size_t n;
	bool mapped;
};

static bool text_open(struct text *t, const char *path) {
	memset(t, 0, sizeof(*t));
	size_t len = strlen(path);
	if (len > 3 && !strcmp(path + len - 3, ".gz")) {
		t->s = gunzip_file(path);
		t->n = t->s ? strlen(t->s) : 0;
		return t->s != NULL;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	long page = sysconf(_SC_PAGESIZE);
	/* mapped only when the map ends in zeros past the file (the parser wants
	 * a NUL at the end); a file a whole number of pages long is read instead */
	if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size % page) {
		void *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (m != MAP_FAILED) {
			close(fd);
			t->s = m;
			t->n = st.st_size;
			t->mapped = true;
			return true;
		}
	}
	close(fd);
	t->s = read_all(path, &t->n);
	return t->s != NULL;
}

static void text_close(struct text *t) {
	if (t->mapped) {
		munmap(t->s, t->n);
	} else {
		free(t->s);
	}
	memset(t, 0, sizeof(*t));
}

static bool untranslated(const char *tag, const char *tag_end) {
	char lang[16];
	return !xml_attr(tag + 1, tag_end, "xml:lang", lang, sizeof(lang));
}

/* Where in a component its elements of other things are: a developer's
 * <name>, a release's <description> are not the app's own. Found in one pass
 * over the component's tags, once per component (it is 45 MB of XML: looking
 * again for every child made reading it take 15 s on the phone). */
#define MAX_SKIP 24
struct skips {
	const char *s[MAX_SKIP], *e[MAX_SKIP];
	int n;
};

static const char *const NOT_OURS[] = { "developer", "releases", "screenshots", "provides", "requires",
	"recommends", "supports", "artifacts", NULL };

static void find_skips(const char *cs, const char *ce, const char *const *names, struct skips *sk) {
	sk->n = 0;
	for (const char *p = cs; p && p < ce && sk->n < MAX_SKIP; ) {
		p = memchr(p, '<', ce - p);
		if (!p) {
			break;
		}
		p++;
		for (int k = 0; names[k]; k++) {
			size_t n = strlen(names[k]);
			if (!strncmp(p, names[k], n) && (p[n] == '>' || p[n] == ' ' || p[n] == '/')) {
				const char *gt = memchr(p, '>', ce - p);
				if (!gt) {
					return;
				}
				const char *end = gt + 1;
				if (gt[-1] != '/') {
					char close[64];
					snprintf(close, sizeof(close), "</%s>", names[k]);
					const char *c = memmem(gt, ce - gt, close, strlen(close));
					end = c ? c + strlen(close) : ce;
				}
				sk->s[sk->n] = p - 1;
				sk->e[sk->n++] = end;
				p = end;
				break;
			}
		}
	}
}

/* The first untranslated `name` among the component's own children (not
 * inside one of `sk`'s ranges) */
static const char *child_in(const char *cs, const char *ce, const char *name, const struct skips *sk,
		const char **t, const char **tend) {
	const char *te, *q = cs;
	while ((q = xml_element(q, ce, name, &te, t, tend))) {
		bool inside = false;
		for (int k = 0; sk && k < sk->n && !inside; k++) {
			inside = q > sk->s[k] && q < sk->e[k];
		}
		if (!inside && untranslated(q, te)) {
			return q;
		}
		q = *tend;
	}
	return NULL;
}

static const char *child(const char *cs, const char *ce, const char *name, const char *const *skip,
		const char **t, const char **tend) {
	struct skips sk;
	if (skip) {
		find_skips(cs, ce, skip, &sk);
	}
	return child_in(cs, ce, name, skip ? &sk : NULL, t, tend);
}

/* the component being read, with its ranges found once */
static _Thread_local const struct skips *cur_skips;

static char *child_text(const char *cs, const char *ce, const char *name) {
	const char *t, *tend;
	struct skips sk;
	if (!cur_skips) {
		find_skips(cs, ce, NOT_OURS, &sk);
	}
	return child_in(cs, ce, name, cur_skips ? cur_skips : &sk, &t, &tend) ? xml_unescape(t, tend - t) : NULL;
}

/* <custom> (or the older <metadata>) <value key="K">V</value> */
static char *custom_value(const char *cs, const char *ce, const char *key) {
	const char *q = cs, *te, *t, *tend;
	while ((q = xml_element(q, ce, "value", &te, &t, &tend))) {
		char k[128];
		if (xml_attr(q + 1, te, "key", k, sizeof(k)) && !strcmp(k, key)) {
			return xml_unescape(t, tend - t);
		}
		q = tend;
	}
	return NULL;
}

static char *developer_of(const char *cs, const char *ce) {
	const char *te, *t, *tend, *q;
	if ((q = xml_element(cs, ce, "developer", &te, &t, &tend))) {
		const char *nt, *ntend;
		const char *n = child(t, tend, "name", NULL, &nt, &ntend);
		if (n) {
			return xml_unescape(nt, ntend - nt);
		}
	}
	return child_text(cs, ce, "developer_name");
}

/* made to fit a phone: it says so (Purism's form factor), or it fits in 360 px */
static bool mobile_of(const char *cs, const char *ce) {
	char *ff = custom_value(cs, ce, "Purism::form_factor");
	bool yes = ff && (!strcmp(ff, "mobile") || !strcmp(ff, "handset"));
	free(ff);
	const char *te, *t, *tend, *q;
	if (!yes && (q = xml_element(cs, ce, "requires", &te, &t, &tend))) {
		const char *dt, *dtend;
		if (xml_element(t, tend, "display_length", &te, &dt, &dtend)) {
			char *v = xml_unescape(dt, dtend - dt);
			int n = atoi(v);
			yes = n > 0 && n <= 400;
			free(v);
		}
	}
	return yes;
}

static char *bundle_runtime(const char *cs, const char *ce) {
	const char *te, *t, *tend, *q = xml_element(cs, ce, "bundle", &te, &t, &tend);
	char rt[256];
	return q && xml_attr(q + 1, te, "runtime", rt, sizeof(rt)) ? xml_unescape(rt, strlen(rt)) : NULL;
}

/* the newest release's version: releases are listed newest first */
static char *latest_version(const char *cs, const char *ce) {
	const char *te, *t, *tend, *q;
	if (!(q = xml_element(cs, ce, "releases", &te, &t, &tend))) {
		return NULL;
	}
	const char *rt, *rtt, *rte;
	const char *r = xml_element(t, tend, "release", &rt, &rtt, &rte);
	char v[128];
	return r && xml_attr(r + 1, rt, "version", v, sizeof(v)) ? xml_unescape(v, strlen(v)) : NULL;
}

/* the largest cached icon up to 128 px that is there */
static char *icon_of(const char *cs, const char *ce, const char *icons) {
	if (!icons) {
		return NULL;
	}
	const char *q = cs, *te, *t, *tend;
	char *best = NULL;
	int best_w = 0;
	while ((q = xml_element(q, ce, "icon", &te, &t, &tend))) {
		char kind[16] = "", width[16] = "", scale[8] = "";
		xml_attr(q + 1, te, "type", kind, sizeof(kind));
		int w = xml_attr(q + 1, te, "width", width, sizeof(width)) ? atoi(width) : 64;
		bool scaled = xml_attr(q + 1, te, "scale", scale, sizeof(scale)) && atoi(scale) > 1;
		if (!strcmp(kind, "cached") && w <= 128 && w > best_w) {
			char *file = xml_unescape(t, tend - t);
			char path[4096];
			snprintf(path, sizeof(path), "%s/%dx%d%s/%s", icons, w, w, scaled ? "@2" : "", file);
			free(file);
			if (access(path, R_OK) == 0) {
				free(best);
				best = xstrdup(path);
				best_w = w;
			}
		}
		q = tend;
	}
	return best;
}

static int by_id(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return strcmp(x->name, y->name);
}

static bool app_type(const char *comp, const char *tag_end) {
	char type[64] = "";
	xml_attr(comp + 1, tag_end, "type", type, sizeof(type));
	return !strcmp(type, "desktop-application") || !strcmp(type, "desktop") ||
		!strcmp(type, "console-application");
}

void flathub_parse(struct flathub *fh, const char *xml, size_t len, const char *icons) {
	const char *end = xml + len, *p = xml, *comp, *tag_end, *cs, *ce;
	while ((comp = xml_element(p, end, "component", &tag_end, &cs, &ce))) {
		p = ce;
		if (!app_type(comp, tag_end)) {
			continue;	/* runtimes, add-ons, fonts */
		}
		struct skips sk;
		find_skips(cs, ce, NOT_OURS, &sk);
		cur_skips = &sk;
		char *id = child_text(cs, ce, "id");
		if (!id || !*id) {
			free(id);
			cur_skips = NULL;
			continue;
		}
		/* "org.foo.Bar.desktop": old catalogues name apps by their file */
		size_t n = strlen(id);
		if (n > 8 && !strcmp(id + n - 8, ".desktop")) {
			id[n - 8] = 0;
		}
		struct pkg *a = pkg_new(id, "flathub");
		free(id);
		a->app_name = child_text(cs, ce, "name");
		a->summary = child_text(cs, ce, "summary");
		a->license = child_text(cs, ce, "project_license");
		a->packager = developer_of(cs, ce);
		a->version = latest_version(cs, ce);
		a->depends = bundle_runtime(cs, ce);
		a->icon = icon_of(cs, ce, icons);
		a->mobile = mobile_of(cs, ce);
		char *v = custom_value(cs, ce, "flathub::verification::verified");
		a->verified = v && !strcmp(v, "true");
		free(v);
		const char *te, *t, *tend, *q;
		if ((q = xml_element(cs, ce, "url", &te, &t, &tend))) {
			const char *uq = cs;
			while ((uq = xml_element(uq, ce, "url", &te, &t, &tend))) {
				char kind[32] = "";
				xml_attr(uq + 1, te, "type", kind, sizeof(kind));
				if (!strcmp(kind, "homepage")) {
					a->url = xml_unescape(t, tend - t);
					break;
				}
				uq = tend;
			}
		}
		const char *cats_e;
		if ((q = xml_element(cs, ce, "categories", &te, &t, &cats_e))) {
			const char *c = t;
			while ((q = xml_element(c, cats_e, "category", &te, &t, &tend))) {
				char *one = xml_unescape(t, tend - t);
				append_word(&a->categories, one);
				free(one);
				c = tend;
			}
		}
		/* the untranslated keywords (they help search find "editor") */
		const char *kq = cs, *kte, *kt, *ktend;
		while ((kq = xml_element(kq, ce, "keywords", &kte, &kt, &ktend))) {
			if (untranslated(kq, kte)) {
				const char *c = kt;
				while ((q = xml_element(c, ktend, "keyword", &te, &t, &tend))) {
					if (untranslated(q, te)) {
						char *one = xml_unescape(t, tend - t);
						for (char *s = one; *s; s++) {
							*s = *s == ' ' ? '-' : *s;
						}
						append_word(&a->keywords, one);
						free(one);
					}
					c = tend;
				}
				break;
			}
			kq = ktend;
		}
		cur_skips = NULL;
		vec_push(&fh->apps, a);
	}
	qsort(fh->apps.v, fh->apps.n, sizeof(void *), by_id);
	/* the same ID twice (an old and a new entry): keep the first */
	int w = 0;
	for (int i = 0; i < fh->apps.n; i++) {
		if (w && !strcmp(((struct pkg *)fh->apps.v[w - 1])->name, ((struct pkg *)fh->apps.v[i])->name)) {
			pkg_free(fh->apps.v[i]);
			continue;
		}
		fh->apps.v[w++] = fh->apps.v[i];
	}
	fh->apps.n = w;
}

static bool catalogue_path(const char *dir, char *out, size_t size) {
	snprintf(out, size, "%s/appstream.xml", dir);
	if (access(out, R_OK) == 0) {
		return true;
	}
	snprintf(out, size, "%s/appstream.xml.gz", dir);
	return access(out, R_OK) == 0;
}

long flathub_stamp(const char *dir) {
	/* the plain XML comes out of flatpak's OSTree checkout with the time 0:
	 * the gzipped copy beside it has when it came */
	char path[600];
	snprintf(path, sizeof(path), "%s/appstream.xml.gz", dir);
	long m = file_mtime(path);
	if (m <= 0) {
		snprintf(path, sizeof(path), "%s/appstream.xml", dir);
		m = file_mtime(path);
	}
	return m;
}

/* ---- the index: what the list needs of the catalogue, kept --------------------- */

/* Reading the catalogue takes two seconds on the phone; what the lists need of
 * it is kept in ~/.cache/ztstore/flathub-index, a line per app, read again
 * in a few milliseconds until the catalogue changes:
 *   ztstore-flathub-index 1 MTIME PATH
 *   ID \t NAME \t SUMMARY \t ICON \t CATEGORIES \t KEYWORDS \t LICENCE \t
 *   DEVELOPER \t VERSION \t RUNTIME \t HOMEPAGE \t FLAGS (v verified, m mobile) */
#define INDEX_MAGIC	"ztstore-flathub-index 1"

static void put_field(FILE *f, const char *s, bool last) {
	for (const char *c = s ? s : ""; *c; c++) {
		fputc(*c == '\t' || *c == '\n' || *c == '\r' ? ' ' : *c, f);
	}
	fputc(last ? '\n' : '\t', f);
}

bool flathub_index_write(const struct flathub *fh, const char *path) {
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	fprintf(f, INDEX_MAGIC " %ld %s\n", fh->stamp, fh->path);
	for (int i = 0; i < fh->apps.n; i++) {
		struct pkg *p = fh->apps.v[i];
		const char *fields[] = { p->name, p->app_name, p->summary, p->icon, p->categories, p->keywords,
			p->license, p->packager, p->version, p->depends, p->url };
		for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
			put_field(f, fields[k], false);
		}
		char flags[4] = "";
		snprintf(flags, sizeof(flags), "%s%s", p->verified ? "v" : "", p->mobile ? "m" : "");
		put_field(f, flags, true);
	}
	bool ok = !ferror(f);
	ok = fclose(f) == 0 && ok;
	return ok && rename(tmp, path) == 0;
}

bool flathub_index_read(struct flathub *fh, const char *path) {
	size_t n;
	char *text = read_all(path, &n);
	if (!text) {
		return false;
	}
	char *nl = strchr(text, '\n');
	long stamp = 0;
	char head[700];
	snprintf(head, sizeof(head), INDEX_MAGIC " %ld %s", fh->stamp, fh->path);
	if (!nl || (size_t)(nl - text) != strlen(head) || strncmp(text, head, nl - text) ||
			sscanf(text + strlen(INDEX_MAGIC), " %ld", &stamp) != 1 || stamp != fh->stamp) {
		free(text);
		return false;	/* another catalogue's: read the catalogue */
	}
	char *line = nl + 1;
	while (line && *line) {
		char *end = strchr(line, '\n');
		if (end) {
			*end = 0;
		}
		char *f[12] = { 0 };
		int k = 0;
		for (char *s = line; s && k < 12; k++) {
			char *tab = strchr(s, '\t');
			if (tab) {
				*tab = 0;
			}
			f[k] = s;
			s = tab ? tab + 1 : NULL;
		}
		if (k == 12 && *f[0]) {
			struct pkg *p = pkg_new(f[0], "flathub");
			char **to[] = { NULL, &p->app_name, &p->summary, &p->icon, &p->categories, &p->keywords,
				&p->license, &p->packager, &p->version, &p->depends, &p->url };
			for (int i = 1; i < 11; i++) {
				if (*f[i]) {
					*to[i] = xstrdup(f[i]);
				}
			}
			p->verified = strchr(f[11], 'v') != NULL;
			p->mobile = strchr(f[11], 'm') != NULL;
			vec_push(&fh->apps, p);
		}
		line = end ? end + 1 : NULL;
	}
	free(text);
	return true;
}

bool flathub_load(struct flathub *fh, const char *dir) {
	memset(fh, 0, sizeof(*fh));
	if (!catalogue_path(dir, fh->path, sizeof(fh->path))) {
		fh->path[0] = 0;
		return false;
	}
	fh->stamp = flathub_stamp(dir);
	/* "active" is a link to the deployed commit: the real path names it */
	char real[PATH_MAX];
	if (realpath(fh->path, real)) {
		snprintf(fh->path, sizeof(fh->path), "%s", real);
	}
	char index[4096];
	cache_path(index, sizeof(index), "flathub-index");
	if (flathub_index_read(fh, index)) {
		return true;	/* (written sorted) */
	}
	vec_free(&fh->apps, pkg_free);
	struct text t;
	if (!text_open(&t, fh->path)) {
		return false;
	}
	char icons[600];
	snprintf(icons, sizeof(icons), "%s/icons", dir);
	flathub_parse(fh, t.s, t.n, icons);
	text_close(&t);
	flathub_index_write(fh, index);
	return true;
}

void flathub_free(struct flathub *fh) {
	vec_free(&fh->apps, pkg_free);
}

struct pkg *flathub_find(const struct flathub *fh, const char *id) {
	int lo = 0, hi = fh->apps.n;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(((struct pkg *)fh->apps.v[mid])->name, id);
		if (!c) {
			return fh->apps.v[mid];
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return NULL;
}

/* ---- one app's details -------------------------------------------------------------- */

void fh_detail_free(struct fh_detail *d) {
	if (!d) {
		return;
	}
	for (int i = 0; i < d->shots.n; i++) {
		struct fh_shot *s = d->shots.v[i];
		free(s->caption);
		free(s->url);
		free(s);
	}
	vec_free(&d->shots, NULL);
	for (int i = 0; i < d->releases.n; i++) {
		struct fh_release *r = d->releases.v[i];
		free(r->version);
		free(r->notes);
		free(r);
	}
	vec_free(&d->releases, NULL);
	for (int i = 0; i < d->perms.n; i++) {
		struct fh_perm *p = d->perms.v[i];
		free(p->text);
		free(p);
	}
	vec_free(&d->perms, NULL);
	char **strs[] = { &d->id, &d->description, &d->developer, &d->license, &d->project_group, &d->homepage,
		&d->bugtracker, &d->donation, &d->help, &d->vcs, &d->translate, &d->contact, &d->rating,
		&d->runtime, &d->download, &d->installed_size, &d->remote_version, &d->runtime_download };
	for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
		free(*strs[i]);
	}
	free(d);
}

/* the image that fits a screen `width_px` across: the smallest at least as
 * wide, else the widest there is */
static void shots_of(const char *cs, const char *ce, int width_px, struct vec *out) {
	const char *te, *t, *tend, *q;
	if (!(q = xml_element(cs, ce, "screenshots", &te, &t, &tend))) {
		return;
	}
	const char *ss = t, *se = tend, *sq = ss, *ste, *st, *stend;
	while ((sq = xml_element(sq, se, "screenshot", &ste, &st, &stend))) {
		sq = stend;
		struct fh_shot *s = calloc(1, sizeof(*s));
		const char *ct, *ctend;
		const char *c = child(st, stend, "caption", NULL, &ct, &ctend);
		if (c) {
			s->caption = xml_unescape(ct, ctend - ct);
		}
		int best_fit = 0, best_any = 0;
		char *fit = NULL, *any = NULL;
		int fit_h = 0, any_h = 0;
		const char *iq = st, *ite, *it, *itend;
		while ((iq = xml_element(iq, stend, "image", &ite, &it, &itend))) {
			const char *open = iq;
			iq = itend;
			char ws[16] = "", hs[16] = "";
			int w = xml_attr(open + 1, ite, "width", ws, sizeof(ws)) ? atoi(ws) : 0;
			int h = xml_attr(open + 1, ite, "height", hs, sizeof(hs)) ? atoi(hs) : 0;
			char *url = xml_unescape(it, itend - it);
			if (!*url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) {
				free(url);
				continue;
			}
			if (w >= width_px && (!fit || w < best_fit)) {
				free(fit);
				fit = xstrdup(url);
				best_fit = w;
				fit_h = h;
			}
			if (!any || w > best_any) {
				free(any);
				any = xstrdup(url);
				best_any = w;
				any_h = h;
			}
			free(url);
		}
		if (fit) {
			s->url = fit;
			s->w = best_fit;
			s->h = fit_h;
			free(any);
		} else {
			s->url = any;
			s->w = best_any;
			s->h = any_h;
		}
		if (!s->url) {
			free(s->caption);
			free(s);
			continue;
		}
		vec_push(out, s);
	}
}

static void releases_of(const char *cs, const char *ce, struct vec *out) {
	const char *te, *t, *tend, *q;
	if (!(q = xml_element(cs, ce, "releases", &te, &t, &tend))) {
		return;
	}
	const char *rq = t, *rte, *rt, *rtend, *re = tend;
	while ((rq = xml_element(rq, re, "release", &rte, &rt, &rtend))) {
		struct fh_release *r = calloc(1, sizeof(*r));
		char v[128] = "", ts[32] = "", type[32] = "";
		xml_attr(rq + 1, rte, "version", v, sizeof(v));
		xml_attr(rq + 1, rte, "type", type, sizeof(type));
		if (xml_attr(rq + 1, rte, "timestamp", ts, sizeof(ts))) {
			r->when = atol(ts);
		} else if (xml_attr(rq + 1, rte, "date", ts, sizeof(ts))) {
			struct tm tm = { 0 };
			if (strptime(ts, "%Y-%m-%d", &tm)) {
				r->when = (long)timegm(&tm);
			}
		}
		r->version = xstrdup(v);
		if (rte[-1] != '/') {
			const char *dt, *dtend;
			if (child(rt, rtend, "description", NULL, &dt, &dtend)) {
				r->notes = xml_markup_text(dt, dtend);
				if (!*r->notes) {
					free(r->notes);
					r->notes = NULL;
				}
			}
		}
		rq = rte[-1] == '/' ? rte + 1 : rtend;
		if (!strcmp(type, "development")) {
			free(r->version);
			free(r->notes);
			free(r);
			continue;	/* betas: not what is installed */
		}
		vec_push(out, r);
	}
}

/* OARS: what the app says it shows, in a few words ("mild cartoon violence") */
static char *rating_of(const char *cs, const char *ce) {
	const char *te, *t, *tend, *q;
	if (!(q = xml_element(cs, ce, "content_rating", &te, &t, &tend))) {
		return NULL;
	}
	static const struct { const char *id, *words; } names[] = {
		{ "violence-cartoon", "cartoon violence" }, { "violence-fantasy", "fantasy violence" },
		{ "violence-realistic", "realistic violence" }, { "violence-bloodshed", "bloodshed" },
		{ "violence-sexual", "sexual violence" }, { "violence-desecration", "desecration" },
		{ "violence-slavery", "slavery" }, { "violence-worship", "worship" },
		{ "drugs-alcohol", "alcohol" }, { "drugs-narcotics", "drugs" }, { "drugs-tobacco", "tobacco" },
		{ "sex-nudity", "nudity" }, { "sex-themes", "sexual themes" }, { "sex-homosexuality", "homosexuality" },
		{ "sex-prostitution", "prostitution" }, { "sex-adultery", "adultery" },
		{ "sex-appearance", "sexualised characters" }, { "language-profanity", "profanity" },
		{ "language-humor", "crude humour" }, { "language-discrimination", "discriminatory language" },
		{ "social-chat", "chat with others" }, { "social-info", "shares information about you" },
		{ "social-audio", "voice or video chat" }, { "social-location", "shares your location" },
		{ "social-contacts", "finds contacts" }, { "money-purchasing", "in-app purchases" },
		{ "money-gambling", "gambling" }, { "money-advertising", "advertising" },
	};
	char *out = NULL;
	size_t len = 0;
	const char *aq = t, *ate, *at, *atend;
	while (te[-1] != '/' && (aq = xml_element(aq, tend, "content_attribute", &ate, &at, &atend))) {
		const char *open = aq;
		aq = atend;
		char id[64] = "";
		xml_attr(open + 1, ate, "id", id, sizeof(id));
		char *level = xml_unescape(at, atend - at);
		if (!strcmp(level, "none") || !*level) {
			free(level);
			continue;
		}
		const char *words = id;
		for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			if (!strcmp(names[k].id, id)) {
				words = names[k].words;
			}
		}
		char one[160];
		int n = snprintf(one, sizeof(one), "%s%s %s", len ? ", " : "", level, words);
		out = realloc(out, len + n + 1);
		memcpy(out + len, one, n + 1);
		len += n;
		free(level);
	}
	if (!out) {
		out = xstrdup("for everyone: nothing of concern declared");
	}
	return out;
}

static struct fh_detail *detail_from(const char *cs, const char *ce, const char *id, int width_px) {
	struct fh_detail *d = calloc(1, sizeof(*d));
	d->id = xstrdup(id);
	const char *dt, *dtend;
	static const char *const skip[] = { "releases", NULL };
	if (child(cs, ce, "description", skip, &dt, &dtend)) {
		d->description = xml_markup_text(dt, dtend);
	}
	d->developer = developer_of(cs, ce);
	d->license = child_text(cs, ce, "project_license");
	d->project_group = child_text(cs, ce, "project_group");
	d->runtime = bundle_runtime(cs, ce);
	d->mobile = mobile_of(cs, ce);
	char *v = custom_value(cs, ce, "flathub::verification::verified");
	d->verified = v && !strcmp(v, "true");
	free(v);
	const char *uq = cs, *te, *t, *tend;
	while ((uq = xml_element(uq, ce, "url", &te, &t, &tend))) {
		char kind[32] = "";
		xml_attr(uq + 1, te, "type", kind, sizeof(kind));
		char **to = !strcmp(kind, "homepage") ? &d->homepage : !strcmp(kind, "bugtracker") ? &d->bugtracker :
			!strcmp(kind, "donation") ? &d->donation : !strcmp(kind, "help") ? &d->help :
			!strcmp(kind, "vcs-browser") ? &d->vcs : !strcmp(kind, "translate") ? &d->translate :
			!strcmp(kind, "contact") ? &d->contact : NULL;
		if (to && !*to) {
			*to = xml_unescape(t, tend - t);
		}
		uq = tend;
	}
	shots_of(cs, ce, width_px, &d->shots);
	releases_of(cs, ce, &d->releases);
	d->rating = rating_of(cs, ce);
	return d;
}

/* the component whose <id> is `id` */
static bool find_component(const char *xml, size_t len, const char *id, const char **cs, const char **ce) {
	const char *end = xml + len;
	char needles[2][300];
	snprintf(needles[0], sizeof(needles[0]), "<id>%s</id>", id);
	snprintf(needles[1], sizeof(needles[1]), "<id>%s.desktop</id>", id);
	for (int k = 0; k < 2; k++) {
		const char *p = xml;
		const char *hit;
		while ((hit = memmem(p, end - p, needles[k], strlen(needles[k])))) {
			/* back to the <component that holds it */
			const char *c = hit;
			while (c > xml && !(c[0] == '<' && !strncmp(c, "<component", 10))) {
				c--;
			}
			const char *te, *t, *tend;
			if (xml_element(c, end, "component", &te, &t, &tend) == c && hit < tend && app_type(c, te)) {
				*cs = t;
				*ce = tend;
				return true;
			}
			p = hit + 1;
		}
	}
	return false;
}

struct fh_detail *flathub_detail_parse(const char *xml, size_t len, const char *id, int width_px) {
	const char *cs, *ce;
	if (!find_component(xml, len, id, &cs, &ce)) {
		return NULL;
	}
	return detail_from(cs, ce, id, width_px);
}

struct fh_detail *flathub_detail(const struct flathub *fh, const char *id, int width_px) {
	struct text t;
	if (!fh->path[0] || !text_open(&t, fh->path)) {
		return NULL;
	}
	struct fh_detail *d = flathub_detail_parse(t.s, t.n, id, width_px);
	text_close(&t);
	return d;
}

/* ---- permissions ---------------------------------------------------------------------- */

static void perm(struct vec *out, bool warn, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void perm(struct vec *out, bool warn, const char *fmt, ...) {
	char buf[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (int i = 0; i < out->n; i++) {
		if (!strcmp(((struct fh_perm *)out->v[i])->text, buf)) {
			return;
		}
	}
	struct fh_perm *p = calloc(1, sizeof(*p));
	p->text = xstrdup(buf);
	p->warn = warn;
	vec_push(out, p);
}

static const char *xdg_dir_name(const char *d) {
	static const struct { const char *k, *v; } dirs[] = {
		{ "xdg-download", "your Downloads folder" }, { "xdg-documents", "your Documents folder" },
		{ "xdg-pictures", "your Pictures folder" }, { "xdg-music", "your Music folder" },
		{ "xdg-videos", "your Videos folder" }, { "xdg-desktop", "your Desktop folder" },
		{ "xdg-templates", "your Templates folder" }, { "xdg-public-share", "your Public folder" },
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		size_t n = strlen(dirs[i].k);
		if (!strncmp(d, dirs[i].k, n) && (d[n] == 0 || d[n] == '/')) {
			return dirs[i].v;
		}
	}
	return NULL;
}

static void filesystem(struct vec *out, char *fs) {
	bool ro = false, create = false;
	char *colon = strrchr(fs, ':');
	if (colon) {
		ro = !strcmp(colon, ":ro");
		create = !strcmp(colon, ":create");
		*colon = 0;
	}
	(void)create;
	bool negated = fs[0] == '!';
	if (negated) {
		return;	/* taken away, not given */
	}
	const char *how = ro ? "read" : "read and change";
	const char *xdg;
	if (!strcmp(fs, "host")) {
		perm(out, true, "can %s all your files and the system's", how);
	} else if (!strcmp(fs, "host-os")) {
		perm(out, !ro, "can %s the system's programs and libraries", how);
	} else if (!strcmp(fs, "host-etc")) {
		perm(out, !ro, "can %s the system's settings (/etc)", how);
	} else if (!strcmp(fs, "home") || !strcmp(fs, "~")) {
		perm(out, true, "can %s all the files in your home folder", how);
	} else if ((xdg = xdg_dir_name(fs))) {
		const char *sub = strchr(fs, '/');
		if (sub && sub[1]) {
			perm(out, false, "can %s %s in %s", how, sub + 1, xdg);
		} else {
			perm(out, false, "can %s %s", how, xdg);
		}
	} else if (starts_with(fs, "xdg-config")) {
		perm(out, false, "can %s settings in %s", how, fs[10] == '/' ? fs + 11 : "~/.config");
	} else if (starts_with(fs, "xdg-data")) {
		perm(out, false, "can %s data in %s", how, fs[8] == '/' ? fs + 9 : "~/.local/share");
	} else if (starts_with(fs, "xdg-cache")) {
		perm(out, false, "can %s cached files in %s", how, fs[9] == '/' ? fs + 10 : "~/.cache");
	} else if (starts_with(fs, "xdg-run")) {
		perm(out, false, "can use %s in the runtime folder", fs[7] == '/' ? fs + 8 : "everything");
	} else if (fs[0] == '~' || fs[0] == '/') {
		bool broad = !strcmp(fs, "/") || !strcmp(fs, "~/") || !strncmp(fs, "/media", 6) ||
			!strncmp(fs, "/run/media", 10) || !strncmp(fs, "/mnt", 4);
		perm(out, broad && !ro, "can %s %s", how, fs);
	} else if (*fs) {
		perm(out, false, "can %s %s", how, fs);
	}
}

static void each_item(const char *list, void (*fn)(struct vec *, char *), struct vec *out) {
	char *copy = xstrdup(list), *save = NULL;
	for (char *w = strtok_r(copy, ";", &save); w; w = strtok_r(NULL, ";", &save)) {
		fn(out, trim(w));
	}
	free(copy);
}

static void shared(struct vec *out, char *s) {
	if (!strcmp(s, "network")) {
		perm(out, false, "uses the internet");
	}
	/* (ipc: shared memory with X11, not something to explain) */
}

static void socket_(struct vec *out, char *s) {
	if (!strcmp(s, "x11")) {
		perm(out, true, "uses X11: it can see what other X11 apps show and what is typed into them");
	} else if (!strcmp(s, "pulseaudio")) {
		perm(out, false, "plays sound and can use the microphone");
	} else if (!strcmp(s, "session-bus")) {
		perm(out, true, "full access to your session's services: it can leave its sandbox");
	} else if (!strcmp(s, "system-bus")) {
		perm(out, true, "full access to the system's services");
	} else if (!strcmp(s, "ssh-auth")) {
		perm(out, true, "can use your SSH keys");
	} else if (!strcmp(s, "gpg-agent")) {
		perm(out, true, "can use your GPG keys");
	} else if (!strcmp(s, "cups")) {
		perm(out, false, "can print");
	} else if (!strcmp(s, "pcsc")) {
		perm(out, false, "can use smart cards");
	} else if (!strcmp(s, "inherit-wayland-socket")) {
		perm(out, false, "uses the display");
	}
	/* wayland and fallback-x11: showing its windows, the normal thing */
}

static void device(struct vec *out, char *d) {
	if (!strcmp(d, "all")) {
		perm(out, true, "can use every device: camera, microphone, USB and more");
	} else if (!strcmp(d, "dri")) {
		perm(out, false, "uses the graphics chip");
	} else if (!strcmp(d, "kvm")) {
		perm(out, false, "can run virtual machines");
	} else if (!strcmp(d, "input")) {
		perm(out, true, "can read input devices (touch, keys) directly");
	} else if (!strcmp(d, "usb")) {
		perm(out, true, "can use USB devices");
	} else if (!strcmp(d, "shm")) {
		perm(out, false, "shares memory with other apps");
	}
}

static void feature(struct vec *out, char *f) {
	if (!strcmp(f, "bluetooth")) {
		perm(out, false, "can use Bluetooth");
	} else if (!strcmp(f, "devel")) {
		perm(out, true, "can debug other programs (developer tools)");
	} else if (!strcmp(f, "multiarch")) {
		perm(out, false, "runs programs for other processors");
	} else if (!strcmp(f, "canbus")) {
		perm(out, false, "can use CAN bus devices");
	}
}

static void bus_name(struct vec *out, const char *name, bool system) {
	static const struct { const char *name, *words; bool warn; } known[] = {
		{ "org.freedesktop.Flatpak", "can run commands outside its sandbox", true },
		{ "org.freedesktop.secrets", "can read your saved passwords (keyring)", true },
		{ "org.freedesktop.Notifications", "shows notifications", false },
		{ "org.kde.StatusNotifierWatcher", "puts an icon in the system tray", false },
		{ "org.freedesktop.ScreenSaver", "can keep the screen on", false },
		{ "org.freedesktop.PowerManagement", "can keep the phone awake", false },
		{ "org.freedesktop.login1", "can see sessions and keep the phone awake", false },
		{ "org.freedesktop.NetworkManager", "can see and change network settings", true },
		{ "org.freedesktop.UPower", "can see the battery", false },
		{ "org.freedesktop.UDisks2", "can manage disks", true },
		{ "org.bluez", "can use Bluetooth", false },
		{ "org.freedesktop.ModemManager1", "can use the modem", true },
		{ "org.freedesktop.GeoClue2", "can ask your location", false },
		{ "org.mpris.MediaPlayer2", "shows its playback controls", false },
		{ "ca.desrt.dconf", "keeps settings with the system's", false },
		{ "org.gnome.SettingsDaemon", "reads desktop settings", false },
		{ "org.freedesktop.systemd1", "can control system services", true },
		{ "org.freedesktop.PolicyKit1", "can ask for administrator rights", true },
	};
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		size_t n = strlen(known[i].name);
		if (!strncmp(name, known[i].name, n) && (name[n] == 0 || name[n] == '.' || name[n] == '*')) {
			perm(out, known[i].warn, "%s", known[i].words);
			return;
		}
	}
	if (starts_with(name, "org.freedesktop.portal.")) {
		return;	/* portals: they ask you each time */
	}
	if (system) {
		perm(out, false, "talks to the system service %s", name);
	}
	/* other session names: the app's own helpers, mostly; not listed */
}

void flathub_permissions(const char *metadata, struct vec *out) {
	char *text = xstrdup(metadata), *save = NULL;
	enum { NONE, CONTEXT, SESSION, SYSTEM } in = NONE;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *l = trim(line);
		if (l[0] == '[') {
			in = !strcmp(l, "[Context]") ? CONTEXT : !strcmp(l, "[Session Bus Policy]") ? SESSION :
				!strcmp(l, "[System Bus Policy]") ? SYSTEM : NONE;
			continue;
		}
		char *eq = strchr(l, '=');
		if (!eq || in == NONE) {
			continue;
		}
		*eq = 0;
		char *key = trim(l), *val = trim(eq + 1);
		if (in == CONTEXT) {
			if (!strcmp(key, "shared")) {
				each_item(val, shared, out);
			} else if (!strcmp(key, "sockets")) {
				each_item(val, socket_, out);
			} else if (!strcmp(key, "devices")) {
				each_item(val, device, out);
			} else if (!strcmp(key, "features")) {
				each_item(val, feature, out);
			} else if (!strcmp(key, "filesystems")) {
				each_item(val, filesystem, out);
			}
		} else if (!strcmp(val, "talk") || !strcmp(val, "own")) {
			bus_name(out, key, in == SYSTEM);
		}
	}
	free(text);
	/* x11 next to wayland is only a fallback: not a broad reach */
	bool wayland = strstr(metadata, "wayland") != NULL;
	for (int i = 0; wayland && i < out->n; i++) {
		struct fh_perm *p = out->v[i];
		if (starts_with(p->text, "uses X11")) {
			p->warn = false;
		}
	}
	if (!out->n) {
		perm(out, false, "no special permissions: it keeps to its own sandbox");
	}
}

/* ---- flatpak, run as the person -------------------------------------------------------- */

static char *flatpak(char *const argv[], int *status) {
	return run_capture(argv, status);
}

void flathub_parse_remote_info(const char *text, struct fh_detail *d) {
	char *copy = xstrdup(text), *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *colon = strchr(line, ':');
		if (!colon) {
			continue;
		}
		*colon = 0;
		char *key = trim(line), *val = trim(colon + 1);
		if (!strcmp(key, "Download Size") || !strcmp(key, "Download")) {
			set_str(&d->download, val);
		} else if (!strcmp(key, "Installed Size") || !strcmp(key, "Installed")) {
			set_str(&d->installed_size, val);
		} else if (!strcmp(key, "Version")) {
			set_str(&d->remote_version, val);
		} else if (!strcmp(key, "Runtime") && !d->runtime) {
			set_str(&d->runtime, val);
		}
	}
	free(copy);
	d->have_remote = d->download || d->installed_size;
}

static bool runtime_installed(const char *runtime) {
	/* "org.gnome.Platform/aarch64/51" is installed when its folder is there */
	char name[200], arch[32], branch[64];
	if (sscanf(runtime, "%199[^/]/%31[^/]/%63s", name, arch, branch) != 3) {
		return true;
	}
	char path[512];
	snprintf(path, sizeof(path), "/var/lib/flatpak/runtime/%s/%s/%s/active", name, arch, branch);
	if (access(path, F_OK) == 0) {
		return true;
	}
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.local/share/flatpak/runtime/%s/%s/%s/active", home ? home : "", name, arch,
		branch);
	return access(path, F_OK) == 0;
}

void flathub_remote_info(struct fh_detail *d) {
	char ref[300];
	snprintf(ref, sizeof(ref), "app/%s/" FLATHUB_ARCH "/stable", d->id);
	char *info_argv[] = { "flatpak", "remote-info", "--system", FLATHUB_REMOTE, ref, NULL };
	int st;
	char *out = flatpak(info_argv, &st);
	if (out && st == 0) {
		flathub_parse_remote_info(out, d);
	}
	free(out);
	char *meta_argv[] = { "flatpak", "remote-info", "--system", "-m", FLATHUB_REMOTE, ref, NULL };
	out = flatpak(meta_argv, &st);
	if (out && st == 0) {
		flathub_permissions(out, &d->perms);
		/* X11 only, and no Xwayland on the phone: it cannot show a window */
		if (strstr(out, "x11") && !strstr(out, "wayland") && access("/usr/bin/Xwayland", X_OK) != 0) {
			struct fh_perm *p = calloc(1, sizeof(*p));
			p->text = xstrdup("draws only through X11, which this phone does not run (no Xwayland): "
				"it will not open");
			p->warn = true;
			vec_push(&d->perms, p);
			d->x11_only = true;
		}
	}
	free(out);
	if (d->runtime && !runtime_installed(d->runtime)) {
		d->runtime_missing = true;
		char rref[320];
		snprintf(rref, sizeof(rref), "runtime/%s", d->runtime);
		char *rt_argv[] = { "flatpak", "remote-info", "--system", FLATHUB_REMOTE, rref, NULL };
		out = flatpak(rt_argv, &st);
		if (out && st == 0) {
			struct fh_detail tmp = { 0 };
			flathub_parse_remote_info(out, &tmp);
			d->runtime_download = tmp.download;
			free(tmp.installed_size);
			free(tmp.remote_version);
			free(tmp.runtime);
		}
		free(out);
	}
}

void flathub_parse_list(const char *text, struct vec *out) {
	char *copy = xstrdup(text), *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4] = { 0 };
		int n = 0;
		for (char *s = line; s && n < 4; n++) {
			char *tab = strchr(s, '\t');
			if (tab) {
				*tab = 0;
			}
			f[n] = trim(s);
			s = tab ? tab + 1 : NULL;
		}
		if (n < 1 || !f[0][0] || !strchr(f[0], '.')) {
			continue;
		}
		struct pkg *p = pkg_new(f[0], "flathub");
		p->installed = true;
		p->explicit_install = true;
		if (n > 1 && *f[1]) {
			set_str(&p->installed_version, f[1]);
			set_str(&p->version, f[1]);
		}
		if (n > 2 && *f[2] && strcmp(f[2], FLATHUB_REMOTE)) {
			set_str(&p->packager, f[2]);	/* (another remote: said where) */
		}
		char path[512];
		snprintf(path, sizeof(path), "/var/lib/flatpak/exports/share/applications/%s.desktop", f[0]);
		if (access(path, R_OK) == 0) {
			set_str(&p->desktop, path);
		}
		vec_push(out, p);
	}
	free(copy);
}

void flathub_installed(struct vec *out) {
	char *argv[] = { "flatpak", "list", "--system", "--app", "--columns=application,version,origin", NULL };
	int st;
	char *text = flatpak(argv, &st);
	if (text && st == 0) {
		flathub_parse_list(text, out);
	}
	free(text);
}

bool flathub_updates(struct vec *out) {
	char *argv[] = { "flatpak", "remote-ls", "--system", "--updates", "--app",
		"--columns=application,version", NULL };
	int st;
	char *text = flatpak(argv, &st);
	bool ok = text && st == 0;
	if (ok) {
		flathub_parse_list(text, out);
		for (int i = 0; i < out->n; i++) {
			struct pkg *p = out->v[i];
			p->installed = true;
			set_str(&p->installed_version, NULL);	/* (the column is the new one's) */
		}
	}
	free(text);
	return ok;
}

bool flathub_remote_present(void) {
	char *argv[] = { "flatpak", "remotes", "--system", "--columns=name", NULL };
	int st;
	char *text = flatpak(argv, &st);
	bool yes = false;
	if (text && st == 0) {
		char *save = NULL;
		for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (!strcmp(trim(l), FLATHUB_REMOTE)) {
				yes = true;
			}
		}
	}
	free(text);
	return yes;
}

bool flathub_refresh(long max_age) {
	char path[600], asked[4096];
	bool have = catalogue_path(FLATHUB_AS_DIR, path, sizeof(path));
	/* when it was last asked for (an unchanged catalogue keeps its old time) */
	cache_path(asked, sizeof(asked), "flathub-refreshed");
	long last = file_mtime(asked), made = flathub_stamp(FLATHUB_AS_DIR);
	last = last > made ? last : made;
	if (have && time(NULL) - last < max_age) {
		return true;
	}
	char *argv[] = { "flatpak", "update", "--system", "--appstream", "--noninteractive", FLATHUB_REMOTE, NULL };
	int st;
	free(flatpak(argv, &st));
	if (st == 0) {
		write_atomic(asked, "", 0);
	}
	return st == 0;
}

/* ---- the web API ----------------------------------------------------------------------- */

void flathub_api_ids(const struct json *j, struct vec *ids) {
	if (!j) {
		return;
	}
	const char *keys[] = { "hits", "apps", NULL };
	for (int k = 0; keys[k]; k++) {
		struct json *arr = json_get(j, keys[k]);
		if (arr && arr->type == J_ARR) {
			for (int i = 0; i < arr->n; i++) {
				const char *id = json_str(arr->items[i], "app_id");
				if (id) {
					vec_push(ids, xstrdup(id));
				}
			}
			return;
		}
	}
	if (j->type == J_ARR) {	/* a plain list of IDs */
		for (int i = 0; i < j->n; i++) {
			if (j->items[i]->type == J_STR) {
				vec_push(ids, xstrdup(j->items[i]->str));
			}
		}
		return;
	}
	const char *one = json_str(j, "app_id");
	if (one) {
		vec_push(ids, xstrdup(one));
	}
}

static bool api_ids(const char *url, const char *cache_name, struct vec *ids, long max_age) {
	char path[4096];
	cache_path(path, sizeof(path), cache_name);
	/* max_age < 0: the copy there is, never the network */
	if (max_age < 0 ? file_mtime(path) < 0 : !fetch_to_file(url, path, max_age)) {
		return false;
	}
	size_t n;
	char *text = read_all(path, &n);
	if (!text) {
		return false;
	}
	struct json *j = json_parse(text, n);
	free(text);
	if (!j) {
		unlink(path);	/* a broken copy: fetch it again next time */
		return false;
	}
	flathub_api_ids(j, ids);
	json_free(j);
	return true;
}

bool flathub_collection(const char *which, struct vec *ids, long max_age) {
	char url[512], name[128];
	if (!strcmp(which, "picks")) {
		/* this week's picks, and today's app first */
		time_t now = time(NULL);
		struct tm tm;
		gmtime_r(&now, &tm);
		char day[16];
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
		snprintf(url, sizeof(url), FLATHUB_API "/app-picks/app-of-the-day/%s", day);
		bool a = api_ids(url, "flathub-app-of-the-day.json", ids, max_age);
		snprintf(url, sizeof(url), FLATHUB_API "/app-picks/apps-of-the-week/%s", day);
		bool b = api_ids(url, "flathub-apps-of-the-week.json", ids, max_age);
		return a || b;
	}
	if (!strcmp(which, "mobile")) {
		/* (no collection of its own: the popular ones that fit a phone are
		 * picked out by the caller, from the catalogue) */
		which = "popular";
	}
	snprintf(url, sizeof(url), FLATHUB_API "/collection/%s?page=1&per_page=250", which);
	snprintf(name, sizeof(name), "flathub-%s.json", which);
	return api_ids(url, name, ids, max_age);
}

bool flathub_search_other_arch(const struct flathub *fh, const char *term, struct vec *out) {
	if (strlen(term) < 2) {
		return true;
	}
	/* {"query": TERM}, the term escaped for JSON */
	char body[600];
	size_t o = snprintf(body, sizeof(body), "{\"query\":\"");
	for (const char *c = term; *c && o + 8 < sizeof(body); c++) {
		if (*c == '"' || *c == '\\') {
			body[o++] = '\\';
		}
		if ((unsigned char)*c >= 0x20) {
			body[o++] = *c;
		}
	}
	snprintf(body + o, sizeof(body) - o, "\"}");
	char *argv[] = { "curl", "-fsSL", "--compressed", "--max-time", "20", "-X", "POST", "-H",
		"Content-Type: application/json", "--data", body, FLATHUB_API "/search?locale=en", NULL };
	int st;
	char *text = run_capture(argv, &st);
	if (!text || st != 0) {
		free(text);
		return false;
	}
	struct json *j = json_parse(text, strlen(text));
	free(text);
	if (!j) {
		return false;
	}
	struct json *hits = json_get(j, "hits");
	for (int i = 0; hits && hits->type == J_ARR && i < hits->n; i++) {
		struct json *h = hits->items[i];
		const char *id = json_str(h, "app_id");
		if (!id || flathub_find(fh, id)) {
			continue;
		}
		struct json *arches = json_get(h, "arches");
		bool ours = false;
		char *archs = NULL;
		for (int k = 0; arches && arches->type == J_ARR && k < arches->n; k++) {
			if (arches->items[k]->type == J_STR) {
				ours = ours || !strcmp(arches->items[k]->str, FLATHUB_ARCH);
				append_word(&archs, arches->items[k]->str);
			}
		}
		if (ours) {
			free(archs);	/* built for it, but not in the catalogue yet: leave it */
			continue;
		}
		struct pkg *p = pkg_new(id, "flathub");
		set_str(&p->app_name, json_str(h, "name"));
		set_str(&p->summary, json_str(h, "summary"));
		p->arch = archs ? archs : xstrdup("unknown");
		vec_push(out, p);
	}
	json_free(j);
	return true;
}

/* ---- the network ------------------------------------------------------------------------ */

bool network_metered(void) {
	/* "yes", "yes (guessed)", "no", "no (guessed)", "unknown" per device */
	char *argv[] = { "nmcli", "-t", "-f", "GENERAL.STATE,GENERAL.METERED", "device", "show", NULL };
	int st;
	char *text = run_capture(argv, &st);
	bool metered = false;
	if (text && st == 0) {
		bool connected = false;
		char *save = NULL;
		for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (starts_with(l, "GENERAL.STATE:")) {
				connected = strstr(l, "(connected)") != NULL;
			} else if (starts_with(l, "GENERAL.METERED:") && connected && starts_with(l + 16, "yes")) {
				metered = true;
			}
		}
	}
	free(text);
	return metered;
}
