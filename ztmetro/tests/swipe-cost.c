/* make check-swipe-cost: what a swipe between tabs costs, counted on the real
 * core (and the real texture cache in zrnmetrogpu.c) driving a real app, with
 * no compositor and no GPU: Wayland and GL are stubs that count. Link it with
 * an app's source, its main() renamed app_main (see the Makefile).
 *
 * It swipes SWIPES times, as a finger does, on a spot that is no slider, and
 * says for each swipe how many times the page was drawn, how much went to the
 * GPU, and how much stays held afterwards. It fails if what is held keeps
 * growing with the swipes. Writes nothing outside SWIPE_DIR. */
#define _GNU_SOURCE
#include "../core/zrnmetrocore.c"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <sys/resource.h>

#include "swipe-cost/fake-platform.h"

/* ---- the script --------------------------------------------------------------- */
static double (*real_render)(cairo_t *, int);
static struct zmc_app counted;
static unsigned renders;
static double counted_render(cairo_t *cr, int page) {
	renders++;
	return real_render(cr, page);
}

static double clock_of(clockid_t id) {
	struct timespec t;
	clock_gettime(id, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static long rss_kb(void) {
	long pages = 0, rss = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if (f) {
		if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
		fclose(f);
	}
	return rss * sysconf(_SC_PAGESIZE) / 1024;
}

static int swipes = 12, swipe_at, step;
static double wake_at, frame_at;
static enum { S_START, S_ENTERING, S_MOVE, S_COAST, S_IDLE, S_DONE } state;
static unsigned renders_at, tex_held_first, frames_at, gl_draws_at;
static size_t uploaded_at, moving_at, held_first, held_peak;
static double cpu_at;
static uint32_t fake_time = 1000;
static double swipe_y = 112;
static int failures;

static void report(void) {
	double cpu = clock_of(CLOCK_PROCESS_CPUTIME_ID);
	unsigned f = frames - frames_at;
	printf("%3d  page %2d  %2u pages drawn  %7.1f MB up  (%6.1f MB in motion)  %6.1f MB %2u tex held  "
		"cache %5d px  %3u frames, %3u GL draws each  %6.1f ms cpu  rss %6.1f MB\n", swipe_at + 1, zmc.page,
		renders - renders_at, (gl_uploaded - uploaded_at) / 1048576.0, moving_at / 1048576.0,
		gl_live / 1048576.0, gl_tex_live, zmc.cache ? cairo_image_surface_get_height(zmc.cache) : 0,
		f, f ? (gl_draws - gl_draws_at) / f : 0, cpu - cpu_at, rss_kb() / 1024.0);
	frames_at = frames;
	gl_draws_at = gl_draws;
	if (swipe_at == 4) {	/* every tab has been seen by now */
		held_first = gl_live;
		tex_held_first = gl_tex_live;
	}
	if (swipe_at > 4 && gl_live > held_peak) held_peak = gl_live;
	renders_at = renders;
	uploaded_at = gl_uploaded;
	cpu_at = cpu;
}

static void script(double now) {
	switch (state) {
	case S_START: {
		counted = *zmc.app;
		real_render = counted.render;
		counted.render = counted_render;
		zmc.app = &counted;
		uint32_t active[1] = { XDG_TOPLEVEL_STATE_ACTIVATED };
		struct wl_array states = { sizeof(active), sizeof(active), active };
		xdg_toplevel_configure(NULL, NULL, 360, 664, &states);
		xdg_surface_configure(NULL, NULL, 1);
		state = S_ENTERING;
		wake_at = now + 16;
		break;
	}
	case S_ENTERING:
		wake_at = now + 16;
		if (!zmc.anim_active && !zmc.frame) {
			state = S_IDLE;
			wake_at = now + 600;
			step = -1;
		}
		break;
	case S_MOVE: {
		/* right to left over 130 ms, on a spot with nothing to grab */
		double x = 320 - step * 32;
		fake_time += 16;
		if (step == 0) {
			moving_at = gl_uploaded;
			touch_down(NULL, NULL, 0, fake_time, NULL, 0, wl_fixed_from_double(x), wl_fixed_from_double(swipe_y));
		} else if (step < 9) {
			touch_motion(NULL, NULL, fake_time, 0, wl_fixed_from_double(x), wl_fixed_from_double(swipe_y));
		} else {
			touch_up(NULL, NULL, 0, fake_time, 0);
			state = S_COAST;
		}
		step++;
		wake_at = now + 16;
		break;
	}
	case S_COAST:
		wake_at = now + 16;
		if (!zmc.slide_active && !zmc.swiping && !zmc.frame && !zmc.dirty) {
			moving_at = gl_uploaded - moving_at;
			state = S_IDLE;
			step = 0;
			wake_at = now + 600;	/* idle: the core draws both tabs ahead */
		}
		break;
	case S_IDLE:
		if (step >= 0) {
			report();
			if (++swipe_at >= swipes) {
				state = S_DONE;
				zmc.running = false;
				break;
			}
		} else {
			renders_at = renders;
			uploaded_at = gl_uploaded;
			cpu_at = clock_of(CLOCK_PROCESS_CPUTIME_ID);
			printf("opened: %u draws, %.1f MB up, %.1f MB %u tex held, rss %.1f MB\n", renders,
				gl_uploaded / 1048576.0, gl_live / 1048576.0, gl_tex_live, rss_kb() / 1024.0);
		}
		state = S_MOVE;
		step = 0;
		wake_at = now;
		break;
	case S_DONE:
		break;
	}
}

int wl_display_dispatch_pending(struct wl_display *a) {
	double now = clock_of(CLOCK_MONOTONIC);
	if (zmc.frame && now >= frame_at) {
		frame_at = now + 16;
		frame_done(NULL, zmc.frame, (uint32_t)now);
	}
	if (now >= wake_at) script(now);
	double next = wake_at;
	if (zmc.frame && frame_at < next) next = frame_at;
	double in = next - clock_of(CLOCK_MONOTONIC);
	in = in < 1 ? 1 : in;
	struct itimerspec spec = { .it_value = { (time_t)(in / 1000), (long)(fmod(in, 1000) * 1e6) } };
	timerfd_settime(display_fd, 0, &spec, NULL);
	return 0;
}

/* some apps, so that the apps page has rows */
static void write_state(const char *dir, int apps) {
	char path[400];
	snprintf(path, sizeof(path), "%s/ztos-audio-state.json", dir);
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "{\"version\":1,\"active\":null,\"outputs\":[{\"name\":\"spk\",\"desc\":\"Speaker\",\"kind\":\"speaker\","
		"\"percent\":50,\"mute\":false,\"current\":true}],\"apps\":[");
	for (int i = 0; i < apps; i++) {
		fprintf(f, "%s{\"key\":\"app%d\",\"name\":\"App number %d\",\"output\":\"default\",\"percent\":70,"
			"\"mute\":false,\"running\":false,\"output_present\":true}", i ? "," : "", i, i);
	}
	fprintf(f, "]}");
	fclose(f);
}

int app_main(int argc, char **argv);

int main(int argc, char **argv) {
	static char *args[] = { "app", "apps", NULL };
	if (getenv("SWIPE_ARGS_AS_GIVEN")) return app_main(argc, argv);	/* (--render, with no display) */
	const char *dir = getenv("SWIPE_DIR");
	const char *apps = getenv("SWIPE_APPS");
	if (!dir) dir = "/tmp";
	setenv("XDG_RUNTIME_DIR", dir, 1);
	setenv("HOME", dir, 1);
	if (!getenv("ZTMETRO_SCALE")) setenv("ZTMETRO_SCALE", "4", 1);
	if (getenv("SWIPES")) swipes = atoi(getenv("SWIPES"));
	if (getenv("SWIPE_Y")) swipe_y = atof(getenv("SWIPE_Y"));
	if (argc > 1) args[1] = argv[1];
	write_state(dir, apps ? atoi(apps) : 6);
	int status = app_main(2, args);
	printf("after the first five swipes: %.1f MB held in %u textures; the most after that: %.1f MB\n",
		held_first / 1048576.0, tex_held_first, held_peak / 1048576.0);
	if (held_peak > held_first + (held_first >> 3)) {
		printf("FAIL: what is held keeps growing with the swipes\n");
		failures++;
	}
	if (!failures) puts("PASS: what a swipe holds does not grow");
	return status || failures;
}
