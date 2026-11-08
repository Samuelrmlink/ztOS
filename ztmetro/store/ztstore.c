// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztstore: the ztOS App Store. Apps from the repos in pacman.conf, from the
 * AUR and from Flathub, in one place: lists of what is popular and new (which
 * lists, from a file: see sections.h), categories and groups, search, details
 * (with Flathub's screenshots, descriptions and permissions), installing and
 * removing, and updates. Built on ZrnMetroCore.
 *
 * Nothing it shows is written into it: the repos come from pacman.conf, the
 * packages from pacman's own databases, app names, icons and categories from
 * AppStream, the AUR from its RPC and its daily dump, popularity from
 * pkgstats.archlinux.org, Flathub from flatpak's own AppStream for this arch
 * and flathub.org's API (flathub.c). Slow work runs on threads (jobs.c);
 * installing goes through root services (install.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "appstream.h"
#include "aur.h"
#include "flathub.h"
#include "icons.h"
#include "install.h"
#include "jobs.h"
#include "search.h"
#include "sections.h"
#include "shots.h"
#include "zrnmetrocore.h"

#define MARGIN		ZMC_MARGIN
#define PACMAN_CONF	"/etc/pacman.conf"
#define PKGSTATS_URL	"https://pkgstats.archlinux.org/api/packages?limit=3000&offset=0"
#define UPDATE_STATUS	"/run/ztos-update/status"
#define UPDATE_LIST	"/var/lib/ztos/updates"
#define ROWS_SHOWN	3	/* of each list on the top page */
#define LIST_MAX	100	/* rows on one page (each is a hit; the core keeps 160) */
#define GREY		0.6
#define FH_AS_MAX_AGE	(12 * 3600)	/* Flathub's catalogue, refreshed twice a day */
#define FH_API_MAX_AGE	(12 * 3600)	/* and its lists */
#define SHOTS_MAX	8		/* screenshot pages: the core's most tabs */

enum page {
	P_TOP, P_CATS, P_INSTALLED, P_SEARCH, P_LIST, P_DETAIL, P_PROGRESS,
	P_SHOT, P_SHOT_LAST = P_SHOT + SHOTS_MAX - 1,	/* one screenshot each, a pivot */
	P_COUNT
};
enum action {
	ACT_PKG = 1, ACT_SEARCHBOX, ACT_FILTER, ACT_SEE_ALL, ACT_CAT, ACT_GROUP, ACT_INSTALL, ACT_REMOVE,
	ACT_OPEN, ACT_UPDATE, ACT_PROGRESS, ACT_CANCEL, ACT_DONE, ACT_CHECK_UPDATES, ACT_UPDATE_ALL,
	ACT_RECIPE, ACT_FIND, ACT_RETRY, ACT_INSTALL_BUILT,
	ACT_FH_CAT, ACT_SHOT, ACT_SHOTS_LOAD, ACT_LINK, ACT_ADD_FLATHUB, ACT_FH_UPDATE_ALL, ACT_SHOTS_PAGE,
};

/* ---- what the pages show (touched only on the main loop) ------------------------- */

static struct repo_index *ri;		/* the phone's packages */
static struct vec retired;		/* older ones, freed once no job reads them */
static struct vec sections, cats, groups, mine;
static int n_apps;			/* how many AppStream knows */
static bool loading = true, reload_wanted;
static char load_note[200];

/* Flathub: its catalogue for this arch (or NULL), what of it is installed,
 * and what has an update */
static struct flathub *fh;
static struct vec retired_fh;
static struct vec fh_cats;
static struct vec fh_updates;		/* struct pkg *: installed apps with an update */
static bool fh_updates_loading, fh_updates_checked, fh_updates_failed;
static bool fh_remote = true;		/* the flathub remote is set up */
static bool fh_flatpak = true;		/* flatpak is on the phone */
static bool metered;			/* on mobile data: nothing big unasked */

static char query[128];
static bool typing;
static int filter;			/* 0 all, 1 repos, 2 AUR, 3 Flathub */
static struct vec found_repo, found_aur, found_fh, found_other;
static bool searching, aur_failed, other_failed;
static int search_gen;
static int debounce_fd = -1;

static char list_title[160];
static struct vec list_items;

static struct pkg *shown;		/* the package on the details page */
static bool details_loading;
static struct fh_detail *detail;	/* and Flathub's details of it */
static bool detail_remote_loading;	/* flatpak is asked for sizes and permissions */
static int shot_base;			/* the screenshot pages show shots from here */
static int link_n;			/* the links drawn on the details page */
static const char *links[8];

static char update_status[200];
static struct vec update_lines;		/* "name old -> new" from ztos-update */
static struct vec aur_updates;
static bool aur_updates_loading, aur_updates_checked;

/* every package drawn as a row, per page, so a tap's arg can name it. Per
 * page because the core draws pages other than the one in front (the pivot's
 * neighbours, the page a back swipe reveals, cached page pictures): with one
 * table, drawing the top page behind the search results made a tap on the
 * first result open the top page's first app instead. */
static struct pkg *rows[P_COUNT][ZMC_MAX_HITS];
static int n_rows[P_COUNT];
static _Thread_local int drawing = P_TOP;	/* the page render() is drawing (also on the core's page thread) */

static int nav[16], n_nav;		/* the pages to go back to */

static int add_row(struct pkg *p) {
	if (n_rows[drawing] >= ZMC_MAX_HITS) {
		return -1;
	}
	rows[drawing][n_rows[drawing]] = p;
	return n_rows[drawing]++;
}

/* The data under the rows is about to change: no tap may name an old row. */
static void rows_forget(void) {
	memset(n_rows, 0, sizeof(n_rows));
}

static void go(int page) {
	if (n_nav < 16) {
		nav[n_nav++] = zmc.page;
	}
	zmc_go(page, 1);
}

/* ---- drawing helpers -------------------------------------------------------------- */

static void grey(cairo_t *cr) {
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
}

static void white(cairo_t *cr) {
	cairo_set_source_rgb(cr, 1, 1, 1);
}

/* A page title: the core's own where it fits, else smaller and wrapped. */
static double page_title(cairo_t *cr, const char *title) {
	double room = zmc.width - 2 * MARGIN;
	if (zmc_text_width(title, FONT_SEMILIGHT, 48) <= room) {
		return zmc_header(cr, title);
	}
	white(cr);
	zmc_text(cr, zmc.app->caps, FONT_SEMIBOLD, 11.5, MARGIN, 14, 0);
	double h = zmc_text(cr, title, FONT_SEMILIGHT, 30, MARGIN - 2, 32, room);
	zmc_add_hit(0, 0, zmc.width, 32 + h, ZMC_ACT_BACK, 0);
	return 32 + h + 22;
}

static double heading(cairo_t *cr, double y, const char *text, const char *caption) {
	white(cr);
	y += zmc_text(cr, text, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN - 70) + 2;
	if (caption) {
		grey(cr);
		zmc_text_line(cr, caption, FONT_REGULAR, 12, MARGIN, y, zmc.width - 2 * MARGIN);
		y += 22;
	}
	return y + 4;
}

static double note(cairo_t *cr, double y, const char *text) {
	grey(cr);
	return y + zmc_text(cr, text, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 18;
}

/* Soft placeholders the size of the real thing, drawn while a list is still
 * loading, so the page keeps its shape and nothing pops in or jumps about as
 * the cached lists and then the network lists arrive. */
static void skel_box(cairo_t *cr, double x, double y, double w, double h) {
	cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static double skel_row(cairo_t *cr, double y) {
	double x = MARGIN + 68, w = zmc.width - x - MARGIN;
	skel_box(cr, MARGIN, y, 56, 56);		/* the tile */
	skel_box(cr, x, y, w * 0.55, 16);		/* the name */
	skel_box(cr, x, y + 25, w * 0.85, 12);		/* the line about it */
	skel_box(cr, x, y + 45, 54, 13);		/* the source badge */
	return y + 76;
}

/* the phone's plain button: a white outline with its label in the middle */
static double button(cairo_t *cr, double x, double y, double w, const char *label, int action, int arg) {
	white(cr);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, x + 1, y + 1, w - 2, 44);
	cairo_stroke(cr);
	zmc_text_centred(cr, label, FONT_SEMILIGHT, 17, x + w / 2, y + 11);
	zmc_add_hit(x, y - 4, w, 54, action, arg);
	return y + 46;
}

static struct pkg *fh_update_of(const char *id) {
	for (int i = 0; i < fh_updates.n; i++) {
		struct pkg *u = fh_updates.v[i];
		if (!strcmp(u->name, id)) {
			return u;
		}
	}
	return NULL;
}

static bool has_update(const struct pkg *p) {
	if (pkg_is_flathub(p)) {
		/* flatpak knows by its commits: versions may not change at all */
		return p->installed && fh_update_of(p->name);
	}
	return p->installed && p->installed_version && p->version && vercmp(p->version, p->installed_version) > 0;
}

static void meta_of(const struct pkg *p, char *out, size_t size) {
	if (has_update(p)) {
		struct pkg *u = pkg_is_flathub(p) ? fh_update_of(p->name) : NULL;
		const char *to = u && u->version ? u->version : p->version;
		if (p->installed_version && to && strcmp(p->installed_version, to)) {
			snprintf(out, size, "%s → %s", p->installed_version, to);
		} else {
			snprintf(out, size, "update ready");
		}
	} else if (p->installed) {
		snprintf(out, size, "installed");
	} else if (pkg_is_flathub(p) && p->arch) {
		snprintf(out, size, "not built for this phone (%s only)", p->arch);
	} else if (pkg_is_flathub(p)) {
		snprintf(out, size, "%s%s%s", p->verified ? "verified · " : "", p->mobile ? "fits a phone · " : "",
			p->packager ? p->packager : "");
	} else if (pkg_is_aur(p)) {
		snprintf(out, size, "%.0f vote%s", p->votes, p->votes == 1 ? "" : "s");
	} else if (p->csize > 0) {
		char s[32];
		human_size(s, sizeof(s), p->csize);
		snprintf(out, size, "%s download", s);
	} else {
		out[0] = 0;
	}
}

/* One package: its tile, its name, a line about it, and where it is from. */
static double row(cairo_t *cr, double y, struct pkg *p) {
	int idx = add_row(p);
	if (idx < 0) {
		return y;
	}
	double x = MARGIN + 68, w = zmc.width - x - MARGIN;
	draw_tile(cr, MARGIN, y, 56, p);
	white(cr);
	zmc_text_line(cr, pkg_title(p), FONT_SEMILIGHT, 20, x, y - 3, w);
	cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
	zmc_text_line(cr, pkg_blurb(p), FONT_REGULAR, 13, x, y + 23, w);
	/* the source as a small outlined label, then a fact */
	char badge[48];
	snprintf(badge, sizeof(badge), "%s", p->repo ? p->repo : "");
	for (char *c = badge; *c; c++) {
		*c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
	}
	double bw = zmc_text_width(badge, FONT_SEMIBOLD, 9.5) + 8;
	grey(cr);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 0.5, y + 43.5, bw, 14);
	cairo_stroke(cr);
	zmc_text(cr, badge, FONT_SEMIBOLD, 9.5, x + 4, y + 44, 0);
	char meta[128];
	meta_of(p, meta, sizeof(meta));
	zmc_text_line(cr, meta, FONT_REGULAR, 11.5, x + bw + 8, y + 43, w - bw - 8);
	zmc_add_hit(0, y - 6, zmc.width, 70, ACT_PKG, idx);
	return y + 76;
}

/* ---- loading what the pages show --------------------------------------------------- */

struct load {
	struct repo_index *ri;
	struct flathub *fh;		/* NULL: none (or, in the second half, unchanged) */
	struct vec sections, cats, groups, mine, fh_cats;
	int apps;
	bool fh_remote, fh_flatpak, metered;
	char note[200];
};

static int by_title(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return strcasecmp(pkg_title(x), pkg_title(y));
}

static void read_sections(struct load *l) {
	char path[4096];
	sections_path(path, sizeof(path));
	if (!sections_read(&l->sections, path)) {
		snprintf(l->note, sizeof(l->note), "No lists set up: %s is missing.", path);
	}
}

static void apply_load(void *arg);

static const char *fh_dir(void) {
	/* (can be pointed elsewhere, to try it on a copy) */
	return getenv("ZTSTORE_FLATHUB") ? getenv("ZTSTORE_FLATHUB") : FLATHUB_AS_DIR;
}

/* Flathub's catalogue, with what of it is installed marked; the installed
 * Flathub apps go on `mine` too (as the catalogue knows them, else bare) */
static struct flathub *fh_read(struct vec *mine) {
	struct flathub *f = calloc(1, sizeof(*f));
	flathub_load(f, fh_dir());
	struct vec installed = { 0 };
	flathub_installed(&installed);
	for (int i = 0; i < installed.n; i++) {
		struct pkg *q = installed.v[i];
		struct pkg *p = flathub_find(f, q->name);
		if (p) {
			p->installed = true;
			set_str(&p->installed_version, q->installed_version);
			set_str(&p->desktop, q->desktop);
		}
		if (!mine) {
			continue;
		}
		struct pkg *c = p ? pkg_copy(p) : pkg_copy(q);
		if (!p && c->desktop) {
			/* not in the catalogue (another remote's, or gone from it): its
			 * name and icon from its .desktop file */
			char *text = read_all(c->desktop, NULL), *save = NULL;
			for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
				if (starts_with(line, "Name=") && !c->app_name) {
					set_str(&c->app_name, trim(line + 5));
				} else if (starts_with(line, "Icon=") && !c->icon) {
					char path[4096];
					snprintf(path, sizeof(path), "/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps/%s.png",
						trim(line + 5));
					if (access(path, R_OK) == 0) {
						set_str(&c->icon, path);
					}
				}
			}
			free(text);
		}
		vec_push(mine, c);
	}
	vec_free(&installed, pkg_free);
	return f;
}

/* What is on the phone already: its packages, and the lists from the copies
 * of the AUR dump, pkgstats and Flathub's lists it has (none on the first
 * start). */
static void load_local(struct load *l) {
	/* (both can be pointed elsewhere, to try it on a copy) */
	const char *conf = getenv("ZTSTORE_PACMAN_CONF") ? getenv("ZTSTORE_PACMAN_CONF") : PACMAN_CONF;
	const char *as_env[] = { getenv("ZTSTORE_APPSTREAM"), NULL };
	l->ri = calloc(1, sizeof(*l->ri));
	if (!repo_load(l->ri, conf)) {
		snprintf(l->note, sizeof(l->note), "No repos found in %s.", conf);
	}
	l->apps = appstream_apply(l->ri, as_env[0] ? as_env : APPSTREAM_ROOTS);
	facets_categories(l->ri, &l->cats, 12);
	facets_groups(l->ri, &l->groups, 12);
	/* on this phone: what the person chose, that is an app or came from elsewhere */
	for (int i = 0; i < l->ri->local.n; i++) {
		struct pkg *p = l->ri->local.v[i];
		struct pkg *s = repo_find_sync(l->ri, p->name);
		if (p->explicit_install && (p->desktop || !s)) {
			struct pkg *c = pkg_copy(p);
			set_str(&c->repo, s ? s->repo : "aur");
			vec_push(&l->mine, c);
		}
	}
	char *flatpak_argv[] = { "flatpak", "--version", NULL };
	int st;
	free(run_capture(flatpak_argv, &st));
	l->fh_flatpak = st == 0;
	l->fh_remote = l->fh_flatpak && flathub_remote_present();
	l->metered = network_metered();
	l->fh = fh_read(&l->mine);
	facets_flathub(&l->fh->apps, &l->fh_cats, 12);
	qsort(l->mine.v, l->mine.n, sizeof(void *), by_title);
	read_sections(l);
	char dump[4096], stats[4096];
	cache_path(dump, sizeof(dump), "aur-packages-meta-v1.json.gz");
	cache_path(stats, sizeof(stats), "pkgstats.json");
	sections_fill(&l->sections, l->ri, file_mtime(dump) >= 0 ? dump : NULL, file_mtime(stats) >= 0 ? stats : NULL);
	sections_fill_flathub(&l->sections, l->fh, -1);
}

static long catalogue_stamp(void) {
	return flathub_stamp(fh_dir());
}

static void load_work(void *arg) {
	struct load *l = arg;
	load_local(l);
	/* the page fills at once with that; this job goes on reading l->ri and
	 * l->fh, which the main loop keeps until the job is over (free_retired) */
	struct load *first = calloc(1, sizeof(*first));
	*first = *l;
	memset(l, 0, sizeof(*l));
	struct repo_index *ri_here = first->ri;
	struct flathub *fh_here = first->fh;
	bool remote = first->fh_remote, is_metered = first->metered;
	job_post(apply_load, first);
	/* then fresh copies from the network: the AUR's and pkgstats' once a day,
	 * Flathub's twice. On mobile data only what is missing altogether. */
	long never = LONG_MAX / 2;
	char dump[4096], stats[4096];
	cache_path(dump, sizeof(dump), "aur-packages-meta-v1.json.gz");
	bool got_dump = is_metered && file_mtime(dump) >= 0 ? true : aur_dump(dump, sizeof(dump));
	cache_path(stats, sizeof(stats), "pkgstats.json");
	bool got_stats = fetch_to_file(PKGSTATS_URL, stats, is_metered ? never : 86400);
	if (remote) {
		/* flatpak's own catalogue for this arch: when it is newer, read again */
		long before = catalogue_stamp();
		flathub_refresh(is_metered && before >= 0 ? never : FH_AS_MAX_AGE);
		long now = catalogue_stamp();
		if (now >= 0 && now != before) {
			l->fh = fh_read(NULL);
			fh_here = l->fh;
			facets_flathub(&l->fh->apps, &l->fh_cats, 12);
		}
	}
	read_sections(l);
	sections_fill(&l->sections, ri_here, got_dump ? dump : NULL, got_stats ? stats : NULL);
	sections_fill_flathub(&l->sections, fh_here, is_metered ? never : FH_API_MAX_AGE);
}

static void free_load(struct load *l) {
	if (l->ri) {
		repo_free(l->ri);
		free(l->ri);
	}
	if (l->fh) {
		flathub_free(l->fh);
		free(l->fh);
	}
	vec_free(&l->sections, section_free);
	vec_free(&l->cats, facet_free);
	vec_free(&l->groups, facet_free);
	vec_free(&l->mine, pkg_free);
	vec_free(&l->fh_cats, facet_free);
	free(l);
}

static void free_retired(void) {
	if (jobs_running() > 1) {
		return;	/* (this job counts until its done has run) */
	}
	for (int i = 0; i < retired.n; i++) {
		repo_free(retired.v[i]);
		free(retired.v[i]);
	}
	retired.n = 0;
	for (int i = 0; i < retired_fh.n; i++) {
		flathub_free(retired_fh.v[i]);
		free(retired_fh.v[i]);
	}
	retired_fh.n = 0;
}

static void start_load(void);

static void use_fh(struct load *l) {
	if (!l->fh) {
		return;
	}
	if (fh) {
		vec_push(&retired_fh, fh);
	}
	fh = l->fh;
	l->fh = NULL;
	vec_free(&fh_cats, facet_free);
	fh_cats = l->fh_cats;
	memset(&l->fh_cats, 0, sizeof(l->fh_cats));
}

/* the package on the details page, as it stands now */
static void shown_refresh(void) {
	if (!shown) {
		return;
	}
	if (pkg_is_flathub(shown)) {
		struct pkg *now = fh ? flathub_find(fh, shown->name) : NULL;
		if (now) {
			pkg_free(shown);
			shown = pkg_copy(now);
		} else if (!shown->arch) {
			/* not in the catalogue: installed or not, by flatpak's list */
			bool there = false;
			for (int i = 0; i < mine.n && !there; i++) {
				there = pkg_is_flathub(mine.v[i]) && !strcmp(((struct pkg *)mine.v[i])->name, shown->name);
			}
			shown->installed = there;
		}
	} else if (!pkg_is_aur(shown)) {
		struct pkg *now = repo_find_sync(ri, shown->name);
		if (now) {
			pkg_free(shown);
			shown = pkg_copy(now);
		} else {
			struct pkg *l2 = repo_find_local(ri, shown->name);
			shown->installed = l2 != NULL;
		}
	} else {
		struct pkg *l2 = repo_find_local(ri, shown->name);
		shown->installed = l2 != NULL;
		set_str(&shown->installed_version, l2 ? l2->version : NULL);
		set_str(&shown->desktop, l2 ? l2->desktop : NULL);
		if (l2 && l2->icon) {
			set_str(&shown->icon, l2->icon);
		}
	}
}

/* the first half: the phone's own packages and lists from cached copies */
static void apply_load(void *arg) {
	struct load *l = arg;
	rows_forget();
	if (ri) {
		vec_push(&retired, ri);
	}
	ri = l->ri;
	l->ri = NULL;
	use_fh(l);
	vec_free(&sections, section_free);
	vec_free(&cats, facet_free);
	vec_free(&groups, facet_free);
	vec_free(&mine, pkg_free);
	sections = l->sections;
	cats = l->cats;
	groups = l->groups;
	mine = l->mine;
	memset(&l->sections, 0, sizeof(l->sections));
	memset(&l->cats, 0, sizeof(l->cats));
	memset(&l->groups, 0, sizeof(l->groups));
	memset(&l->mine, 0, sizeof(l->mine));
	n_apps = l->apps;
	fh_remote = l->fh_remote;
	fh_flatpak = l->fh_flatpak;
	metered = l->metered;
	shot_set_metered(metered);
	snprintf(load_note, sizeof(load_note), "%s", l->note);
	free_load(l);
	shown_refresh();
	zmc_redraw();
}

/* the second half: the lists again, with fresh copies from the network */
static void load_done(void *arg) {
	struct load *l = arg;
	if (l->fh) {
		rows_forget();
		use_fh(l);	/* (what is installed is marked on it too: fh_read) */
		shown_refresh();
	}
	if (l->sections.n) {
		rows_forget();
		vec_free(&sections, section_free);
		sections = l->sections;
		memset(&l->sections, 0, sizeof(l->sections));
	}
	free_load(l);
	loading = false;
	zmc_busy(searching || inst.state == INST_RUNNING);
	free_retired();
	if (reload_wanted) {
		reload_wanted = false;
		start_load();
	}
	zmc_redraw();
}

static void start_load(void) {
	if (loading && ri) {
		reload_wanted = true;
		return;
	}
	loading = true;
	aur_updates_checked = false;
	fh_updates_checked = false;
	zmc_busy(true);
	job_run(load_work, load_done, calloc(1, sizeof(struct load)));
}

/* ---- search ---------------------------------------------------------------------- */

struct search {
	int gen;
	char term[128];
	struct repo_index *ri;
	struct flathub *fh;
	struct vec repo, aur, flathub, other;
	bool aur_ok, other_ok;
};

static void search_work(void *arg) {
	struct search *s = arg;
	search_repo(s->ri, s->term, 50, &s->repo);
	if (s->fh) {
		search_list(&s->fh->apps, s->term, 50, &s->flathub);
		/* what Flathub has only for other processors: said, not offered */
		s->other_ok = flathub_search_other_arch(s->fh, s->term, &s->other);
		while (s->other.n > 6) {
			pkg_free(s->other.v[--s->other.n]);
		}
	}
	s->aur_ok = aur_search(s->term, &s->aur);
	/* the AUR's best first: by popularity, as its own site sorts them */
	for (int i = 1; i < s->aur.n; i++) {
		for (int j = i; j > 0 && ((struct pkg *)s->aur.v[j])->popularity >
				((struct pkg *)s->aur.v[j - 1])->popularity; j--) {
			void *t = s->aur.v[j];
			s->aur.v[j] = s->aur.v[j - 1];
			s->aur.v[j - 1] = t;
		}
	}
	while (s->aur.n > 50) {
		pkg_free(s->aur.v[--s->aur.n]);
	}
	for (int i = 0; i < s->aur.n; i++) {
		struct pkg *p = s->aur.v[i];
		struct pkg *l = repo_find_local(s->ri, p->name);
		if (l) {
			p->installed = true;
			set_str(&p->installed_version, l->version);
			set_str(&p->icon, l->icon);
			set_str(&p->desktop, l->desktop);
		}
	}
}

static void search_done(void *arg) {
	struct search *s = arg;
	if (s->gen == search_gen) {
		rows_forget();
		vec_free(&found_repo, pkg_free);
		vec_free(&found_aur, pkg_free);
		vec_free(&found_fh, pkg_free);
		vec_free(&found_other, pkg_free);
		found_repo = s->repo;
		found_aur = s->aur;
		found_fh = s->flathub;
		found_other = s->other;
		memset(&s->repo, 0, sizeof(s->repo));
		memset(&s->aur, 0, sizeof(s->aur));
		memset(&s->flathub, 0, sizeof(s->flathub));
		memset(&s->other, 0, sizeof(s->other));
		aur_failed = !s->aur_ok;
		other_failed = !s->other_ok;
		searching = false;
		zmc_busy(loading || inst.state == INST_RUNNING);
		zmc_redraw();
	}
	vec_free(&s->repo, pkg_free);
	vec_free(&s->aur, pkg_free);
	vec_free(&s->flathub, pkg_free);
	vec_free(&s->other, pkg_free);
	free(s);
	free_retired();
}

static void search_now(void) {
	search_gen++;
	if (strlen(query) < 2 || !ri) {
		rows_forget();
		vec_free(&found_repo, pkg_free);
		vec_free(&found_aur, pkg_free);
		vec_free(&found_fh, pkg_free);
		vec_free(&found_other, pkg_free);
		searching = false;
		return;
	}
	struct search *s = calloc(1, sizeof(*s));
	s->gen = search_gen;
	snprintf(s->term, sizeof(s->term), "%s", query);
	s->ri = ri;
	s->fh = fh && fh_remote ? fh : NULL;
	searching = true;
	zmc_busy(true);
	job_run(search_work, search_done, s);
}

/* typing waits a moment before it searches, so each letter is not a search */
static void debounce_ready(int fd, void *data) {
	uint64_t n;
	while (read(fd, &n, sizeof(n)) > 0) {
	}
	search_now();
	zmc_redraw();
}

static void debounce(void) {
	if (debounce_fd < 0) {
		debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
		zmc_watch_fd(debounce_fd, debounce_ready, NULL);
	}
	struct itimerspec spec = { .it_value = { 0, 350 * 1000000L } };
	timerfd_settime(debounce_fd, 0, &spec, NULL);
}

/* ---- details and updates from the AUR -------------------------------------------- */

struct info {
	char name[128];
	struct vec got;
	struct vec names;	/* for updates: every installed AUR package */
	struct vec versions;	/* and the version installed */
};

static void info_work(void *arg) {
	struct info *in = arg;
	if (in->names.n) {
		aur_info((const char *const *)in->names.v, in->names.n, &in->got);
	} else {
		const char *one[] = { in->name };
		aur_info(one, 1, &in->got);
	}
}

static void info_free(struct info *in) {
	vec_free(&in->got, pkg_free);
	vec_free(&in->names, free);
	vec_free(&in->versions, free);
	free(in);
}

static void details_done(void *arg) {
	struct info *in = arg;
	details_loading = false;
	if (shown && in->got.n && !strcmp(shown->name, in->name)) {
		struct pkg *p = in->got.v[0];
		p->installed = shown->installed;
		set_str(&p->installed_version, shown->installed_version);
		set_str(&p->icon, shown->icon);
		set_str(&p->desktop, shown->desktop);
		pkg_free(shown);
		shown = p;
		in->got.v[0] = NULL;
	}
	info_free(in);
	free_retired();
	zmc_redraw();
}

static void aur_updates_done(void *arg) {
	struct info *in = arg;
	rows_forget();
	vec_free(&aur_updates, pkg_free);
	for (int i = 0; i < in->got.n; i++) {
		struct pkg *p = in->got.v[i];
		for (int k = 0; k < in->names.n; k++) {
			if (!strcmp(in->names.v[k], p->name) && vercmp(p->version, in->versions.v[k]) > 0) {
				p->installed = true;
				set_str(&p->installed_version, in->versions.v[k]);
				struct pkg *l = ri ? repo_find_local(ri, p->name) : NULL;
				if (l) {
					set_str(&p->icon, l->icon);
					set_str(&p->desktop, l->desktop);
				}
				vec_push(&aur_updates, p);
				in->got.v[i] = NULL;
				break;
			}
		}
	}
	aur_updates_loading = false;
	info_free(in);
	zmc_redraw();
}

static void check_aur_updates(void) {
	if (!ri || aur_updates_loading || aur_updates_checked) {
		return;
	}
	struct info *in = calloc(1, sizeof(*in));
	struct vec foreign = { 0 };
	repo_foreign(ri, &foreign);
	for (int i = 0; i < foreign.n; i++) {
		struct pkg *p = foreign.v[i];
		vec_push(&in->names, xstrdup(p->name));
		vec_push(&in->versions, xstrdup(p->version));
	}
	vec_free(&foreign, NULL);
	aur_updates_checked = true;
	if (!in->names.n) {
		info_free(in);
		return;
	}
	aur_updates_loading = true;
	job_run(info_work, aur_updates_done, in);
}

/* What ztos-update, the system's updater, has found: its status line and list. */
static void read_updates(void) {
	zmc_read_file(UPDATE_STATUS, update_status, sizeof(update_status));
	vec_free(&update_lines, free);
	char *text = read_all(UPDATE_LIST, NULL);
	if (text) {
		char *save = NULL;
		for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (*trim(l)) {
				vec_push(&update_lines, xstrdup(trim(l)));
			}
		}
		free(text);
	}
}

