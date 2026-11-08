#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../flathub.h"
#include "../xml.h"

#define FIX "tests/fixtures/flathub-appstream.xml"

static bool has_perm(struct vec *perms, const char *text, bool warn) {
	for (int i = 0; i < perms->n; i++) {
		struct fh_perm *p = perms->v[i];
		if (strstr(p->text, text) && p->warn == warn) {
			return true;
		}
	}
	return false;
}

int main(void) {
	/* the catalogue, from a folder laid out as flatpak's */
	const char *dir = "/tmp/ztstore-fixture/fh";
	mkdir("/tmp/ztstore-fixture", 0755);
	mkdir(dir, 0755);
	mkdir("/tmp/ztstore-fixture/fh/icons", 0755);
	mkdir("/tmp/ztstore-fixture/fh/icons/128x128", 0755);
	FILE *f = fopen("/tmp/ztstore-fixture/fh/icons/128x128/org.gnome.Calculator.png", "w");
	fputs("png", f);
	fclose(f);
	size_t n;
	char *xml = read_all(FIX, &n);
	assert(xml);
	assert(write_atomic("/tmp/ztstore-fixture/fh/appstream.xml", xml, n));

	setenv("XDG_CACHE_HOME", "/tmp/ztstore-fixture/cache", 1);
	unlink("/tmp/ztstore-fixture/cache/ztstore/flathub-index");
	struct flathub fh;
	assert(flathub_load(&fh, dir));
	/* read again: from the index it wrote, the same */
	struct flathub again;
	assert(flathub_load(&again, dir) && again.apps.n == fh.apps.n);
	for (int i = 0; i < fh.apps.n; i++) {
		struct pkg *a = fh.apps.v[i], *b = again.apps.v[i];
		assert(!strcmp(a->name, b->name) && !strcmp(a->app_name, b->app_name) && !strcmp(a->summary, b->summary));
		assert(a->verified == b->verified && a->mobile == b->mobile);
		assert(!a->icon == !b->icon && !a->keywords == !b->keywords && !strcmp(a->depends, b->depends));
	}
	flathub_free(&again);
	/* an index of another catalogue is not read */
	struct flathub other = { .stamp = 1 };
	snprintf(other.path, sizeof(other.path), "%s", fh.path);
	assert(!flathub_index_read(&other, "/tmp/ztstore-fixture/cache/ztstore/flathub-index") && !other.apps.n);
	assert(fh.apps.n == 2);	/* the runtime is not an app */
	struct pkg *c = flathub_find(&fh, "org.gnome.Calculator");
	assert(c && pkg_is_flathub(c));
	assert(!strcmp(c->app_name, "Calculator"));
	assert(!strcmp(c->summary, "Perform arithmetic, scientific or financial calculations"));
	assert(!strcmp(c->packager, "The GNOME Project"));
	assert(!strcmp(c->license, "GPL-3.0-or-later"));
	assert(!strcmp(c->version, "51.1"));
	assert(!strcmp(c->depends, "org.gnome.Platform/aarch64/51"));
	assert(!strcmp(c->categories, "Utility Calculator"));
	assert(!strcmp(c->keywords, "calculation unit-converter"));
	assert(!strcmp(c->url, "https://apps.gnome.org/Calculator"));
	assert(c->icon && strstr(c->icon, "icons/128x128/org.gnome.Calculator.png"));
	assert(c->verified && c->mobile);
	struct pkg *g = flathub_find(&fh, "org.example.Game");	/* ".desktop" taken off */
	assert(g && !strcmp(g->summary, "Shoot <things>") && !strcmp(g->packager, "Example & Co"));
	assert(!g->verified && !g->mobile && !g->icon);
	assert(!flathub_find(&fh, "org.gnome.Platform") && !flathub_find(&fh, "nope"));

	/* details: description as text, screenshots for a screen 600 px across */
	struct fh_detail *d = flathub_detail(&fh, "org.gnome.Calculator", 600);
	assert(d);
	assert(!strcmp(d->description,
		"Calculator is an application that solves mathematical equations. Though it at first appears & simple,\n\n"
		"Modes:\n\n• Advanced mode\n• Financial\n\n1. first\n2. second"));
	assert(!strcmp(d->developer, "The GNOME Project") && !strcmp(d->project_group, "GNOME"));
	assert(d->shots.n == 2);
	struct fh_shot *s0 = d->shots.v[0], *s1 = d->shots.v[1];
	assert(!strcmp(s0->caption, "Basic Mode") && s0->w == 410 && strstr(s0->url, "image-1_orig"));
	assert(!s1->caption && s1->w == 624 && strstr(s1->url, "624x554"));
	assert(d->releases.n == 2);	/* the development release left out */
	struct fh_release *r0 = d->releases.v[0], *r1 = d->releases.v[1];
	assert(!strcmp(r0->version, "51.1") && r0->when == 1791590400 && !strcmp(r0->notes, "• Updated translations"));
	assert(!strcmp(r1->version, "50.0") && r1->when == 1767312000 && !r1->notes);
	assert(!strcmp(d->homepage, "https://apps.gnome.org/Calculator") && d->bugtracker && d->donation && !d->help);
	assert(!strcmp(d->rating, "for everyone: nothing of concern declared"));
	assert(!strcmp(d->runtime, "org.gnome.Platform/aarch64/51") && d->verified);
	fh_detail_free(d);
	d = flathub_detail(&fh, "org.example.Game", 600);
	assert(d && !strcmp(d->description, "Plain text, no markup.") && !strcmp(d->developer, "Example & Co"));
	assert(!strcmp(d->rating, "mild cartoon violence, moderate in-app purchases"));
	assert(!d->shots.n && !d->releases.n);
	fh_detail_free(d);
	assert(!flathub_detail(&fh, "org.gnome.Platform", 600));
	flathub_free(&fh);

	/* permissions in plain words */
	struct vec perms = { 0 };
	flathub_permissions("[Application]\nname=x\n\n[Context]\nshared=ipc;network;\n"
		"sockets=fallback-x11;wayland;pulseaudio;\ndevices=dri;\n"
		"filesystems=xdg-download;home:ro;!host;xdg-config/foo:create;/media;\n\n"
		"[Session Bus Policy]\norg.freedesktop.Notifications=talk\norg.freedesktop.Flatpak=talk\n"
		"org.example.Own=own\n\n[System Bus Policy]\norg.freedesktop.UPower=talk\n", &perms);
	assert(has_perm(&perms, "uses the internet", false));
	assert(has_perm(&perms, "microphone", false));
	assert(has_perm(&perms, "graphics chip", false));
	assert(has_perm(&perms, "read and change your Downloads folder", false));
	assert(has_perm(&perms, "can read all the files in your home folder", true));
	assert(has_perm(&perms, "settings in foo", false));
	assert(has_perm(&perms, "/media", true));
	assert(has_perm(&perms, "notifications", false));
	assert(has_perm(&perms, "outside its sandbox", true));
	assert(has_perm(&perms, "battery", false));
	assert(!has_perm(&perms, "all your files and the system's", true));	/* !host takes away */
	for (int i = 0; i < perms.n; i++) {
		struct fh_perm *p = perms.v[i];
		free(p->text);
		free(p);
	}
	vec_free(&perms, NULL);
	flathub_permissions("[Context]\nsockets=x11;\ndevices=all;\nfilesystems=host;\n", &perms);
	assert(has_perm(&perms, "X11", true) && has_perm(&perms, "every device", true));
	assert(has_perm(&perms, "all your files and the system's", true));
	for (int i = 0; i < perms.n; i++) {
		struct fh_perm *p = perms.v[i];
		free(p->text);
		free(p);
	}
	vec_free(&perms, NULL);
	flathub_permissions("[Application]\nname=x\n", &perms);
	assert(perms.n == 1 && has_perm(&perms, "own sandbox", false));
	for (int i = 0; i < perms.n; i++) {
		struct fh_perm *p = perms.v[i];
		free(p->text);
		free(p);
	}
	vec_free(&perms, NULL);

	/* flatpak's answers */
	struct fh_detail ri = { 0 };
	flathub_parse_remote_info("Calculator - Perform arithmetic\n\n            ID: org.gnome.Calculator\n"
		"       Version: 51.1\n Download Size: 1.9 MB\nInstalled Size: 5.2 MB\n"
		"       Runtime: org.gnome.Platform/aarch64/51\n", &ri);
	assert(ri.have_remote && !strcmp(ri.download, "1.9 MB") && !strcmp(ri.installed_size, "5.2 MB"));
	assert(!strcmp(ri.remote_version, "51.1") && !strcmp(ri.runtime, "org.gnome.Platform/aarch64/51"));
	free(ri.download);
	free(ri.installed_size);
	free(ri.remote_version);
	free(ri.runtime);
	struct vec list = { 0 };
	flathub_parse_list("app.grayjay.Grayjay\t20\tflathub\norg.other.App\t\tother-remote\n\nnonsense\n", &list);
	assert(list.n == 2);
	struct pkg *l0 = list.v[0], *l1 = list.v[1];
	assert(!strcmp(l0->name, "app.grayjay.Grayjay") && !strcmp(l0->installed_version, "20") && l0->installed);
	assert(!l0->packager && !strcmp(l1->packager, "other-remote") && !l1->installed_version);
	vec_free(&list, pkg_free);

	/* the web API's lists */
	struct vec ids = { 0 };
	const char *hits = "{\"hits\":[{\"app_id\":\"a.b.C\",\"arches\":[\"x86_64\"]},{\"name\":\"no id\"},{\"app_id\":\"d.e.F\"}]}";
	struct json *j = json_parse(hits, strlen(hits));
	flathub_api_ids(j, &ids);
	json_free(j);
	const char *picks = "{\"apps\":[{\"position\":1,\"app_id\":\"g.h.I\"}]}";
	j = json_parse(picks, strlen(picks));
	flathub_api_ids(j, &ids);
	json_free(j);
	const char *day = "{\"app_id\":\"j.k.L\",\"day\":\"2026-11-08\"}";
	j = json_parse(day, strlen(day));
	flathub_api_ids(j, &ids);
	json_free(j);
	assert(ids.n == 4 && !strcmp(ids.v[0], "a.b.C") && !strcmp(ids.v[1], "d.e.F") && !strcmp(ids.v[2], "g.h.I") &&
		!strcmp(ids.v[3], "j.k.L"));
	vec_free(&ids, free);

	/* markup on its own */
	const char *m = "<p>One <code>two</code></p><ul><li>a</li></ul>";
	char *t = xml_markup_text(m, m + strlen(m));
	assert(!strcmp(t, "One two\n\n• a"));
	free(t);
	free(xml);
	puts("flathub: ok");
	return 0;
}
