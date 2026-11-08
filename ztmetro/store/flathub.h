// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: Flathub, through the phone's own flatpak.
 *
 * What there is: Flathub's AppStream for aarch64, as flatpak itself downloads
 * it (`flatpak update --appstream`) into
 *   /var/lib/flatpak/appstream/flathub/aarch64/active/appstream.xml(.gz)
 * Only apps in it are offered: it lists exactly the apps built for this arch.
 * What is popular, new and picked: Flathub's web API (flathub.org/api/v2),
 * cached for half a day. Sizes and permissions of one app: `flatpak
 * remote-info`. What is installed and what can be updated: `flatpak list` and
 * `flatpak remote-ls --updates`. Installing, updating and removing go through
 * the App Store's root service (install.c, ztstore-helper), system-wide. */
#ifndef ZTSTORE_FLATHUB_H
#define ZTSTORE_FLATHUB_H

#include "json.h"
#include "pkg.h"

#define FLATHUB_REMOTE	"flathub"
#define FLATHUB_API	"https://flathub.org/api/v2"
#define FLATHUB_ARCH	"aarch64"
#define FLATHUB_AS_DIR	"/var/lib/flatpak/appstream/" FLATHUB_REMOTE "/" FLATHUB_ARCH "/active"

/* Every app in Flathub's catalogue for this arch: struct pkg with name = the
 * app's ID, repo = "flathub", app_name, summary, icon (a cached file),
 * categories, desc (the description as text), license, packager (the
 * developer), version (the newest release), url (homepage), depends (the
 * runtime ref), verified and mobile. Sorted by ID. */
struct flathub {
	struct vec apps;
	long stamp;		/* the catalogue file's mtime */
	char path[512];		/* which file */
};

/* Read the catalogue under `dir` (normally FLATHUB_AS_DIR); false if none.
 * What the lists need is kept in ~/.cache/ztstore/flathub-index and read from
 * there while the catalogue is unchanged (two seconds become a few ms). */
bool flathub_load(struct flathub *fh, const char *dir);
/* that index, written and read (exposed for the tests); reading checks it
 * belongs to fh->stamp and fh->path */
bool flathub_index_write(const struct flathub *fh, const char *path);
bool flathub_index_read(struct flathub *fh, const char *path);
/* when the catalogue under `dir` came: its .gz's mtime (the plain XML in
 * flatpak's checkout always says 1970); -1 if there is none */
long flathub_stamp(const char *dir);
void flathub_free(struct flathub *fh);
struct pkg *flathub_find(const struct flathub *fh, const char *id);
/* parse catalogue text (exposed for the tests): `icons` is the folder of its
 * cached icons, or NULL */
void flathub_parse(struct flathub *fh, const char *xml, size_t len, const char *icons);

/* ---- one app's details ------------------------------------------------------ */

struct fh_shot {
	char *caption;
	char *url;		/* the image chosen for the width asked for */
	int w, h;
};

struct fh_release {
	char *version, *notes;	/* notes as text, maybe NULL */
	long when;
};

struct fh_perm {
	char *text;		/* in plain words */
	bool warn;		/* broad: reaches outside the app's own corner */
};

struct fh_detail {
	char *id;
	char *description;	/* as text: paragraphs and lists */
	char *developer, *license, *project_group;
	struct vec shots;	/* struct fh_shot * */
	struct vec releases;	/* struct fh_release *, newest first */
	char *homepage, *bugtracker, *donation, *help, *vcs, *translate, *contact;
	char *rating;		/* the content rating in words, NULL if it has none */
	char *runtime;		/* "org.gnome.Platform/aarch64/51" */
	bool verified, mobile;
	/* from flatpak remote-info, when it answered */
	bool have_remote;
	char *download, *installed_size;	/* as flatpak says them: "1.9 MB" */
	char *remote_version;
	struct vec perms;	/* struct fh_perm * */
	bool runtime_missing;	/* the runtime is not on the phone yet */
	bool x11_only;		/* it needs X11, and the phone has no Xwayland */
	char *runtime_download;	/* its size, when it is missing */
};

/* the details of `id` out of the catalogue file; screenshots chosen for a
 * screen `width_px` pixels across */
struct fh_detail *flathub_detail(const struct flathub *fh, const char *id, int width_px);
/* the same from catalogue text (for the tests) */
struct fh_detail *flathub_detail_parse(const char *xml, size_t len, const char *id, int width_px);
void fh_detail_free(struct fh_detail *d);

/* Ask flatpak for its sizes, version and permissions (a few seconds: it may
 * fetch the remote's summary). Fills d->have_remote and the rest. */
void flathub_remote_info(struct fh_detail *d);
/* The permissions in a flatpak metadata keyfile ([Context] and the bus
 * policies), in plain words, broad ones marked (exposed for the tests). */
void flathub_permissions(const char *metadata, struct vec *out);
/* "Download Size: 1.9 MB" style answers of `flatpak remote-info` */
void flathub_parse_remote_info(const char *text, struct fh_detail *d);

/* ---- this phone --------------------------------------------------------------- */

/* Installed apps (system-wide), as packages: name, version (installed),
 * installed = true, repo "flathub" (their origin), desktop (the exported
 * .desktop file, if any). From `flatpak list`. */
void flathub_installed(struct vec *out);
/* parse `flatpak list --columns=application,version,origin,size` (tests) */
void flathub_parse_list(const char *text, struct vec *out);
/* Apps with an update: IDs and new versions, from `flatpak remote-ls
 * --updates` (asks the network). false when flatpak could not answer. */
bool flathub_updates(struct vec *out);

/* Is the Flathub remote set up system-wide? */
bool flathub_remote_present(void);
/* Refresh the catalogue (flatpak update --appstream) if it is older than
 * max_age seconds or missing. Runs as the person: flatpak's polkit policy lets
 * the active session's user do that without a password. */
bool flathub_refresh(long max_age);

/* ---- Flathub's web API ---------------------------------------------------------- */

/* a collection: "popular", "trending", "recently-added", "recently-updated",
 * "verified", "mobile"; or the editors' picks: "picks" (apps of the week and
 * app of the day). Gives app IDs, best first, from a copy at most max_age old
 * (max_age < 0: only a copy, never the network). false if neither the network
 * nor a copy had it. */
bool flathub_collection(const char *which, struct vec *ids, long max_age);
/* the IDs in one API answer (exposed for the tests): hits[].app_id, or
 * apps[].app_id (picks), or app_id (app of the day) */
void flathub_api_ids(const struct json *j, struct vec *ids);
/* Search Flathub itself for `term`: the apps that are there but have no build
 * for this arch, as packages (name = ID, app_name, summary, arch = what it is
 * built for). Never the ones the catalogue has. */
bool flathub_search_other_arch(const struct flathub *fh, const char *term, struct vec *out);

/* ---- the network ---------------------------------------------------------------- */

/* Is the phone on a metered connection (mobile data)? NetworkManager says. */
bool network_metered(void);

#endif
