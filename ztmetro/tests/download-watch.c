/* Following an app's downloads through its files (make check-notify): reading
 * Grayjay's file, and what a look at a directory tree decides. Works in a temporary
 * $HOME of its own. The file below has the shape of a real one from the phone
 * (Grayjay 20, flathub), cut down. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>
#include "../notifyd/download-watch.h"

#define HLS "application/vnd.apple.mpegurl"
#define ID "e57c1676-5c8c-488d-a7bb-50d5305bbb67"

/* (the same keys at other depths, ahead of the one that counts, on purpose) */
#define GRAYJAY(state, progress, source) \
	"{\"State\":" state ",\"Video\":{\"ContentType\":1,\"Thumbnails\":{\"Sources\":[{\"Url\":\"https://x/y.jpg\",\"Quality\":0}]}," \
	"\"Duration\":3107,\"Metadata\":{},\"ID\":{\"Platform\":\"PeerTube\",\"Value\":\"" ID "\",\"ClaimType\":0}," \
	"\"Name\":\"Freenet - Services \\\"Without\\\" Servers\",\"Author\":{\"ID\":{\"Value\":\"futo\"},\"Name\":\"FUTO\"}}," \
	"\"VideoDetails\":{\"Description\":\"\\\"State\\\":9, \\\"Progress\\\":0.9 } {\",\"Name\":\"Details name\"," \
	"\"Video\":{\"VideoSources\":[{\"Name\":\"HLS\",\"Bitrate\":1,\"Duration\":3107}]}}," \
	"\"TargetPixelCount\":2738880,\"VideoSource\":" source ",\"AudioSource\":null,\"Progress\":" progress ",\"Error\":null}"
#define SOURCE_HLS "{\"Type\":\"VideoUrlSource\",\"Container\":\"" HLS "\",\"Name\":\"avc1\",\"Bitrate\":1005660,\"Duration\":0}"
#define SOURCE_MP4 "{\"Type\":\"VideoUrlSource\",\"Container\":\"video/mp4\",\"Name\":\"avc1\",\"Bitrate\":8000000,\"Duration\":0}"

static void test_json(void) {
	char out[32];
	double d;
	const char *j = "{ \"a\" : {\"b\":[1,{\"b\":2}],\"c\":\"x\\\"y\"}, \"b\":-1.5, \"s\":\"\\u00e9\\n\\\\\", \"n\":null }";
	assert(dl_json_complete(j));
	assert(dl_json_number(dl_json_get(j, "b"), &d) && d == -1.5);	/* its own, not the one inside "a" */
	assert(dl_json_string(dl_json_get(dl_json_get(j, "a"), "c"), out, sizeof(out)) && !strcmp(out, "x\"y"));
	assert(dl_json_string(dl_json_get(j, "s"), out, sizeof(out)) && !strcmp(out, "\xc3\xa9 \\"));
	assert(!dl_json_number(dl_json_get(j, "n"), &d));
	assert(!dl_json_string(dl_json_get(j, "n"), out, sizeof(out)));
	assert(!dl_json_get(j, "missing") && !dl_json_get(NULL, "a") && !dl_json_get("[1]", "a"));
	/* a string too long for its place is cut, within it */
	char small[6];
	assert(dl_json_string("\"abcdefghijkl\"", small, sizeof(small)) && strlen(small) < sizeof(small));

	/* cut anywhere, a file is not whole: that is what a read mid-write looks like */
	static const char whole[] = GRAYJAY("3", "0.5", SOURCE_HLS);
	char cut[sizeof(whole)];
	assert(dl_json_complete(whole));
	for (size_t n = 0; n < sizeof(whole) - 1; n++) {
		memcpy(cut, whole, n);
		cut[n] = 0;
		assert(!dl_json_complete(cut));
	}
	assert(!dl_json_complete(""));
	assert(!dl_json_complete("[]"));
	assert(!dl_json_complete("{} trailing"));
	assert(!dl_json_complete("{\"a\":\"\\"));
	assert(!dl_json_complete("}"));
}

