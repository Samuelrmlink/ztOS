#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zlib.h>
#include "../sections.h"
#include "../appstream.h"

int main(void) {
	struct vec secs = {0};
	assert(sections_read(&secs, "data/sections") && secs.n == 9);
	struct section *s0 = secs.v[0];
	assert(!strcmp(s0->source, "repo") && !strcmp(s0->sort, "popular") && s0->count == 10);
	gzFile gz = gzopen("/tmp/ztstore-fixture/dump.json.gz", "wb");
	gzputs(gz, "[{\"Name\":\"yay\",\"NumVotes\":2500,\"Popularity\":40,\"FirstSubmitted\":1,\"OutOfDate\":null},"
		"{\"Name\":\"paru\",\"NumVotes\":900,\"Popularity\":45,\"FirstSubmitted\":3},"
		"{\"Name\":\"stale\",\"NumVotes\":9999,\"Popularity\":99,\"FirstSubmitted\":9,\"OutOfDate\":1700000000},"
		"{\"Name\":\"x\",\"NumVotes\":1,\"Popularity\":0.1,\"FirstSubmitted\":5}]");
	gzclose(gz);
	FILE *f = fopen("/tmp/ztstore-fixture/pkgstats.json", "w");
	fputs("{\"total\":3,\"packagePopularities\":[{\"name\":\"glibc\",\"popularity\":100},"
		"{\"name\":\"firefox\",\"popularity\":80},{\"name\":\"mpv\",\"popularity\":40}]}", f);
	fclose(f);
	struct repo_index ri;
	repo_load(&ri, "/tmp/ztstore-fixture/pacman.conf");
	const char *roots[] = { "/tmp/ztstore-fixture/as", NULL };
	appstream_apply(&ri, roots);
	sections_fill(&secs, &ri, "/tmp/ztstore-fixture/dump.json.gz", "/tmp/ztstore-fixture/pkgstats.json");
	struct section *pop = secs.v[0], *aurpop = secs.v[4], *votes = secs.v[5], *fresh = secs.v[7], *built = secs.v[8];
	assert(pop->items.n == 2 && !strcmp(((struct pkg *)pop->items.v[0])->name, "firefox"));	/* glibc is no app */
	assert(aurpop->items.n == 3 && !strcmp(((struct pkg *)aurpop->items.v[0])->name, "paru"));	/* stale skipped */
	assert(!strcmp(((struct pkg *)votes->items.v[0])->name, "yay") && ((struct pkg *)votes->items.v[0])->installed);
	assert(!strcmp(((struct pkg *)fresh->items.v[0])->name, "x"));
	assert(built->items.n == 2 && !strcmp(((struct pkg *)built->items.v[0])->name, "mpv"));
	sections_fill(&secs, &ri, NULL, NULL);
	assert(aurpop->note && pop->note && !pop->items.n);
	struct vec cats = {0}, groups = {0};
	facets_categories(&ri, &cats, 12);
	facets_groups(&ri, &groups, 12);
	assert(cats.n == 3 && groups.n == 1 && !strcmp(((struct facet *)groups.v[0])->key, "xfce4"));
	int seen_av = 0;
	for (int i = 0; i < cats.n; i++) {
		struct facet *c = cats.v[i];
		if (!strcmp(c->key, "AudioVideo")) { seen_av = 1; assert(!strcmp(c->label, "audio video")); }
	}
	assert(seen_av);
	struct vec members = {0};
	facet_members(&ri, false, "Video", &members);
	assert(members.n == 1);
	vec_free(&members, pkg_free); vec_free(&cats, facet_free); vec_free(&groups, facet_free);
	/* Flathub's lists: from cached API answers, only what the catalogue has */
	setenv("XDG_CACHE_HOME", "/tmp/ztstore-fixture/cache", 1);
	mkdir("/tmp/ztstore-fixture/cache", 0755);
	mkdir("/tmp/ztstore-fixture/cache/ztstore", 0755);
	f = fopen("/tmp/ztstore-fixture/cache/ztstore/flathub-popular.json", "w");
	fputs("{\"hits\":[{\"app_id\":\"com.spotify.Client\"},{\"app_id\":\"org.example.Game\"},"
		"{\"app_id\":\"org.gnome.Calculator\"}]}", f);
	fclose(f);
	struct flathub fh = {0};
	size_t n;
	char *xml = read_all("tests/fixtures/flathub-appstream.xml", &n);
	flathub_parse(&fh, xml, n, NULL);
	free(xml);
	sections_fill_flathub(&secs, &fh, -1);
	struct section *picks = secs.v[1], *phones = secs.v[2], *fpop = secs.v[3], *fnew = secs.v[6];
	assert(!strcmp(fpop->source, "flathub") && fpop->items.n == 2);	/* spotify: not built for aarch64 */
	assert(!strcmp(((struct pkg *)fpop->items.v[0])->name, "org.example.Game"));
	assert(phones->items.n == 1 && !strcmp(((struct pkg *)phones->items.v[0])->name, "org.gnome.Calculator"));
	assert(!picks->items.n && !picks->note && !fnew->items.n);	/* no copy yet: no note either */
	sections_fill_flathub(&secs, NULL, -1);
	assert(fpop->note && !fpop->items.n);
	struct vec fcats = {0};
	facets_flathub(&fh.apps, &fcats, 12);
	assert(fcats.n == 2);	/* Utility, Game: "Calculator" is no main category */
	struct vec fm = {0};
	facet_members_flathub(&fh.apps, "Game", &fm);
	assert(fm.n == 1);
	vec_free(&fm, pkg_free);
	vec_free(&fcats, facet_free);
	flathub_free(&fh);
	vec_free(&secs, section_free);
	repo_free(&ri);
	puts("sections: ok");
	return 0;
}