static void start_unit(const char *unit) {
	if (fork() == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("systemctl", "systemctl", "start", "--no-block", unit, (char *)NULL);
		_exit(127);
	}
}

/* ---- Flathub: details and updates ------------------------------------------------- */

struct fh_job {
	char id[256];
	struct flathub *fh;
	struct fh_detail *d;
	bool remote;		/* the second step: flatpak's sizes and permissions */
	struct vec updates;
	bool ok;
};

static void fh_detail_work(void *arg) {
	struct fh_job *j = arg;
	if (!j->remote) {
		/* out of the catalogue: quick (it is mapped, not read) */
		j->d = flathub_detail(j->fh, j->id, SHOT_WANT_PX);
	} else if (j->d) {
		flathub_remote_info(j->d);
	}
}

static void fh_detail_done(void *arg);

static void fh_detail_start(const char *id) {
	fh_detail_free(detail);
	detail = NULL;
	detail_remote_loading = false;
	if (!fh) {
		return;
	}
	struct fh_job *j = calloc(1, sizeof(*j));
	snprintf(j->id, sizeof(j->id), "%s", id);
	j->fh = fh;
	details_loading = true;
	job_run(fh_detail_work, fh_detail_done, j);
}

static void fh_detail_done(void *arg) {
	struct fh_job *j = arg;
	bool ours = shown && pkg_is_flathub(shown) && !strcmp(shown->name, j->id);
	if (!j->remote) {
		details_loading = false;
		if (ours && j->d) {
			fh_detail_free(detail);
			detail = j->d;
			/* now flatpak, which may take seconds: it reads the remote's summary.
			 * It works on its own copy and hands it back. */
			struct fh_job *k = calloc(1, sizeof(*k));
			snprintf(k->id, sizeof(k->id), "%s", j->id);
			k->d = calloc(1, sizeof(*k->d));
			k->d->id = xstrdup(j->id);
			k->d->runtime = xstrdup(detail->runtime);
			k->remote = true;
			detail_remote_loading = true;
			job_run(fh_detail_work, fh_detail_done, k);
		} else {
			fh_detail_free(j->d);
		}
	} else {
		if (ours && detail && j->d) {
			/* flatpak's answers onto the catalogue's details */
			struct fh_detail *d = j->d;
			detail->have_remote = d->have_remote;
			detail->download = d->download;
			detail->installed_size = d->installed_size;
			detail->remote_version = d->remote_version;
			detail->runtime_missing = d->runtime_missing;
			detail->x11_only = d->x11_only;
			detail->runtime_download = d->runtime_download;
			detail->perms = d->perms;
			d->download = d->installed_size = d->remote_version = d->runtime_download = NULL;
			memset(&d->perms, 0, sizeof(d->perms));
		}
		detail_remote_loading = false;
		fh_detail_free(j->d);
	}
	free(j);
	free_retired();
	zmc_redraw();
}