static void test_grayjay(void) {
	struct dl_info i;
	/* as written when a download is asked for */
	assert(grayjay_parse(GRAYJAY("0", "0", SOURCE_HLS), &i));
	assert(!strcmp(i.name, "Freenet - Services \"Without\" Servers"));
	assert(!strcmp(i.id, ID));
	assert(i.progress == 0 && !i.finishing && !i.failed && i.spooled);
	assert(i.estimate == 1005660LL * 3107 / 8);
	/* direct to a file */
	assert(grayjay_parse(GRAYJAY("3", "0", SOURCE_MP4), &i) && !i.spooled);
	/* no source chosen yet */
	assert(grayjay_parse(GRAYJAY("0", "0", "null"), &i) && i.spooled && i.estimate == 0);
	/* a source with no bitrate: no guess */
	assert(grayjay_parse(GRAYJAY("3", "0", "{\"Container\":\"video/mp4\"}"), &i) && i.estimate == 0);
	/* the states it ends in, and a real progress */
	assert(grayjay_parse(GRAYJAY("3", "0.25", SOURCE_MP4), &i) && i.progress == 0.25 && !i.finishing);
	assert(grayjay_parse(GRAYJAY("4", "1", SOURCE_MP4), &i) && i.finishing);
	assert(grayjay_parse(GRAYJAY("5", "1", SOURCE_MP4), &i) && i.finishing);
	assert(grayjay_parse(GRAYJAY("6", "1", SOURCE_MP4), &i) && i.finishing);
	assert(grayjay_parse(GRAYJAY("7", "0.4", SOURCE_MP4), &i) && i.failed);
	/* nonsense in the fields */
	assert(grayjay_parse(GRAYJAY("3", "7.5", SOURCE_MP4), &i) && i.progress == 1);
	assert(grayjay_parse(GRAYJAY("3", "-1", SOURCE_MP4), &i) && i.progress == 0);
	assert(grayjay_parse(GRAYJAY("3", "null", SOURCE_MP4), &i) && i.progress == 0);
	/* not its file */
	assert(!grayjay_parse("{\"Other\":1}", &i));
	assert(!grayjay_parse("{\"State\":\"x\"}", &i));
	assert(!grayjay_parse("", &i));
	/* the bare minimum still reads; it has a name for the line, later */
	assert(grayjay_parse("{\"State\":0}", &i) && !i.name[0] && !i.id[0]);
}

static void test_view(void) {
	struct dl_info i;
	struct dl_view v;
	memset(&i, 0, sizeof(i));
	dl_view_of(&i, 0, &v);
	assert(v.percent == DL_BUSY && !strcmp(v.text, "Starting"));
	dl_view_of(&i, 5000000, &v);
	assert(v.percent == DL_BUSY && !strcmp(v.text, "Downloading, 5 MB"));
	dl_view_of(&i, 2500000000LL, &v);
	assert(v.percent == DL_BUSY && !strcmp(v.text, "Downloading, 2.5 GB"));
	i.estimate = 100000000;
	dl_view_of(&i, 42000000, &v);
	assert(v.percent == 41 || v.percent == 42);
	assert(strstr(v.text, "About") && strstr(v.text, "42 MB"));
	/* past the estimate: still not done, and doesn't overflow */
	dl_view_of(&i, 900000000, &v);
	assert(v.percent == 99);
	i.estimate = 1;
	dl_view_of(&i, 900000000, &v);
	assert(v.percent == 99);
	/* a real number beats it */
	i.estimate = 100000000;
	i.progress = 0.5;
	dl_view_of(&i, 1000, &v);
	assert(v.percent == 50 && !strcmp(v.text, "50%"));
	i.progress = 1;
	dl_view_of(&i, 1000, &v);
	assert(v.percent == DL_BUSY && !strcmp(v.text, "Finishing"));
	i.progress = 0.2;
	i.finishing = true;
	dl_view_of(&i, 1000, &v);
	assert(v.percent == DL_BUSY && !strcmp(v.text, "Finishing"));
}

/* ---- a tree on disk ---------------------------------------------------------- */

static char told[8][600];
static int n_told;

static void tell(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	assert(n_told < 8);
	vsnprintf(told[n_told++], sizeof(told[0]), fmt, ap);
	va_end(ap);
}

static char home[256];
static const struct dl_source *src = &dl_sources[0];

static void path_of(char *out, size_t size, const char *dir, const char *name) {
	snprintf(out, size, "%s/%s/%s", home, dir, name);
}

