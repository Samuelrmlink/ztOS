/* An Android app tapped while Android is not marked ready is launched, to
 * start Android (ROADMAP 8.54 follow-up): "Starting Android...", not "has
 * stopped"; a second tap while that launch runs waits; "has stopped" comes
 * only from a failed launch (the control pipe's "waydroid-failed"). */
#define WD_TEST_NO_DRAW
#define WD_CONFIG		"/tmp/ztmetro-wd-test/waydroid.cfg"
#define WD_READY_MARKER		"/tmp/ztmetro-wd-test/ready"
#define WD_BOOT_UNIT		"ztmetro-wd-test-none-boot.service"
#define WD_SESSION_UNIT		"ztmetro-wd-test-none-session.service"
#define WD_READY_UNIT		"ztmetro-wd-test-none-ready.service"
#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static const char *APP = "ztos-android launch com.spotify.music --runtime waydroid";

static void launcher(pid_t pid) {
	char path[256];
	FILE *f = fopen(runtime_file(path, sizeof(path), WD_LAUNCHING), "w");
	assert(f);
	fprintf(f, "%d\n", (int)pid);
	fclose(f);
}

int main(void) {
	mkdir("/tmp/ztmetro-wd-test", 0700);
	mkdir("/tmp/ztmetro-wd-test/run", 0700);
	setenv("XDG_RUNTIME_DIR", "/tmp/ztmetro-wd-test/run", 1);
	FILE *cfg = fopen(WD_CONFIG, "w");
	assert(cfg);
	fclose(cfg);
	unlink(WD_READY_MARKER);
	char path[256];
	unlink(runtime_file(path, sizeof(path), WD_LAUNCHING));

	struct zt_server s = {0};
	s.wl_display = wl_display_create();
	s.loop = wl_display_get_event_loop(s.wl_display);
	wl_list_init(&s.toplevels);
	s.tapped = 3;
	g_wd_state = WD_DOWN;

	/* not an Android app: never held */
	assert(!wd_launch_blocked(&s, "ztsettings"));

	/* had been ready, marker gone, nothing running: failed, yet launched */
	g_wd_was_ready = true;
	g_sys_toast[0] = 0;
	assert(!wd_launch_blocked(&s, APP));
	assert(g_wd_state == WD_FAILED);
	assert(!strcmp(g_sys_toast, WD_MSG_STARTING));
	assert(s.tapped == 3);	/* (the tile is not sprung back) */

	/* never started: launched as well */
	g_wd_was_ready = false;
	g_wd_start_ms = 0;
	g_sys_toast[0] = 0;
	assert(!wd_launch_blocked(&s, APP));
	assert(!strcmp(g_sys_toast, WD_MSG_STARTING));

	/* a launch is already starting Android: the second tap waits */
	launcher(getpid());
	g_sys_toast[0] = 0;
	assert(wd_launch_blocked(&s, APP));
	assert(!strcmp(g_sys_toast, WD_MSG_BOOTING));
	assert(s.tapped == -1);

	/* a launcher that died does not hold taps back */
	pid_t child = fork();
	if (child == 0) {
		_exit(0);
	}
	waitpid(child, NULL, 0);
	launcher(child);
	s.tapped = 3;
	assert(!wd_launch_blocked(&s, APP));
	unlink(runtime_file(path, sizeof(path), WD_LAUNCHING));

	/* the launch failed after its recovery: now it has stopped */
	g_sys_toast[0] = 0;
	wd_launch_failed(&s);
	assert(!strcmp(g_sys_toast, WD_MSG_STOPPED));

	/* not set up: nothing said, nothing held */
	g_wd_state = WD_NONE;
	g_sys_toast[0] = 0;
	wd_launch_failed(&s);
	assert(!g_sys_toast[0]);
	assert(!wd_launch_blocked(&s, APP));

	puts("PASS: Android app launched to start Android when not ready (failed and never started), "
	     "second tap waits on a live launcher, dead launcher ignored, failed launch says stopped");
	return 0;
}