static void fh_updates_work(void *arg) {
	struct fh_job *j = arg;
	j->ok = flathub_updates(&j->updates);
}

static void fh_updates_done(void *arg) {
	struct fh_job *j = arg;
	rows_forget();
	vec_free(&fh_updates, pkg_free);
	fh_updates = j->updates;
	fh_updates_failed = !j->ok;
	/* as rows: the catalogue's names and icons */
	for (int i = 0; i < fh_updates.n; i++) {
		struct pkg *u = fh_updates.v[i];
		struct pkg *p = fh ? flathub_find(fh, u->name) : NULL;
		if (p) {
			set_str(&u->app_name, p->app_name);
			set_str(&u->summary, p->summary);
			set_str(&u->icon, p->icon);
			set_str(&u->installed_version, p->installed_version);
		}
		u->installed = true;
	}
	fh_updates_loading = false;
	free(j);
	zmc_redraw();
}

static void check_fh_updates(void) {
	if (!fh_remote || fh_updates_loading || fh_updates_checked) {
		return;
	}
	bool any = false;
	for (int i = 0; i < mine.n && !any; i++) {
		any = pkg_is_flathub(mine.v[i]);
	}
	fh_updates_checked = true;
	if (!any) {
		return;
	}
	fh_updates_loading = true;
	job_run(fh_updates_work, fh_updates_done, calloc(1, sizeof(struct fh_job)));
}

