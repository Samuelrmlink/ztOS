/* Two things that take the whole shell down or lock it: check-reload-hub
 *  - a settings reload must keep the scale: load_config starts from automatic
 *    (0), and a surface told 0 aborts wlroots (changing a running app's scale)
 *  - when the hub warning moves on by itself, its unseen buttons must stop
 *    taking touches (one of them reboots to recovery) */
#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <stdio.h>

int main(void) {
	char dir[] = "/tmp/ztreloadXXXXXX", path[200];
	int bad = 0;
	if (!mkdtemp(dir)) return 2;
	setenv("HOME", dir, 1);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	snprintf(path, sizeof(path), "%s/.config", dir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/.config/ztmetro", dir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/.config/ztmetro/app-scale", dir);
	FILE *f = fopen(path, "w");
	fputs("brave-browser = 0.75\n", f);
	fclose(f);

	static struct zt_server s;
	s.width = 360; s.height = 740; s.scale = 4.0f;
	reload_config(&s);
	if (s.scale != 4.0f) { printf("FAIL: reload left scale at %g\n", s.scale); bad = 1; }
	if (s.n_app_scales != 1 || s.app_scales[0].zoom != 0.75f) { printf("FAIL: app scale not read\n"); bad = 1; }

	/* past the 45 s limit, hub still not ready */
	setenv("ZT_TEST_SENSORHUB_FAIL", "1", 1);
	s.sensorhub_fail_screen = true;
	s.boot_wait_started_ms = now_ms() - 50000;
	boot_check_sensorhub(&s);
	if (s.sensorhub_fail_screen) { printf("FAIL: hub warning still takes touches\n"); bad = 1; }

	if (!bad) printf("PASS: reload keeps the scale; hub warning lets go\n");
	return bad;
}