/* a file with this much in it, last written `age` seconds before `now` */
static void put(const char *dir, const char *name, const char *text, long size, time_t now, long age) {
	char path[600];
	path_of(path, sizeof(path), dir, name);
	FILE *f = fopen(path, "w");
	assert(f);
	if (text) {
		fputs(text, f);
	} else {
		/* real bytes, as a hole wouldn't count */
		static char block[65536];
		memset(block, 'x', sizeof(block));
		for (long left = size; left > 0; left -= (long)sizeof(block))
			fwrite(block, 1, left < (long)sizeof(block) ? (size_t)left : sizeof(block), f);
	}
	fclose(f);
	struct timeval tv[2] = { { now - age, 0 }, { now - age, 0 } };
	assert(!utimes(path, tv));
}

static void make_dirs(const char *rel) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", home, rel);
	for (char *p = path + strlen(home) + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0700);
			*p = '/';
		}
	}
	assert(!mkdir(path, 0700) || errno == EEXIST);
}

static void clean_tree(void) {
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/.var'", home);
	assert(!system(cmd));
	make_dirs(src->list_dir);
	make_dirs(src->data_dir);
	make_dirs(src->spool_dir);
	n_told = 0;
}

static bool look(struct dl_watch *w, time_t now) {
	n_told = 0;
	return dl_scan(w, now);
}

/* The file the phone actually had: left by an app that quit, with a killed
 * download's pieces around it. Nothing is shown, and nothing runs. */
static void test_left_behind(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	clean_tree();
	dl_start(&w, src, home, tell);
	put(src->list_dir, "ea115ce9-0f3d", GRAYJAY("0", "0", SOURCE_HLS), 0, now, 30000);
	char name[32];
	for (int i = 0; i < 20; i++) {
		snprintf(name, sizeof(name), "segment-%d", i);
		put(src->spool_dir, name, NULL, 1000, now, 30000);
	}
	assert(!look(&w, now));
	assert(n_told == 0);
	/* and then it writes a piece: the app is back at it. The old ones aren't its */
	put(src->spool_dir, "segment-new", NULL, 3000000, now, 1);
	assert(look(&w, now));
	assert(n_told == 1);
	assert(strstr(told[0], "notekey dl:grayjay:ea115ce9-0f3d|q|0|app.grayjay.Grayjay|Grayjay|Freenet - Services \"Without\" Servers|"));
	assert(strstr(told[0], "(3 MB)"));
	/* it stops, and is cleared; the file is still there */
	assert(!look(&w, now + DL_STALE_S + 1));
	assert(n_told == 1 && !strcmp(told[0], "noteclose dl:grayjay:ea115ce9-0f3d"));
	assert(!look(&w, now + DL_STALE_S + 2));
	assert(n_told == 0);
	close(w.fd);
}

/* A download from the start: asked for, bytes coming, joined, gone. */
static void test_hls_whole(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	clean_tree();
	dl_start(&w, src, home, tell);
	assert(!look(&w, now) && n_told == 0);

	put(src->list_dir, "u1", GRAYJAY("0", "0", SOURCE_HLS), 0, now, 0);
	assert(look(&w, now));
	assert(n_told == 1 && strstr(told[0], "|q|-2|") && strstr(told[0], "|Starting"));
	/* nothing changed: nothing said */
	assert(look(&w, now) && n_told == 0);

	/* an estimate of 390 MB: 39 MB is about 10% */
	put(src->spool_dir, "segment-a", NULL, 39000000, now, 0);
	assert(look(&w, now));
	assert(n_told == 1 && (strstr(told[0], "|q|9|") || strstr(told[0], "|q|10|")));
	/* pieces joined into the data file, the pieces still there: not counted twice */
	char name[128];
	snprintf(name, sizeof(name), "%s [2536x1080].mp4", ID);
	put(src->data_dir, name, NULL, 30000000, now, 0);
	assert(look(&w, now));
	assert(n_told == 0);	/* still 39 MB: the larger of the two, not 69 */

	/* its last save: all in */
	put(src->list_dir, "u1", GRAYJAY("3", "1", SOURCE_HLS), 0, now, -1);
	assert(look(&w, now + 1));
	assert(n_told == 1 && strstr(told[0], "|q|-2|") && strstr(told[0], "|Finishing"));

	/* gone from the list: finished */
	char path[600];
	path_of(path, sizeof(path), src->list_dir, "u1");
	assert(!unlink(path));
	assert(!look(&w, now));
	assert(n_told == 1 && !strcmp(told[0], "noteclose dl:grayjay:u1"));
	close(w.fd);
}