/* ---- the pages ---------------------------------------------------------------------- */

static struct pkg *spotlight(int *from) {
	for (int i = 0; i < sections.n; i++) {
		struct section *s = sections.v[i];
		if (s->items.n) {
			*from = i;
			return s->items.v[0];
		}
	}
	return NULL;
}

static double tab_header(cairo_t *cr, int tab) {
	static const struct zmc_tab tabs[] = { { "top", P_TOP }, { "categories", P_CATS }, { "installed", P_INSTALLED } };
	return zmc_pivot_wheel(cr, tabs, 3, tab);
}

static double draw_top(cairo_t *cr) {
	double y = tab_header(cr, P_TOP);
	y = zmc_textbox(cr, y - 8, fh_remote ? "search your repos, the AUR and Flathub" : "search your repos and the AUR",
		"", false, ACT_SEARCHBOX, 0);
	if (load_note[0]) {
		y = note(cr, y, load_note);
	}
	if (fh_flatpak && !fh_remote && !loading) {
		/* asked once, here: nothing is added without a yes */
		double bw = zmc.width - 2 * MARGIN;
		y = heading(cr, y, "add Flathub?", NULL);
		grey(cr);
		y += zmc_text(cr, "Flathub has thousands of apps, packed with what they need and kept apart from the "
			"system (Flatpak). Adding it lets the App Store show and install them. It adds Flathub's address "
			"and signing key (flathub.org's official file) to the phone's Flatpak, for everyone on it.",
			FONT_REGULAR, 13.5, MARGIN, y, bw) + 12;
		y = button(cr, MARGIN, y, bw, "add Flathub", ACT_ADD_FLATHUB, 0) + 26;
	}
	if (!sections.n) {
		if (!loading) {
			return note(cr, y, "No lists to show.");
		}
		/* nothing read yet: a skeleton the shape of the page to come */
		skel_box(cr, MARGIN, y, zmc.width - 2 * MARGIN, 150);
		y += 150 + 26;
		for (int s = 0; s < 2; s++) {
			skel_box(cr, MARGIN, y, 180, 20);
			y += 34;
			for (int k = 0; k < ROWS_SHOWN; k++) {
				y = skel_row(cr, y);
			}
			y += 14;
		}
		return y + 20;
	}
	/* the first list's first package, large */
	int from = -1;
	struct pkg *spot = spotlight(&from);
	if (!spot && loading) {
		skel_box(cr, MARGIN, y, zmc.width - 2 * MARGIN, 150);
		y += 150 + 26;
	}
	if (spot) {
		struct section *s = sections.v[from];
		double w = zmc.width - 2 * MARGIN, h = 150;
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, y, w, h);
		cairo_fill(cr);
		draw_tile_shaded(cr, MARGIN + w - 76, y + 14, 62, spot);
		white(cr);
		char caps[200];
		snprintf(caps, sizeof(caps), "TOP OF “%s”", s->title);
		for (char *c = caps; *c; c++) {
			*c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
		}
		zmc_text_line(cr, caps, FONT_SEMIBOLD, 10.5, MARGIN + 14, y + 14, w - 104);
		zmc_text_line(cr, pkg_title(spot), FONT_LIGHT, 30, MARGIN + 14, y + 66, w - 28);
		zmc_text_line(cr, pkg_blurb(spot), FONT_REGULAR, 13, MARGIN + 14, y + 112, w - 28);
		int idx = add_row(spot);
		if (idx >= 0) {
			zmc_add_hit(MARGIN, y, w, h, ACT_PKG, idx);
		}
		y += h + 26;
	}
	for (int i = 0; i < sections.n; i++) {
		struct section *s = sections.v[i];
		int first = (i == from) ? 1 : 0;	/* (the spotlight is already up there) */
		if (s->items.n > first) {
			grey(cr);
			double sw = zmc_text_width("see all", FONT_REGULAR, 14);
			zmc_text(cr, "see all", FONT_REGULAR, 14, zmc.width - MARGIN - sw, y + 8, 0);
			zmc_add_hit(zmc.width - MARGIN - sw - 16, y - 4, sw + 32, 40, ACT_SEE_ALL, i);
		}
		y = heading(cr, y, s->title, s->caption);
		if (!s->items.n) {
			if (loading && !s->note) {
				for (int k = 0; k < ROWS_SHOWN; k++) {
					y = skel_row(cr, y);
				}
				y += 14;
			} else {
				y = note(cr, y, s->note ? s->note : "Nothing here yet.");
			}
			continue;
		}
		for (int k = first; k < s->items.n && k < first + ROWS_SHOWN; k++) {
			y = row(cr, y, s->items.v[k]);
		}
		y += 14;
	}
	return y + 20;
}

/* categories as squares, three across */
static double cat_grid(cairo_t *cr, double y, const struct vec *facets, int action) {
	double gap = 10, cell = (zmc.width - 2 * MARGIN - 2 * gap) / 3;
	for (int i = 0; i < facets->n; i++) {
		struct facet *f = facets->v[i];
		double x = MARGIN + (i % 3) * (cell + gap), ty = y + (i / 3) * (cell + gap);
		zmc_accent(cr);
		cairo_rectangle(cr, x, ty, cell, cell);
		cairo_fill(cr);
		white(cr);
		char count[16];
		snprintf(count, sizeof(count), "%d", f->count);
		double cw = zmc_text_width(count, FONT_REGULAR, 11);
		zmc_text(cr, count, FONT_REGULAR, 11, x + cell - cw - 8, ty + 6, 0);
		zmc_text(cr, f->label, FONT_REGULAR, 13.5, x + 8, ty + cell - 44, cell - 14);
		zmc_add_hit(x, ty, cell, cell, action, i);
	}
	return y + ((facets->n + 2) / 3) * (cell + gap) + 24;
}

static double draw_cats(cairo_t *cr) {
	double y = tab_header(cr, P_CATS);
	y = heading(cr, y, "apps", n_apps ? "AppStream categories · your repos" : NULL);
	if (!cats.n) {
		y = note(cr, y, loading ? "Reading your repos…" :
			"No app data on this phone yet. Installing archlinux-appstream-data adds names, icons and categories.");
	}
	y = cat_grid(cr, y, &cats, ACT_CAT);
	if (fh_flatpak) {
		char cap[96];
		snprintf(cap, sizeof(cap), "%d apps built for this phone", fh ? fh->apps.n : 0);
		y = heading(cr, y, "Flathub", fh && fh->apps.n ? cap : NULL);
		if (!fh_remote) {
			y = note(cr, y, "Flathub is not set up: see the top page.");
		} else if (!fh_cats.n) {
			y = note(cr, y, loading ? "Reading Flathub's catalogue…" :
				"Flathub's catalogue is not on this phone yet. It comes with the next refresh.");
		}
		y = cat_grid(cr, y, &fh_cats, ACT_FH_CAT);
	}
	if (groups.n) {
		y = heading(cr, y, "package groups", "whole sets that install together");
		for (int i = 0; i < groups.n; i++) {
			struct facet *f = groups.v[i];
			char detail[48];
			snprintf(detail, sizeof(detail), "%d package%s", f->count, f->count == 1 ? "" : "s");
			y = zmc_entry(cr, y, f->label, detail, ACT_GROUP, i);
		}
	}
	return y + 20;
}

