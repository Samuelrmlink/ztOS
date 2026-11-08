// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: screenshots, fetched when a details page wants them, never before.
 *
 * Each is fetched once, on a thread, made no wider than SHOT_MAX_PX (a phone
 * does not need the 1920 px originals) and kept as a PNG in
 * ~/.cache/ztstore/shots/, which is kept under SHOT_CACHE_MB: the least
 * recently seen go first. In memory only the few last drawn are kept. On a
 * metered connection nothing is fetched unless asked (shot_allow). */
#ifndef ZTSTORE_SHOTS_H
#define ZTSTORE_SHOTS_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#define SHOT_MAX_PX	1080
#define SHOT_WANT_PX	720	/* the image chosen from those offered: the smallest this wide */
#define SHOT_CACHE_MB	40
#define SHOT_KEEP	6	/* decoded, in memory */

enum shot_state { SHOT_NONE, SHOT_LOADING, SHOT_READY, SHOT_FAILED, SHOT_HELD };

/* `ready` runs on the main loop when an image has arrived */
void shots_init(void (*ready)(void));
/* the picture for `url` if it is here; else NULL, and it is fetched (unless
 * held back on a metered connection). *state says which. */
cairo_surface_t *shot_get(const char *url, enum shot_state *state);
/* fetch even on a metered connection, from now on (the person asked) */
void shot_allow(bool allow);
void shot_set_metered(bool metered);
/* fetch and keep it now, on this thread (ztstore --render) */
void shot_fetch_now(const char *url);
/* keep the folder under its size: the oldest go (run on a thread) */
void shots_prune(void);
/* the file a URL is kept in (exposed for the tests) */
void shot_file(const char *url, char *out, size_t size);

#endif