/* Direct to a file, under a cleaned-up name; and who owns what, with two going. */
static void test_direct_and_owners(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	clean_tree();
	dl_start(&w, src, home, tell);
	static const char other[] = "{\"State\":0,\"Video\":{\"ID\":{\"Value\":\"https://site/watch?v=Ab_C\"},\"Name\":\"Other | one\",\"Duration\":100},"
		"\"VideoSource\":{\"Container\":\"video/mp4\",\"Bitrate\":8000000},\"AudioSource\":{\"Container\":\"audio/mp4\",\"Bitrate\":0}}";
	put(src->list_dir, "old", GRAYJAY("0", "0", SOURCE_MP4), 0, now, 30000);
	put(src->list_dir, "new", other, 0, now, 30000);
	put(src->data_dir, "httpssitewatchvAb_C [640x360].mp4", NULL, 2000000, now, 0);
	assert(look(&w, now));
	/* only that one; a "|" in a title can't break the line; no bitrate for audio: no guess */
	assert(n_told == 1 && strstr(told[0], "dl:grayjay:new|q|-2|") && strstr(told[0], "|Other / one|Downloading, 2 MB"));
	/* an earlier copy of the first, long finished, doesn't count as it going */
	char name[128];
	snprintf(name, sizeof(name), "%s [640x360].mp4", ID);
	put(src->data_dir, name, NULL, 5000000, now, 30000);
	assert(look(&w, now) && n_told == 0);
	/* the app gives up on one */
	put(src->list_dir, "new", "{\"State\":7,\"Video\":{\"Name\":\"Other\"}}", 0, now, 0);
	assert(!look(&w, now));
	assert(n_told == 1 && !strcmp(told[0], "noteclose dl:grayjay:new"));
	close(w.fd);
}

/* What it must not read or believe. */
static void test_hostile(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	char path[600], target[600];
	clean_tree();
	dl_start(&w, src, home, tell);
	/* outside the app's directories, through a link: not followed */
	snprintf(target, sizeof(target), "%s/outside", home);
	FILE *f = fopen(target, "w");
	assert(f);
	fputs(GRAYJAY("3", "0.5", SOURCE_MP4), f);
	fclose(f);
	path_of(path, sizeof(path), src->list_dir, "link");
	assert(!symlink(target, path));
	/* a directory, a backup, a hidden file */
	path_of(path, sizeof(path), src->list_dir, "adir");
	assert(!mkdir(path, 0700));
	put(src->list_dir, "x.bak", GRAYJAY("3", "0.5", SOURCE_MP4), 0, now, 0);
	put(src->list_dir, ".hidden", GRAYJAY("3", "0.5", SOURCE_MP4), 0, now, 0);
	/* something else entirely, and one too large */
	put(src->list_dir, "garbage", "not json at all", 0, now, 0);
	put(src->list_dir, "huge", NULL, DL_FILE_MAX + 1, now, 0);
	/* read part way through being written: not shown yet */
	put(src->list_dir, "partial", "{\"State\":0,\"Video\":{\"Na", 0, now, 0);
	assert(!look(&w, now));
	assert(n_told == 0);
	/* and once it is whole, it is */
	put(src->list_dir, "partial", GRAYJAY("0", "0", SOURCE_MP4), 0, now, 0);
	assert(look(&w, now));
	assert(n_told == 1);
	/* a rewrite cut short keeps what was known */
	struct timeval tv[2] = { { now + 1, 0 }, { now + 1, 0 } };
	put(src->list_dir, "partial", "{\"State\":", 0, now, 0);
	path_of(path, sizeof(path), src->list_dir, "partial");
	assert(!utimes(path, tv));
	assert(look(&w, now + 1));
	assert(n_told == 0);
	close(w.fd);

	/* a key made from a file name can't break the line, either */
	clean_tree();
	dl_start(&w, src, home, tell);
	put(src->list_dir, "a|b c", GRAYJAY("0", "0", SOURCE_MP4), 0, now, 0);
	assert(look(&w, now));
	assert(n_told == 1 && strstr(told[0], "notekey dl:grayjay:a_b_c|q|"));
	close(w.fd);
}