static double draw_installed(cairo_t *cr) {
	double y = tab_header(cr, P_INSTALLED);
	/* the system's updates come from ztos-update: Arch updates everything at
	 * once, never one package on its own */
	y = heading(cr, y, "updates", "your repos, through the system updater");
	const char *s = update_status;
	if (starts_with(s, "available")) {
		char line[96];
		snprintf(line, sizeof(line), "%s ready to install", s + 10);
		white(cr);
		y += zmc_text(cr, line, FONT_REGULAR, 16, MARGIN, y, 0) + 8;
		grey(cr);
		for (int i = 0; i < update_lines.n && i < 8; i++) {
			zmc_text_line(cr, update_lines.v[i], FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN);
			y += 20;
		}
		if (update_lines.n > 8) {
			char more[48];
			snprintf(more, sizeof(more), "and %d more", update_lines.n - 8);
			zmc_text(cr, more, FONT_REGULAR, 13, MARGIN, y, 0);
			y += 20;
		}
		y = button(cr, MARGIN, y + 10, zmc.width - 2 * MARGIN, "update all", ACT_UPDATE_ALL, 0) + 18;
	} else {
		const char *say = "Not checked yet.";
		char buf[200];
		if (starts_with(s, "checking")) {
			say = "Checking…";
		} else if (starts_with(s, "uptodate")) {
			say = "Everything is up to date.";
		} else if (starts_with(s, "downloading") || starts_with(s, "installing")) {
			say = "Updating now…";
		} else if (starts_with(s, "done")) {
			say = "Updated.";
		} else if (starts_with(s, "failed")) {
			snprintf(buf, sizeof(buf), "That did not work: %s", s + 7);
			say = buf;
		}
		y = note(cr, y, say);
		if (!starts_with(s, "checking") && !starts_with(s, "downloading") && !starts_with(s, "installing")) {
			y = button(cr, MARGIN, y, zmc.width - 2 * MARGIN, "check for updates", ACT_CHECK_UPDATES, 0) + 18;
		}
	}
	if (aur_updates.n || aur_updates_loading) {
		y = heading(cr, y, "from the AUR", "built again on this phone, one at a time");
		if (aur_updates_loading) {
			y = note(cr, y, "Asking the AUR…");
		}
		for (int i = 0; i < aur_updates.n; i++) {
			y = row(cr, y, aur_updates.v[i]);
		}
		y += 10;
	}
	if (fh_updates.n || fh_updates_loading || fh_updates_failed) {
		/* Flatpak apps update one by one, apart from the system */
		y = heading(cr, y, "from Flathub", "each on its own, apart from the system");
		if (fh_updates_loading) {
			y = note(cr, y, "Asking Flathub…");
		} else if (fh_updates_failed) {
			y = note(cr, y, "Could not ask Flathub for updates.");
		}
		for (int i = 0; i < fh_updates.n; i++) {
			y = row(cr, y, fh_updates.v[i]);
		}
		if (fh_updates.n > 1 && inst.state != INST_RUNNING) {
			y = button(cr, MARGIN, y, zmc.width - 2 * MARGIN, "update them all", ACT_FH_UPDATE_ALL, 0) + 18;
		}
		y += 10;
	}
	y = heading(cr, y, "on this phone", "apps you installed");
	if (!mine.n) {
		y = note(cr, y, loading ? "Reading…" : "Nothing yet.");
	}
	for (int i = 0; i < mine.n && i < LIST_MAX; i++) {
		y = row(cr, y, mine.v[i]);
	}
	return y + 20;
}

static double draw_search(cairo_t *cr) {
	double y = zmc_header(cr, "search");
	y = zmc_textbox(cr, y - 8, fh_remote ? "your repos, the AUR and Flathub" : "your repos and the AUR", query, typing,
		ACT_SEARCHBOX, 0);
	/* which to show: all, the repos, the AUR, Flathub, with how many each found */
	const char *names[] = { "all", "repos", "AUR", "Flathub" };
	int counts[] = { found_repo.n + found_aur.n + found_fh.n, found_repo.n, found_aur.n, found_fh.n };
	int n_filters = fh_remote ? 4 : 3;
	double x = MARGIN, fs = 17;
	for (int i = 0; i < n_filters; i++) {
		char label[32];
		if (strlen(query) >= 2 && !searching) {
			snprintf(label, sizeof(label), "%s %d", names[i], counts[i]);
		} else {
			snprintf(label, sizeof(label), "%s", names[i]);
		}
		if (i == filter) {
			white(cr);
		} else {
			cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
		}
		zmc_text(cr, label, FONT_SEMILIGHT, fs, x, y, 0);
		double w = zmc_text_width(label, FONT_SEMILIGHT, fs);
		zmc_add_hit(x - 8, y - 8, w + 16, 44, ACT_FILTER, i);
		x += w + 18;
	}
	y += 44;
	if (strlen(query) < 2) {
		return note(cr, y, "Type two letters or more.");
	}
	if (searching && !found_repo.n && !found_aur.n && !found_fh.n) {
		return note(cr, y, "Searching…");
	}
	/* (in "all", each source's best: the page has room for so many rows) */
	int cap = filter == 0 ? 25 : 50;
	if (filter == 0 || filter == 1) {
		if (filter == 0) {
			y = heading(cr, y, "from your repos", NULL);
		}
		if (!found_repo.n) {
			y = note(cr, y, "Nothing in your repos.");
		}
		for (int i = 0; i < found_repo.n && i < cap; i++) {
			y = row(cr, y, found_repo.v[i]);
		}
		y += 10;
	}
	if (filter == 0 || filter == 2) {
		if (filter == 0) {
			y = heading(cr, y, "from the AUR", NULL);
		}
		if (aur_failed) {
			y = note(cr, y, "Could not reach the AUR.");
		} else if (!found_aur.n) {
			y = note(cr, y, "Nothing in the AUR.");
		}
		for (int i = 0; i < found_aur.n && i < cap; i++) {
			y = row(cr, y, found_aur.v[i]);
		}
		y += 10;
	}
	if (fh_remote && (filter == 0 || filter == 3)) {
		if (filter == 0) {
			y = heading(cr, y, "from Flathub", NULL);
		}
		if (!fh || !fh->apps.n) {
			y = note(cr, y, "Flathub's catalogue is not on this phone yet.");
		} else if (!found_fh.n) {
			y = note(cr, y, "Nothing on Flathub for this phone.");
		}
		for (int i = 0; i < found_fh.n && i < cap; i++) {
			y = row(cr, y, found_fh.v[i]);
		}
		/* there, but not for this phone: said plainly, not offered */
		if (found_other.n) {
			y += 8;
			grey(cr);
			y += zmc_text(cr, "On Flathub, but not built for this phone (it needs an aarch64 build):",
				FONT_REGULAR, 13.5, MARGIN, y, zmc.width - 2 * MARGIN) + 8;
			for (int i = 0; i < found_other.n; i++) {
				struct pkg *p = found_other.v[i];
				char line[300];
				snprintf(line, sizeof(line), "%s · %s only", pkg_title(p), p->arch);
				cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
				zmc_text_line(cr, line, FONT_REGULAR, 15, MARGIN, y, zmc.width - 2 * MARGIN);
				grey(cr);
				zmc_text_line(cr, p->summary ? p->summary : p->name, FONT_REGULAR, 12, MARGIN, y + 20,
					zmc.width - 2 * MARGIN);
				y += 44;
			}
		} else if (other_failed && !searching) {
			y = note(cr, y, "(flathub.org could not be asked which apps exist only for other phones.)");
		}
	}
	return y + 20;
}

static double draw_list(cairo_t *cr) {
	double y = page_title(cr, list_title);
	if (!list_items.n) {
		return note(cr, y, "Nothing here.");
	}
	for (int i = 0; i < list_items.n && i < LIST_MAX; i++) {
		y = row(cr, y, list_items.v[i]);
	}
	if (list_items.n > LIST_MAX) {
		char more[96];
		snprintf(more, sizeof(more), "and %d more: search to find them.", list_items.n - LIST_MAX);
		y = note(cr, y, more);
	}
	return y + 20;
}

/* ---- Flathub's details ------------------------------------------------------------ */

static void amber(cairo_t *cr) {
	cairo_set_source_rgb(cr, 0.98, 0.70, 0.25);
}

