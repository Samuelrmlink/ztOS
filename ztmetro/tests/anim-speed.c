/* make check-anim-speed: the animation speeds (core/zt-anim.h, 8.66) as the
 * config says them, the old menu_speed carried over, and every duration. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../core/zt-anim.h"
#include "../settings/config-merge.h"

/* A config's text, read as the shell, Settings and the apps read it. */
static void load(struct zt_anim_cfg *c, const char *text) {
	char buf[4096], *save = NULL;
	snprintf(buf, sizeof(buf), "%s", text);
	zt_anim_defaults(c);
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = line, *v = eq + 1, *e = eq;
		while (*k == ' ') k++;
		while (e > k && e[-1] == ' ') *--e = 0;
		while (*v == ' ') v++;
		for (e = v + strlen(v); e > v && e[-1] == ' '; ) *--e = 0;
		zt_anim_config_line(c, k, v);
	}
}

int main(void) {
	struct zt_anim_cfg c;

	/* no file: the master at 0.75x (the shell's pace since 2026-11), every kind 1x */
	load(&c, "");
	assert(c.global == 75 && !c.slow);
	for (int k = 0; k < ZT_ANIM_N; k++) assert(c.kind[k] == 100);
	assert(zt_anim_speed(&c, ZT_ANIM_APP_OPEN) == 75);
	assert(zt_anim_ms(&c, ZT_ANIM_APP_OPEN, 300) == 400);	/* 300 ms at 0.75x */

	/* migration: an old file's menu_speed is the master */
	load(&c, "accent = cobalt\nmenu_speed = 100\n");
	assert(c.global == 100 && !c.global_set);
	load(&c, "menu_speed = 60\n");	/* not a step any more: the nearest */
	assert(c.global == 50);
	load(&c, "menu_speed = 200\nslow_motion = on\n");
	assert(c.global == 200 && c.slow);
	/* ...unless anim_global says otherwise, before it or after */
	load(&c, "menu_speed = 200\nanim_global = 150\n");
	assert(c.global == 150);
	load(&c, "anim_global = 150\nmenu_speed = 200\n");
	assert(c.global == 150);
	load(&c, "anim_global = off\nmenu_speed = 300\n");	/* (Settings writes 300 beside Off) */
	assert(c.global == 0 && zt_anim_off(&c, ZT_ANIM_START));

	/* each kind by its key; off, per cent, 1.5x; nonsense and others' keys left alone */
	load(&c, "anim_settings_link = off\nanim_app_open = 1.5x\nanim_scroll = 300\n"
		"anim_shade = banana\nanim_unknown = 50\nanim_toast = 0\n");
	assert(c.kind[ZT_ANIM_SETTINGS_LINK] == 0);
	assert(c.kind[ZT_ANIM_APP_OPEN] == 150);
	assert(c.kind[ZT_ANIM_SCROLL] == 300);
	assert(c.kind[ZT_ANIM_SHADE] == 100);
	assert(c.kind[ZT_ANIM_TOAST] == 0);
	{
		struct zt_anim_cfg d;
		zt_anim_defaults(&d);
		assert(!zt_anim_config_line(&d, "anim_unknown", "50"));
		assert(!zt_anim_config_line(&d, "accent", "red"));
		assert(zt_anim_config_line(&d, "anim_lock", "on") && d.kind[ZT_ANIM_LOCK] == 100);
	}
	/* every kind's key is its own and reads back */
	for (int k = 0; k < ZT_ANIM_N; k++) {
		char key[48];
		struct zt_anim_cfg d;
		zt_anim_defaults(&d);
		snprintf(key, sizeof(key), "anim_%s", ZT_ANIM_INFO[k].key);
		assert(zt_anim_config_line(&d, key, "25") && d.kind[k] == 25);
		for (int j = 0; j < ZT_ANIM_N; j++) if (j != k) assert(d.kind[j] == 100);
		assert(ZT_ANIM_INFO[k].name && ZT_ANIM_INFO[k].group >= 0 && ZT_ANIM_INFO[k].group < ZT_ANIM_G_N);
	}

	/* snapping to the steps */
	assert(zt_anim_snap(0) == 0 && zt_anim_snap(-5) == 0);
	assert(zt_anim_snap(1) == 25 && zt_anim_snap(1000) == 300 && zt_anim_snap(110) == 100);
	assert(zt_anim_snap(140) == 150 && zt_anim_snap(260) == 300);
	for (int i = 0; i < ZT_ANIM_N_STEPS; i++) {
		assert(zt_anim_step_index(ZT_ANIM_STEPS[i]) == i);
	}
	assert(!strcmp(zt_anim_step_name(0), "Off"));

	/* durations: the kind's speed times the master's */
	zt_anim_defaults(&c);
	c.global = 100;
	c.kind[ZT_ANIM_APP_OPEN] = 200;
	assert(zt_anim_ms(&c, ZT_ANIM_APP_OPEN, 300) == 150);
	c.global = 50;
	assert(zt_anim_ms(&c, ZT_ANIM_APP_OPEN, 300) == 300);	/* 2x of 0.5x */
	assert(zt_anim_ms(&c, ZT_ANIM_SHADE, 260) == 520);
	c.global = 300;
	c.kind[ZT_ANIM_PRESS] = 300;
	assert(zt_anim_ms(&c, ZT_ANIM_PRESS, 1) == 1);	/* never 0 for a length that was there */
	assert(zt_anim_ms(&c, ZT_ANIM_PRESS, 0) == 0);
	/* the screen's turn keeps its time under the master, not under its own speed */
	c.global = 25;
	assert(zt_anim_ms(&c, ZT_ANIM_ROTATION, 400) == 400);
	c.kind[ZT_ANIM_ROTATION] = 200;
	assert(zt_anim_ms(&c, ZT_ANIM_ROTATION, 400) == 200);
	/* slow motion: five times as long, on top */
	c.global = 100;
	c.kind[ZT_ANIM_ROTATION] = 100;
	c.slow = true;
	assert(zt_anim_ms(&c, ZT_ANIM_SHADE, 260) == 1300);
	assert(zt_anim_rate(&c, ZT_ANIM_SHADE) == 0.2);
	c.slow = false;

	/* Off: nothing to draw and nothing to wait for (a timer still fires) */
	c.kind[ZT_ANIM_SETTINGS_LINK] = 0;
	assert(zt_anim_off(&c, ZT_ANIM_SETTINGS_LINK) && zt_anim_ms(&c, ZT_ANIM_SETTINGS_LINK, 520) == 0);
	assert(zt_anim_wait_ms(&c, ZT_ANIM_SETTINGS_LINK, 520) == 1);
	assert(zt_anim_rate(&c, ZT_ANIM_SETTINGS_LINK) == 0);
	assert(!zt_anim_off(&c, ZT_ANIM_SETTINGS_PAGES));	/* only that kind */
	c.global = 0;	/* the master off: every kind, the turn too */
	for (int k = 0; k < ZT_ANIM_N; k++) assert(zt_anim_off(&c, k) && zt_anim_ms(&c, k, 500) == 0);
	assert(zt_anim_ms(&c, ZT_ANIM_ROTATION, 400) == 0);

	/* Settings writes only what changed: a migrated file keeps its menu_speed
	 * line and gains nothing until a speed is chosen */
	{
		static char out[2048];
		struct config_key k[2];
		memset(k, 0, sizeof(k));
		k[0].key = "anim_global";
		snprintf(k[0].loaded, sizeof(k[0].loaded), "100");
		snprintf(k[0].value, sizeof(k[0].value), "100");
		k[1].key = "menu_speed";
		snprintf(k[1].loaded, sizeof(k[1].loaded), "100");
		snprintf(k[1].value, sizeof(k[1].value), "100");
		const char *text = "accent = cobalt\nmenu_speed = 100\n";
		FILE *in = fmemopen((void *)text, strlen(text), "r"), *o = fmemopen(out, sizeof(out), "w");
		config_merge(in, o, k, 2);
		fclose(in);
		fclose(o);
		assert(!strcmp(out, text));
		/* the master to Off: anim_global = off, and menu_speed at its fastest for an older shell */
		snprintf(k[0].value, sizeof(k[0].value), "off");
		snprintf(k[1].value, sizeof(k[1].value), "300");
		in = fmemopen((void *)text, strlen(text), "r");
		o = fmemopen(out, sizeof(out), "w");
		config_merge(in, o, k, 2);
		fclose(in);
		fclose(o);
		assert(!strcmp(out, "accent = cobalt\nmenu_speed = 300\nanim_global = off\n"));
		load(&c, out);
		assert(c.global == 0);
	}
	puts("PASS: animation speeds, their migration and every duration");
	return 0;
}