/* More files than it follows: no harm, and the rest wait. */
static void test_too_many(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	clean_tree();
	dl_start(&w, src, home, tell);
	char name[16];
	for (int i = 0; i < DL_MAX + 4; i++) {
		snprintf(name, sizeof(name), "f%d", i);
		put(src->list_dir, name, GRAYJAY("0", "0", SOURCE_MP4), 0, now, 30000);
	}
	assert(!look(&w, now) && n_told == 0);
	close(w.fd);
}

/* The loop's side of it: nothing to do means no timer at all. */
static void test_timing(void) {
	struct dl_watch w;
	time_t now = time(NULL);
	char ev[64];
	clean_tree();
	dl_start(&w, src, home, tell);
	/* the first look is at once; afterwards, with nothing going on, no timer */
	assert(dl_tick(&w, 1000) == -1);
	assert(w.fd >= 0 && w.wd_list >= 0 && w.wd_data >= 0);
	assert(w.wd_spool == -1);	/* nothing waiting: the spool isn't listened to */
	assert(dl_tick(&w, 900000) == -1);
	assert(read(w.fd, ev, sizeof(ev)) < 0);	/* and looking wakes nothing */

	/* a new file wakes it, and it looks, but not at once after the last look */
	n_told = 0;
	put(src->list_dir, "t1", GRAYJAY("0", "0", SOURCE_MP4), 0, now, 0);
	w.last_scan = 900000;
	dl_readable(&w, 900100);
	assert(dl_tick(&w, 900100) == DL_EVENT_GAP_MS - 100);
	/* however much more it hears meanwhile, once */
	dl_readable(&w, 900200);
	assert(dl_tick(&w, 900200) == DL_EVENT_GAP_MS - 200);
	/* then shown: by the clock, at the slow rate */
	assert(dl_tick(&w, 901000) == DL_POLL_MS);
	assert(n_told == 1);
	assert(dl_tick(&w, 902000) == DL_POLL_MS - 1000);
	close(w.fd);

	/* a file waiting, nothing shown: the spool and writes are listened to */
	clean_tree();
	dl_start(&w, src, home, tell);
	put(src->list_dir, "t2", GRAYJAY("0", "0", SOURCE_HLS), 0, now, 30000);
	assert(dl_tick(&w, 1000) == -1);
	assert(w.wd_spool >= 0);
	/* the first piece is heard */
	while (read(w.fd, ev, sizeof(ev)) > 0) {
	}
	put(src->spool_dir, "segment-x", NULL, 100000, now, 0);
	assert(read(w.fd, ev, sizeof(ev)) > 0);
	while (read(w.fd, ev, sizeof(ev)) > 0) {
	}
	/* shown: it stops listening to them, and the timer takes over */
	dl_readable(&w, 5000);
	assert(dl_tick(&w, 5000) == DL_POLL_MS);
	assert(w.wd_spool == -1);
	while (read(w.fd, ev, sizeof(ev)) > 0) {
	}
	put(src->spool_dir, "segment-y", NULL, 100000, now, 0);
	assert(read(w.fd, ev, sizeof(ev)) < 0);
	/* a shell that started again is told again */
	n_told = 0;
	dl_resend(&w, 6000);
	assert(dl_tick(&w, 6000) == DL_POLL_MS);
	assert(n_told == 1 && strstr(told[0], "notekey dl:grayjay:t2"));
	close(w.fd);

	/* an app that isn't there: looked for now and then, and found when it comes */
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/.var'", home);
	assert(!system(cmd));
	dl_start(&w, src, home, tell);
	assert(dl_tick(&w, 1000) == -1 && w.wd_list < 0);
	assert(dl_tick(&w, 1000 + DL_RETRY_MS - 1) == -1);
	make_dirs(src->list_dir);
	assert(dl_tick(&w, 1000 + DL_RETRY_MS) == -1 && w.wd_list >= 0);
	close(w.fd);
}

int main(void) {
	test_json();
	test_grayjay();
	test_view();

	snprintf(home, sizeof(home), "/tmp/ztmetro-dl-test-%d", (int)getpid());
	assert(!mkdir(home, 0700));
	test_left_behind();
	test_hls_whole();
	test_direct_and_owners();
	test_hostile();
	test_too_many();
	test_timing();
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
	assert(!system(cmd));
	puts("PASS: download watcher: Grayjay's file, what a look decides, and when it looks");
	return 0;
}