/* a screenshot fitted into w x h, centred; a quiet box while it comes */
static void draw_shot(cairo_t *cr, const struct fh_shot *s, double x, double y, double w, double h) {
	enum shot_state state;
	cairo_surface_t *img = shot_get(s->url, &state);
	if (!img) {
		skel_box(cr, x, y, w, h);
		grey(cr);
		const char *say = state == SHOT_FAILED ? "could not load" : state == SHOT_HELD ? "not loaded (mobile data)" :
			"loading…";
		zmc_text_centred(cr, say, FONT_REGULAR, 11.5, x + w / 2, y + h / 2 - 8);
		return;
	}
	double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
	double k = w / iw < h / ih ? w / iw : h / ih;
	double dw = iw * k, dh = ih * k;
	cairo_save(cr);
	cairo_translate(cr, x + (w - dw) / 2, y + (h - dh) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static double shot_cell_h(const struct fh_shot *s, double w) {
	double h = s->w > 0 && s->h > 0 ? w * s->h / s->w : w * 0.625;
	return h > w * 1.8 ? w * 1.8 : h;
}

static double link_field(cairo_t *cr, double y, const char *caption, const char *url) {
	if (!url || link_n >= (int)(sizeof(links) / sizeof(links[0]))) {
		return y;
	}
	double top = y;
	links[link_n] = url;
	y = zmc_field(cr, y, caption, url);
	zmc_add_hit(0, top - 4, zmc.width, y - top, ACT_LINK, link_n++);
	return y;
}

static void date_of(long when, char *out, size_t size) {
	time_t t = when;
	struct tm tm;
	if (when <= 0 || !gmtime_r(&t, &tm)) {
		out[0] = 0;
		return;
	}
	strftime(out, size, "%-d %B %Y", &tm);
}

static double draw_detail_flathub(cairo_t *cr, struct pkg *p) {
	struct fh_detail *d = detail && !strcmp(detail->id, p->name) ? detail : NULL;
	link_n = 0;
	double y = page_title(cr, pkg_title(p));
	double x = MARGIN + 110, w = zmc.width - x - MARGIN, bw = zmc.width - 2 * MARGIN, half = (bw - 12) / 2;
	draw_tile(cr, MARGIN, y, 96, p);
	white(cr);
	zmc_text_line(cr, p->name, FONT_SEMILIGHT, 16, x, y, w);
	grey(cr);
	char line[300];
	snprintf(line, sizeof(line), "Flathub · %s", p->packager ? p->packager : d && d->developer ? d->developer : "");
	zmc_text_line(cr, line, FONT_REGULAR, 13, x, y + 26, w);
	snprintf(line, sizeof(line), "version %s", p->version ? p->version : d && d->remote_version ? d->remote_version : "?");
	if (p->installed && p->installed_version && strcmp(p->installed_version, p->version ? p->version : "")) {
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " (you have %s)", p->installed_version);
	}
	zmc_text_line(cr, line, FONT_REGULAR, 13, x, y + 46, w);
	if (p->verified || p->mobile) {
		zmc_accent(cr);
		snprintf(line, sizeof(line), "%s%s%s", p->verified ? "✓ verified developer" : "",
			p->verified && p->mobile ? " · " : "", p->mobile ? "fits a phone" : "");
		zmc_text_line(cr, line, FONT_SEMIBOLD, 12, x, y + 68, w);
	}
	y += 116;

	/* what can be done with it */
	if (p->arch) {
		white(cr);
		snprintf(line, sizeof(line), "Not built for this phone. Flathub has it for %s only; this phone needs an "
			FLATHUB_ARCH " build, which its makers have not published.", p->arch);
		y += zmc_text(cr, line, FONT_REGULAR, 15, MARGIN, y, bw) + 18;
	} else if (!fh_remote) {
		y = button(cr, MARGIN, y, bw, "add Flathub first", ACT_ADD_FLATHUB, 0);
	} else if (inst.state == INST_RUNNING && !strcmp(inst.name, p->name)) {
		y = button(cr, MARGIN, y, bw, inst.removing ? "removing…" : "installing…", ACT_PROGRESS, 0);
	} else if (p->installed) {
		if (has_update(p)) {
			y = button(cr, MARGIN, y, bw, "update", ACT_UPDATE, 0) + 12;
		}
		if (p->desktop) {
			button(cr, MARGIN, y, half, "open", ACT_OPEN, 0);
			y = button(cr, MARGIN + half + 12, y, half, "remove", ACT_REMOVE, 0);
		} else {
			y = button(cr, MARGIN, y, bw, "remove", ACT_REMOVE, 0);
		}
	} else {
		y = button(cr, MARGIN, y, bw, "install", ACT_INSTALL, 0);
		if (d && d->x11_only) {
			amber(cr);
			y += zmc_text(cr, "⚠ It draws only through X11, which this phone does not run: it would not open.",
				FONT_REGULAR, 13, MARGIN, y + 8, bw) + 8;
		}
		if (d && d->runtime_missing && d->runtime) {
			/* the honest part: the first app on a runtime brings the runtime */
			char rt[200];
			snprintf(rt, sizeof(rt), "%s", d->runtime);
			char *slash = strchr(rt, '/');
			if (slash) {
				*slash = 0;
			}
			grey(cr);
			snprintf(line, sizeof(line), "It also brings %s%s%s%s, which other apps can share.", rt,
				d->runtime_download ? " (" : "", d->runtime_download ? d->runtime_download : "",
				d->runtime_download ? " to download)" : "");
			y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y + 8, bw) + 8;
		}
	}
	y += 22;
	white(cr);
	if (p->summary) {
		y += zmc_text(cr, p->summary, FONT_SEMILIGHT, 18, MARGIN, y, bw) + 14;
	}
	if (details_loading && !d) {
		y = note(cr, y, "Getting the details…");
	}

	/* every screenshot, two across; a tap shows them large, to swipe through */
	if (d && d->shots.n) {
		char cap[64];
		snprintf(cap, sizeof(cap), "%d · tap one to see them large", d->shots.n);
		y = heading(cr, y, "screenshots", cap);
		bool held = metered;
		if (held) {
			enum shot_state st;
			shot_get(((struct fh_shot *)d->shots.v[0])->url, &st);
			held = st == SHOT_HELD;
		}
		if (held) {
			grey(cr);
			y += zmc_text(cr, "On mobile data, screenshots are not fetched by themselves.", FONT_REGULAR, 13.5,
				MARGIN, y, bw) + 10;
			y = button(cr, MARGIN, y, bw, "show screenshots", ACT_SHOTS_LOAD, 0) + 20;
		} else {
			double gap = 10, cw = (bw - gap) / 2;
			for (int i = 0; i < d->shots.n; i += 2) {
				double row_h = 0;
				for (int k = i; k < i + 2 && k < d->shots.n; k++) {
					struct fh_shot *s = d->shots.v[k];
					double h = shot_cell_h(s, cw), cx = MARGIN + (k - i) * (cw + gap);
					draw_shot(cr, s, cx, y, cw, h);
					double th = 0;
					if (s->caption) {
						grey(cr);
						th = zmc_text(cr, s->caption, FONT_REGULAR, 11.5, cx, y + h + 4, cw) + 4;
					}
					zmc_add_hit(cx, y, cw, h + th, ACT_SHOT, k);
					row_h = h + th > row_h ? h + th : row_h;
				}
				y += row_h + gap + 4;
			}
			y += 12;
		}
	}

	/* the whole description */
	if (d && d->description && *d->description) {
		white(cr);
		y += zmc_text(cr, d->description, FONT_REGULAR, 15, MARGIN, y, bw) + 22;
	}

	/* what is new */
	if (d && d->releases.n) {
		struct fh_release *r = d->releases.v[0];
		char when[64], cap[128];
		date_of(r->when, when, sizeof(when));
		snprintf(cap, sizeof(cap), "%s%s", when[0] ? "released " : "", when);
		snprintf(line, sizeof(line), "what's new in %s", r->version);
		y = heading(cr, y, line, when[0] ? cap : NULL);
		cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
		y += zmc_text(cr, r->notes ? r->notes : "No notes for this release.", FONT_REGULAR, 14, MARGIN, y, bw) + 22;
		for (int i = 1; i < d->releases.n && i < 3; i++) {
			struct fh_release *o = d->releases.v[i];
			if (!o->notes) {
				continue;
			}
			date_of(o->when, when, sizeof(when));
			snprintf(line, sizeof(line), "%s%s%s", o->version, when[0] ? " · " : "", when);
			white(cr);
			y += zmc_text(cr, line, FONT_SEMIBOLD, 12.5, MARGIN, y, bw) + 4;
			grey(cr);
			y += zmc_text(cr, o->notes, FONT_REGULAR, 13, MARGIN, y, bw) + 16;
		}
	}

	/* what it may reach */
	if (!p->arch) {
		y = heading(cr, y, "permissions", "what the app asks to reach, from its manifest");
		if (d && d->perms.n) {
			bool any_warn = false;
			for (int i = 0; i < d->perms.n; i++) {
				struct fh_perm *m = d->perms.v[i];
				any_warn = any_warn || m->warn;
				if (m->warn) {
					amber(cr);
				} else {
					cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
				}
				snprintf(line, sizeof(line), "%s %s", m->warn ? "⚠" : "•", m->text);
				y += zmc_text(cr, line, FONT_REGULAR, 14, MARGIN, y, bw) + 6;
			}
			if (any_warn) {
				amber(cr);
				y += zmc_text(cr, "⚠ marks a broad reach: it can see or change things outside its own corner.",
					FONT_REGULAR, 12, MARGIN, y + 6, bw) + 6;
			}
			y += 16;
		} else if (detail_remote_loading || details_loading) {
			y = note(cr, y, "Asking Flatpak…");
		} else {
			y = note(cr, y, "Flatpak could not say (no connection?).");
		}
	}

	/* the facts */
	if (d) {
		if (d->download) {
			y = zmc_field(cr, y, "download", d->download);
		}
		if (d->installed_size) {
			y = zmc_field(cr, y, "installed size", d->installed_size);
		}
		if (d->runtime) {
			snprintf(line, sizeof(line), "%s%s", d->runtime, d->runtime_missing ? " (not on the phone yet)" :
				" (on the phone)");
			y = zmc_field(cr, y, "runtime", line);
		}
		if (d->developer) {
			y = zmc_field(cr, y, "developer", d->developer);
		}
		if (d->project_group) {
			y = zmc_field(cr, y, "project", d->project_group);
		}
		if (d->license) {
			y = zmc_field(cr, y, "licence", d->license);
		}
		if (d->rating) {
			y = zmc_field(cr, y, "content", d->rating);
		}
		y = link_field(cr, y, "website", d->homepage);
		y = link_field(cr, y, "report a problem", d->bugtracker);
		y = link_field(cr, y, "donate", d->donation);
		y = link_field(cr, y, "help", d->help);
		y = link_field(cr, y, "source code", d->vcs);
	} else if (p->license) {
		y = zmc_field(cr, y, "licence", p->license);
	}
	y = zmc_field(cr, y, "app ID", p->name);
	return y + 30;
}

/* one screenshot, large: the screenshots are tabs, swiped through */
static double draw_shot_page(cairo_t *cr, int page) {
	static char titles[SHOTS_MAX][8];
	static struct zmc_tab tabs[SHOTS_MAX];
	struct fh_detail *d = detail;
	if (!d || !d->shots.n) {
		return zmc_header(cr, "screenshots");
	}
	int n = d->shots.n - shot_base;
	n = n > SHOTS_MAX ? SHOTS_MAX : n;
	for (int i = 0; i < n; i++) {
		snprintf(titles[i], sizeof(titles[i]), "%d", shot_base + i + 1);
		tabs[i] = (struct zmc_tab){ titles[i], P_SHOT + i };
	}
	double y;
	if (n > 1) {
		y = zmc_pivot_wheel(cr, tabs, n, page);
	} else {
		y = zmc_header(cr, "1");
	}
	int k = shot_base + (page - P_SHOT);
	if (k < 0 || k >= d->shots.n) {
		return y;
	}
	struct fh_shot *s = d->shots.v[k];
	double bw = zmc.width - 2 * MARGIN;
	double h = shot_cell_h(s, bw), room = zmc.height - y - 130;
	h = h > room ? room : h;
	draw_shot(cr, s, MARGIN, y, bw, h);
	zmc_add_hit(MARGIN, y, bw, h, ZMC_ACT_BACK, 0);
	y += h + 12;
	if (s->caption) {
		white(cr);
		y += zmc_text(cr, s->caption, FONT_REGULAR, 16, MARGIN, y, bw) + 6;
	}
	grey(cr);
	char of[48];
	snprintf(of, sizeof(of), "%d of %d · swipe for the next", k + 1, d->shots.n);
	y += zmc_text(cr, of, FONT_REGULAR, 12.5, MARGIN, y, bw) + 16;
	if (page - P_SHOT == n - 1 && shot_base + n < d->shots.n) {
		y = button(cr, MARGIN, y, bw, "the next screenshots", ACT_SHOTS_PAGE, 0) + 12;
	}
	return y + 20;
}

