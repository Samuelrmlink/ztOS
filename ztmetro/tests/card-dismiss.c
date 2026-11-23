/* A recents card thrown away, and the empty switcher (ROADMAP 8.74, 8.75):
 * what is stopped and how, a hung process's SIGTERM and SIGKILL, a reused
 * pid left alone, a dismissed Android package kept from coming back, and
 * which windows get a card (make check-card-dismiss). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../card-dismiss.h"

static char proc[256];

static void fake_stat(int pid, const char *comm, char state, unsigned long long start) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%d", proc, pid);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%d/stat", proc, pid);
	FILE *f = fopen(path, "w");
	assert(f);
	/* fields 4..21 as zeros, 22 the start time, then more */
	fprintf(f, "%d (%s) %c", pid, comm, state);
	for (int i = 4; i <= 21; i++) fprintf(f, " %d", i);
	fprintf(f, " %llu 1234 5678\n", start);
	fclose(f);
}

static void gone(int pid) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%d/stat", proc, pid);
	unlink(path);
}

int main(void) {
	char pkg[DISMISS_PKG_MAX];

	/* --- what a dismiss stops --- */
	/* Spotify in Waydroid: its package, never a signal to the container */
	assert(dismiss_kind_of("waydroid.com.spotify.music", 4242, 100, pkg, sizeof(pkg)) == DISMISS_ANDROID);
	assert(!strcmp(pkg, "com.spotify.music"));
	assert(dismiss_kind_of("waydroid.org.Some_App2.x", 4242, 100, pkg, sizeof(pkg)) == DISMISS_ANDROID);
	/* a Waydroid window whose id is not a package name: nothing run */
	assert(dismiss_kind_of("waydroid.com.x;reboot", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(pkg[0] == 0);
	assert(dismiss_kind_of("waydroid.", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("waydroid.com.x y", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("waydroid.-rf", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("waydroid.com..x", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("waydroid.com.x.", 4242, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	/* a Linux app: its own process */
	assert(dismiss_kind_of("ztsettings", 4242, 100, pkg, sizeof(pkg)) == DISMISS_PROCESS);
	assert(dismiss_kind_of(NULL, 4242, 100, pkg, sizeof(pkg)) == DISMISS_PROCESS);
	/* never the shell itself, init, or no pid */
	assert(dismiss_kind_of("ztsettings", 100, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("ztsettings", 1, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("ztsettings", 0, 100, pkg, sizeof(pkg)) == DISMISS_NONE);
	assert(dismiss_kind_of("ztsettings", -1, 100, pkg, sizeof(pkg)) == DISMISS_NONE);

	/* package names: [a-zA-Z0-9._]+ only */
	assert(dismiss_package_ok("com.whatsapp"));
	assert(dismiss_package_ok("a"));
	assert(!dismiss_package_ok(""));
	assert(!dismiss_package_ok(NULL));
	assert(!dismiss_package_ok(".com"));
	assert(!dismiss_package_ok("1com"));
	assert(!dismiss_package_ok("com/evil"));
	assert(!dismiss_package_ok("com.x\n"));
	assert(!dismiss_package_ok("com.x*"));
	char longname[200];
	memset(longname, 'a', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = 0;
	assert(!dismiss_package_ok(longname));

	/* --- /proc/PID/stat --- */
	unsigned long long start;
	char state;
	assert(dismiss_parse_stat("12 (a b) c) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 99887 0", &start, &state));
	assert(start == 99887 && state == 'S');
	assert(!dismiss_parse_stat("12 (x) S 1 2", &start, &state));
	assert(!dismiss_parse_stat("garbage", &start, &state));
	assert(!dismiss_parse_stat(NULL, &start, &state));

	char base[] = "/tmp/card-dismiss-XXXXXX";
	assert(mkdtemp(base));
	snprintf(proc, sizeof(proc), "%s", base);
	fake_stat(500, "ztsettings", 'S', 777);
	assert(dismiss_proc_start(proc, 500, &start) && start == 777);
	fake_stat(501, "dead", 'Z', 778);
	assert(!dismiss_proc_start(proc, 501, &start));	/* a zombie is gone */
	assert(!dismiss_proc_start(proc, 502, &start));	/* no such process */

	/* --- a hung app: close, SIGTERM after DISMISS_CLOSE_MS, SIGKILL after
	 * DISMISS_TERM_MS more --- */
	struct dismiss_proc procs[DISMISS_PROCS] = {0};
	uint32_t t0 = 1000000;
	assert(!dismiss_pending(procs, DISMISS_PROCS));
	assert(dismiss_add(procs, DISMISS_PROCS, 500, 777, t0));
	assert(!dismiss_add(procs, DISMISS_PROCS, 500, 777, t0 + 10));	/* (thrown twice: one clock) */
	assert(dismiss_pending(procs, DISMISS_PROCS));
	struct dismiss_proc *d = &procs[0];
	assert(dismiss_step(d, t0 + 100, true) == DA_WAIT);
	assert(dismiss_step(d, t0 + DISMISS_CLOSE_MS - 1, true) == DA_WAIT);
	assert(dismiss_step(d, t0 + DISMISS_CLOSE_MS, true) == DA_TERM);
	assert(d->pid == 500);	/* still watched */
	assert(dismiss_step(d, t0 + DISMISS_CLOSE_MS + 500, true) == DA_WAIT);	/* the grace */
	assert(dismiss_step(d, t0 + DISMISS_CLOSE_MS + DISMISS_TERM_MS, true) == DA_KILL);
	assert(d->pid == 0 && !dismiss_pending(procs, DISMISS_PROCS));

	/* an app that closes when asked: no signal at all */
	assert(dismiss_add(procs, DISMISS_PROCS, 600, 5, t0));
	assert(dismiss_step(&procs[0], t0 + 300, false) == DA_DONE);
	assert(!dismiss_pending(procs, DISMISS_PROCS));

	/* one that ends on SIGTERM: no SIGKILL */
	assert(dismiss_add(procs, DISMISS_PROCS, 601, 5, t0));
	assert(dismiss_step(&procs[0], t0 + DISMISS_CLOSE_MS, true) == DA_TERM);
	assert(dismiss_step(&procs[0], t0 + DISMISS_CLOSE_MS + 100, false) == DA_DONE);

	/* the clock wrapping (now_ms is 32 bits): still in order */
	uint32_t near = 0xFFFFFFFFu - 200;
	assert(dismiss_add(procs, DISMISS_PROCS, 602, 5, near));
	assert(dismiss_step(&procs[0], near + 300, true) == DA_WAIT);	/* (wrapped) */
	assert(dismiss_step(&procs[0], near + DISMISS_CLOSE_MS, true) == DA_TERM);
	procs[0].pid = 0;

	/* a pid reused by another process meanwhile: its start time differs,
	 * so the shell takes it as gone and signals nothing */
	assert(dismiss_add(procs, DISMISS_PROCS, 500, 777, t0));
	fake_stat(500, "other", 'S', 999);
	assert(dismiss_proc_start(proc, 500, &start) && start != procs[0].start);
	assert(dismiss_step(&procs[0], t0 + DISMISS_CLOSE_MS, start == procs[0].start) == DA_DONE);
	gone(500);

	/* full: the ninth is refused, nothing overwritten */
	for (int i = 0; i < DISMISS_PROCS; i++) assert(dismiss_add(procs, DISMISS_PROCS, 700 + i, 1, t0));
	assert(!dismiss_add(procs, DISMISS_PROCS, 800, 1, t0));
	for (int i = 0; i < DISMISS_PROCS; i++) assert(procs[i].pid == 700 + i);

	/* --- a dismissed Android package stays gone --- */
	struct dismiss_recent recent[DISMISS_RECENT] = {0};
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.spotify.music", t0));
	dismiss_remember(recent, DISMISS_RECENT, "com.spotify.music", t0);
	assert(dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.spotify.music", t0 + 2000));
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.whatsapp", t0 + 2000));
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "com.spotify.music", t0 + 2000));	/* (ATL: not this) */
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, NULL, t0 + 2000));
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.spotify.music", t0 + DISMISS_REMAP_MS));
	/* opened again from its tile: forgotten at once */
	dismiss_forget(recent, DISMISS_RECENT, "waydroid.com.spotify.music");
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.spotify.music", t0 + 2000));
	/* more than fit: the oldest goes */
	for (int i = 0; i < DISMISS_RECENT + 1; i++) {
		char p[32];
		snprintf(p, sizeof(p), "com.app%d", i);
		dismiss_remember(recent, DISMISS_RECENT, p, t0 + 10 * (i + 1));
	}
	assert(!dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.app0", t0 + 100));
	for (int i = 1; i < DISMISS_RECENT + 1; i++) {
		char id[48];
		snprintf(id, sizeof(id), "waydroid.com.app%d", i);
		assert(dismiss_was_recent(recent, DISMISS_RECENT, id, t0 + 100));
	}
	/* the same package again: its own slot, refreshed */
	dismiss_remember(recent, DISMISS_RECENT, "com.app4", t0 + 9000);
	assert(dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.app4", t0 + 9000 + DISMISS_REMAP_MS - 1));
	assert(dismiss_was_recent(recent, DISMISS_RECENT, "waydroid.com.app1", t0 + 100));

	/* --- which windows get a card --- */
	assert(card_listable(true, false, false, false, true, "waydroid.com.spotify.music"));
	assert(card_listable(true, false, false, false, true, NULL));
	assert(!card_listable(true, false, true, false, true, "waydroid.com.spotify.music"));	/* thrown away */
	assert(!card_listable(true, true, false, false, true, "ztsettings"));	/* a background app sent Home */
	assert(!card_listable(false, false, false, false, true, "ztsettings"));	/* not mapped */
	assert(!card_listable(true, false, false, true, true, "Waydroid"));	/* the boot surface */
	assert(!card_listable(true, false, false, false, true, "Waydroid"));	/* the container's UI */
	assert(!card_listable(true, false, false, false, false, "waydroid.com.x"));	/* nothing drawn yet */

	/* the empty switcher's words */
	assert(!strcmp(SWITCH_EMPTY_TEXT, "No recent apps"));

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
	assert(system(cmd) == 0);
	puts("card-dismiss: ok");
	return 0;
}