static double draw_detail(cairo_t *cr) {
	struct pkg *p = shown;
	if (!p) {
		return zmc_header(cr, "app");
	}
	if (pkg_is_flathub(p)) {
		return draw_detail_flathub(cr, p);
	}
	double y = page_title(cr, pkg_title(p));
	double x = MARGIN + 110, w = zmc.width - x - MARGIN;
	draw_tile(cr, MARGIN, y, 96, p);
	white(cr);
	zmc_text_line(cr, p->name, FONT_SEMILIGHT, 20, x, y, w);
	grey(cr);
	char line[256];
	if (pkg_is_aur(p)) {
		snprintf(line, sizeof(line), "AUR · %s", p->packager ? p->packager : "no maintainer");
	} else {
		snprintf(line, sizeof(line), "from %s", p->repo);
	}
	zmc_text_line(cr, line, FONT_REGULAR, 13, x, y + 30, w);
	snprintf(line, sizeof(line), "version %s", p->version ? p->version : "?");
	zmc_text_line(cr, line, FONT_REGULAR, 13, x, y + 50, w);
	if (p->installed && p->installed_version && has_update(p)) {
		snprintf(line, sizeof(line), "you have %s", p->installed_version);
		zmc_text_line(cr, line, FONT_REGULAR, 13, x, y + 70, w);
	}
	y += 116;
	/* what can be done with it */
	double bw = zmc.width - 2 * MARGIN, half = (bw - 12) / 2;
	if (inst.state == INST_RUNNING && !strcmp(inst.name, p->name)) {
		y = button(cr, MARGIN, y, bw, inst.removing ? "removing…" : "installing…", ACT_PROGRESS, 0);
	} else if (p->installed) {
		if (has_update(p) && pkg_is_aur(p)) {
			y = button(cr, MARGIN, y, bw, "update", ACT_UPDATE, 0) + 12;
		}
		if (p->desktop) {
			button(cr, MARGIN, y, half, "open", ACT_OPEN, 0);
			y = button(cr, MARGIN + half + 12, y, half, "remove", ACT_REMOVE, 0);
		} else {
			y = button(cr, MARGIN, y, bw, "remove", ACT_REMOVE, 0);
		}
	} else {
		char built[4096];
		if (pkg_is_aur(p) && inst_built(p->name, p->version, built, sizeof(built))) {
			/* built here already, but it never went in: no need to build again */
			y = button(cr, MARGIN, y, bw, "install it now", ACT_INSTALL_BUILT, 0);
			grey(cr);
			y += zmc_text(cr, "Built on this phone already. Installing asks your password.", FONT_REGULAR, 13,
				MARGIN, y + 8, bw) + 8;
		} else {
			y = button(cr, MARGIN, y, bw, "install", ACT_INSTALL, 0);
		}
	}
	y += 22;
	white(cr);
	if (p->summary && p->desc && strcmp(p->summary, p->desc)) {
		y += zmc_text(cr, p->summary, FONT_SEMILIGHT, 18, MARGIN, y, bw) + 8;
	}
	if (p->desc) {
		y += zmc_text(cr, p->desc, FONT_REGULAR, 15, MARGIN, y, bw) + 18;
	}
	if (details_loading) {
		y = note(cr, y, "Getting the details…");
	}
	char v[256];
	if (pkg_is_aur(p)) {
		snprintf(v, sizeof(v), "%.0f", p->votes);
		y = zmc_field(cr, y, "votes", v);
		snprintf(v, sizeof(v), "%.2f", p->popularity);
		y = zmc_field(cr, y, "popularity", v);
		human_age(v, sizeof(v), p->modified);
		y = zmc_field(cr, y, "updated", v);
	} else {
		if (p->csize > 0) {
			human_size(v, sizeof(v), p->csize);
			y = zmc_field(cr, y, "download", v);
		}
		if (p->isize > 0) {
			human_size(v, sizeof(v), p->isize);
			y = zmc_field(cr, y, "installed size", v);
		}
		if (p->builddate > 0) {
			human_age(v, sizeof(v), p->builddate);
			y = zmc_field(cr, y, "built", v);
		}
		if (p->groups) {
			y = zmc_field(cr, y, "groups", p->groups);
		}
	}
	if (p->license) {
		y = zmc_field(cr, y, "licence", p->license);
	}
	if (p->url) {
		y = zmc_field(cr, y, "website", p->url);
	}
	if (p->depends) {
		y = zmc_field(cr, y, "needs", p->depends);
	}
	if (pkg_is_aur(p)) {
		/* the honest part: it is a recipe anyone may publish */
		double top = y + 6;
		cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
		const char *say = "Built on this phone from a recipe (a PKGBUILD) that anyone can publish to the AUR. "
			"Read it before you install. The build runs as you, and installing it asks your password.";
		cairo_rectangle(cr, MARGIN, top, bw, 150);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
		zmc_text(cr, "BUILT ON THIS PHONE", FONT_SEMIBOLD, 11, MARGIN + 14, top + 12, 0);
		double th = zmc_text(cr, say, FONT_REGULAR, 13.5, MARGIN + 14, top + 34, bw - 28);
		white(cr);
		zmc_text(cr, "read the recipe", FONT_REGULAR, 15, MARGIN + 14, top + 44 + th, 0);
		zmc_add_hit(MARGIN, top + 34 + th, bw, 40, ACT_RECIPE, 0);
		y = top + 150 + 20;
	}
	return y + 20;
}

static double draw_progress(cairo_t *cr) {
	double y = zmc_header(cr, inst.removing ? "removing" : "installing");
	white(cr);
	zmc_text_line(cr, inst.name, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN);
	y += 40;
	double bw = zmc.width - 2 * MARGIN;
	if (inst.state == INST_FAILED) {
		white(cr);
		y += zmc_text(cr, "That did not work.", FONT_REGULAR, 17, MARGIN, y, bw) + 6;
		grey(cr);
		y += zmc_text(cr, inst.error, FONT_REGULAR, 14, MARGIN, y, bw) + 18;
		if (inst.aur_needs[0]) {
			char first[128];
			sscanf(inst.aur_needs, "%127s", first);
			char label[160];
			snprintf(label, sizeof(label), "find %s", first);
			y = button(cr, MARGIN, y, bw, label, ACT_FIND, 0) + 12;
		}
		if (inst.can_retry) {
			y = button(cr, MARGIN, y, bw, "install it now", ACT_RETRY, 0) + 12;
		}
	} else {
		white(cr);
		y += zmc_text(cr, inst.stage, FONT_REGULAR, 17, MARGIN, y, bw) + 18;
		if (inst.flatpak && inst.state == INST_RUNNING && inst.percent >= 0) {
			/* flatpak's own count of the download */
			cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
			cairo_rectangle(cr, MARGIN, y, bw, 6);
			cairo_fill(cr);
			zmc_accent(cr);
			cairo_rectangle(cr, MARGIN, y, bw * (inst.percent > 100 ? 100 : inst.percent) / 100.0, 6);
			cairo_fill(cr);
			char pc[16];
			snprintf(pc, sizeof(pc), "%d%%", inst.percent);
			grey(cr);
			zmc_text(cr, pc, FONT_REGULAR, 12, MARGIN, y + 12, 0);
			y += 40;
		}
	}
	/* what pacman, makepkg or flatpak last said */
	if (inst.log_n) {
		double lh = 15, h = inst.log_n * lh + 20;
		cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
		cairo_rectangle(cr, MARGIN, y, bw, h);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		for (int i = 0; i < inst.log_n; i++) {
			int k = (inst.log_head - inst.log_n + i + INST_LOG_LINES) % INST_LOG_LINES;
			zmc_text_line(cr, inst.log[k], FONT_MONO, 10.5, MARGIN + 10, y + 10 + i * lh, bw - 20);
		}
		y += h + 22;
	}
	if (inst.state == INST_RUNNING) {
		if (inst.cancellable) {
			y = button(cr, MARGIN, y, bw, "cancel", ACT_CANCEL, 0);
		}
	} else {
		if (inst.state == INST_DONE && !inst.removing && shown && shown->desktop) {
			y = button(cr, MARGIN, y, bw, "open", ACT_OPEN, 0) + 12;
		}
		y = button(cr, MARGIN, y, bw, "done", ACT_DONE, 0);
	}
	return y + 30;
}

static double render(cairo_t *cr, int page) {
	if (page < 0 || page >= P_COUNT) {
		return 0;
	}
	drawing = page;
	n_rows[page] = 0;
	switch (page) {
	case P_TOP: return draw_top(cr);
	case P_CATS: return draw_cats(cr);
	case P_INSTALLED: return draw_installed(cr);
	case P_SEARCH: return draw_search(cr);
	case P_LIST: return draw_list(cr);
	case P_DETAIL: return draw_detail(cr);
	case P_PROGRESS: return draw_progress(cr);
	default:
		if (page >= P_SHOT && page <= P_SHOT_LAST) {
			return draw_shot_page(cr, page);
		}
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------------------- */

static void open_details(const struct pkg *p) {
	pkg_free(shown);
	shown = pkg_copy(p);
	details_loading = false;
	shot_base = 0;
	shot_allow(false);
	if (pkg_is_flathub(shown)) {
		/* the catalogue's copy, which knows what is installed */
		struct pkg *c = fh ? flathub_find(fh, shown->name) : NULL;
		if (c) {
			pkg_free(shown);
			shown = pkg_copy(c);
		}
		if (!shown->arch) {
			fh_detail_start(shown->name);
		}
	} else if (pkg_is_aur(shown)) {
		struct info *in = calloc(1, sizeof(*in));
		snprintf(in->name, sizeof(in->name), "%s", shown->name);
		details_loading = true;
		job_run(info_work, details_done, in);
	}
	go(P_DETAIL);
}

/* the command a .desktop file starts, without its %f %u field codes */
static bool launch_desktop(const char *desktop) {
	char *text = desktop ? read_all(desktop, NULL) : NULL;
	if (!text) {
		return false;
	}
	bool ok = false, in_entry = false;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '[') {
			in_entry = starts_with(line, "[Desktop Entry]");
		} else if (in_entry && starts_with(line, "Exec=")) {
			char cmd[512];
			size_t o = 0;
			for (const char *c = line + 5; *c && o + 1 < sizeof(cmd); c++) {
				if (*c == '%' && c[1]) {
					c++;
					if (*c == '%') {
						cmd[o++] = '%';
					}
					continue;
				}
				cmd[o++] = *c;
			}
			cmd[o] = 0;
			char ctl[600];
			snprintf(ctl, sizeof(ctl), "launch %s", trim(cmd));
			ok = shell_ctl(ctl);
			break;
		}
	}
	free(text);
	return ok;
}

static void list_open(const char *title, struct vec *items) {
	rows_forget();
	vec_free(&list_items, pkg_free);
	list_items = *items;
	snprintf(list_title, sizeof(list_title), "%s", title);
	go(P_LIST);
}

static int by_fit(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	if (x->mobile != y->mobile) {
		return x->mobile ? -1 : 1;
	}
	return strcasecmp(pkg_title(x), pkg_title(y));
}

static void tick(void);

static void install_changed(void) {
	zmc_redraw();
}

static void install_finished(bool ok) {
	zmc_haptic(ok ? "click" : "heavy");
	if (ok) {
		start_load();	/* what is installed has changed */
	}
	zmc_redraw();
}

static void activate(int action, int arg) {
	if (action != ACT_SEARCHBOX && zmc.page == P_SEARCH && typing) {
		typing = false;
		zmc_keyboard(false);
	}
	switch (action) {
	case ACT_PKG:
		if (zmc.page >= 0 && zmc.page < P_COUNT && arg >= 0 && arg < n_rows[zmc.page] &&
				rows[zmc.page][arg]) {
			open_details(rows[zmc.page][arg]);
		}
		break;
	case ACT_SEARCHBOX:
		typing = true;
		zmc_keyboard(true);
		if (zmc.page != P_SEARCH) {
			go(P_SEARCH);
		}
		break;
	case ACT_FILTER:
		filter = arg;
		rows_forget();
		break;
	case ACT_SEE_ALL:
		if (arg >= 0 && arg < sections.n) {
			struct section *s = sections.v[arg];
			struct vec items = { 0 };
			for (int i = 0; i < s->items.n; i++) {
				vec_push(&items, pkg_copy(s->items.v[i]));
			}
			list_open(s->title, &items);
		}
		break;
	case ACT_CAT:
	case ACT_GROUP: {
		struct vec *from = action == ACT_CAT ? &cats : &groups;
		if (ri && arg >= 0 && arg < from->n) {
			struct facet *f = from->v[arg];
			struct vec items = { 0 };
			facet_members(ri, action == ACT_GROUP, f->key, &items);
			list_open(f->label, &items);
		}
		break;
	}
	case ACT_INSTALL:
	case ACT_UPDATE:
		if (shown && pkg_is_flathub(shown)) {
			if (inst_start_flatpak(action == ACT_UPDATE ? "update" : "install", shown->name, NULL)) {
				go(P_PROGRESS);
				zmc_every(500, tick);
			}
		} else if (shown && inst_start(shown->name, pkg_is_aur(shown), false)) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_FH_UPDATE_ALL:
		if (inst_start_flatpak("update-all", NULL, "Flathub apps")) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_ADD_FLATHUB:
		if (inst_start_flatpak("add-remote", NULL, "Flathub")) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_FH_CAT:
		if (fh && arg >= 0 && arg < fh_cats.n) {
			struct facet *f = fh_cats.v[arg];
			struct vec items = { 0 };
			facet_members_flathub(&fh->apps, f->key, &items);
			/* the ones made for a phone first, then by name */
			qsort(items.v, items.n, sizeof(void *), by_fit);
			char title[160];
			snprintf(title, sizeof(title), "%s · Flathub", f->label);
			list_open(title, &items);
		}
		break;
	case ACT_SHOT:
		if (detail && arg >= 0 && arg < detail->shots.n) {
			shot_base = arg - arg % SHOTS_MAX;
			go(P_SHOT + arg % SHOTS_MAX);
		}
		break;
	case ACT_SHOTS_PAGE:
		if (detail && shot_base + SHOTS_MAX < detail->shots.n) {
			shot_base += SHOTS_MAX;
			zmc_go(P_SHOT, 1);	/* (the same place to go back from) */
		}
		break;
	case ACT_SHOTS_LOAD:
		shot_allow(true);	/* the person asked: on mobile data too */
		break;
	case ACT_LINK:
		if (arg >= 0 && arg < link_n && links[arg] && (starts_with(links[arg], "https://") ||
				starts_with(links[arg], "http://"))) {
			char ctl[700];
			snprintf(ctl, sizeof(ctl), "launch xdg-open '%s'", links[arg]);
			if (!strchr(links[arg], '\'')) {
				shell_ctl(ctl);
			}
		}
		break;
	case ACT_INSTALL_BUILT: {
		char built[4096];
		if (shown && inst_built(shown->name, shown->version, built, sizeof(built)) &&
				inst_start_built(shown->name, built)) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	}
	case ACT_REMOVE:
		if (shown && pkg_is_flathub(shown)) {
			if (inst_start_flatpak("remove", shown->name, NULL)) {
				go(P_PROGRESS);
				zmc_every(500, tick);
			}
		} else if (shown && inst_start(shown->name, false, true)) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_OPEN:
		if (shown && pkg_is_flathub(shown)) {
			struct pkg *c = fh ? flathub_find(fh, shown->name) : NULL;
			if (!launch_desktop(c && c->desktop ? c->desktop : shown->desktop)) {
				char ctl[400];
				snprintf(ctl, sizeof(ctl), "launch flatpak run %s", shown->name);
				shell_ctl(ctl);
			}
		} else if (shown) {
			struct pkg *l = ri ? repo_find_local(ri, shown->name) : NULL;
			launch_desktop(l && l->desktop ? l->desktop : shown->desktop);
		}
		break;
	case ACT_PROGRESS:
		go(P_PROGRESS);
		break;
	case ACT_CANCEL:
		inst_cancel();
		break;
	case ACT_DONE: {
		inst_clear();
		/* back where it was asked from: the app, or the installed page */
		int to = n_nav > 0 ? nav[--n_nav] : P_DETAIL;
		zmc_go(to, -1);
		break;
	}
	case ACT_RETRY:
		if (inst_retry()) {
			zmc_every(500, tick);
		}
		break;
	case ACT_FIND: {
		char first[128] = "";
		sscanf(inst.aur_needs, "%127s", first);
		snprintf(query, sizeof(query), "%s", first);
		filter = 2;
		search_now();
		go(P_SEARCH);
		break;
	}
	case ACT_RECIPE:
		if (shown) {
			char ctl[400];
			snprintf(ctl, sizeof(ctl), "launch xdg-open " AUR_BASE "/cgit/aur.git/tree/PKGBUILD?h=%s", shown->name);
			shell_ctl(ctl);
		}
		break;
	case ACT_CHECK_UPDATES:
		start_unit("ztos-update-check.service");
		snprintf(update_status, sizeof(update_status), "checking");
		aur_updates_checked = false;
		check_aur_updates();
		fh_updates_checked = false;
		check_fh_updates();
		break;
	case ACT_UPDATE_ALL:
		start_unit("ztos-update-apply.service");
		snprintf(update_status, sizeof(update_status), "installing");
		break;
	}
}

static bool back(void) {
	if (typing) {
		typing = false;
		zmc_keyboard(false);
	}
	if (zmc.page == P_TOP || zmc.page == P_CATS || zmc.page == P_INSTALLED) {
		return false;	/* close */
	}
	int to = n_nav > 0 ? nav[--n_nav] : P_TOP;
	if (zmc.page == P_PROGRESS && inst.state != INST_RUNNING) {
		inst_clear();
	}
	zmc_go(to, -1);
	return true;
}

static void arrived(int page) {
	if (page == P_INSTALLED) {
		read_updates();
		check_aur_updates();
		check_fh_updates();
		zmc_every(1000, tick);	/* the updater's status, while this page is open */
	} else if (inst.state != INST_RUNNING) {
		zmc_every(0, NULL);
	}
}

static void tick(void) {
	if (inst.state == INST_RUNNING) {
		inst_tick();
	}
	if (zmc.page == P_INSTALLED) {
		char before[200];
		snprintf(before, sizeof(before), "%s", update_status);
		read_updates();
		if (strcmp(before, update_status)) {
			if (starts_with(update_status, "done")) {
				start_load();
			}
			zmc_redraw();
		}
	} else if (inst.state != INST_RUNNING) {
		zmc_every(0, NULL);
	}
}

static void typed(const char *t) {
	if (zmc.page != P_SEARCH || !typing) {
		return;
	}
	size_t n = strlen(query);
	if (!strcmp(t, "\b")) {
		/* one whole character, however many bytes */
		while (n > 0 && ((unsigned char)query[n - 1] & 0xc0) == 0x80) {
			n--;
		}
		if (n > 0) {
			n--;
		}
		query[n] = 0;
	} else if (!strcmp(t, "\n")) {
		typing = false;
		zmc_keyboard(false);
		search_now();
		return;
	} else if (n + strlen(t) < sizeof(query) - 1) {
		strcat(query, t);
	}
	debounce();
}

static const struct zmc_app app = {
	.app_id = "ztstore", .title = "App Store", .caps = "APP STORE",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .text = typed,
};

static void prune_work(void *unused) {
	(void)unused;
	shots_prune();
}

int main(int argc, char **argv) {
	/* ztstore --render PAGE FILE [PACKAGE]: draw a page into a PNG with no
	 * display, from this phone's data, for checking the look. Pages: top,
	 * categories, installed, search, detail. */
	if (argc >= 4 && !strcmp(argv[1], "--render")) {
		struct load *l = calloc(1, sizeof(*l));
		load_local(l);
		apply_load(l);
		loading = false;
		/* (Flathub's lists: fetched now if there is no copy) */
		sections_fill_flathub(&sections, fh, metered ? LONG_MAX / 2 : FH_API_MAX_AGE);
		const char *names[] = { "top", "categories", "installed", "search", "list", "detail", "progress", "shot" };
		int page = P_TOP;
		for (int i = 0; i < 8; i++) {
			if (!strcmp(argv[2], names[i])) {
				page = i;
			}
		}
		if (argc >= 5) {
			snprintf(query, sizeof(query), "%s", argv[4]);
			struct pkg *p = ri ? repo_find_sync(ri, argv[4]) : NULL;
			if (!p && ri) {
				p = repo_find_local(ri, argv[4]);
			}
			if (!p && fh) {
				p = flathub_find(fh, argv[4]);
			}
			if (p) {
				shown = pkg_copy(p);
			}
			if (shown && pkg_is_flathub(shown)) {
				/* everything the page would wait for, waited for here */
				detail = flathub_detail(fh, shown->name, SHOT_WANT_PX);
				if (detail) {
					flathub_remote_info(detail);
					for (int i = 0; i < detail->shots.n; i++) {
						shot_fetch_now(((struct fh_shot *)detail->shots.v[i])->url);
					}
				}
			}
			if (ri) {
				search_repo(ri, query, 50, &found_repo);
			}
			if (fh) {
				search_list(&fh->apps, query, 50, &found_fh);
				flathub_search_other_arch(fh, query, &found_other);
			}
			/* "shot N": that screenshot, large */
			if (page == P_SHOT && argc >= 6) {
				page = P_SHOT + atoi(argv[5]) % SHOTS_MAX;
				shot_base = atoi(argv[5]) - atoi(argv[5]) % SHOTS_MAX;
			}
		}
		if (getenv("ZTSTORE_FILTER")) {
			filter = atoi(getenv("ZTSTORE_FILTER"));
		}
		read_updates();
		/* (ZTSTORE_RENDER_H: a taller picture, for a long page) */
		int h = getenv("ZTSTORE_RENDER_H") ? atoi(getenv("ZTSTORE_RENDER_H")) : 0;
		return zmc_render_png(&app, page, h > 0 ? h : 2400, argv[3]);
	}
	jobs_init();
	inst_init(install_changed, install_finished);
	shots_init(zmc_redraw);
	job_run(prune_work, NULL, NULL);
	start_load();
	return zmc_run(&app, P_TOP);
}
