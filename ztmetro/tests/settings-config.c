/* make check-settings-config: Settings writes only the keys it changed. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../settings/config-merge.h"

static char result[8192];

static void set(struct config_key *k, const char *key, const char *value) {
	k->key = key;
	snprintf(k->value, sizeof(k->value), "%s", value);
	snprintf(k->loaded, sizeof(k->loaded), "%s", value);
	k->force = false;
}

static const char *merge(const char *text, struct config_key *keys, int n) {
	FILE *in = text ? fmemopen((void *)text, strlen(text), "r") : NULL;
	FILE *out = fmemopen(result, sizeof(result), "w");
	assert(out);
	config_merge(in, out, keys, n);
	if (in) fclose(in);
	fclose(out);
	return result;
}

int main(void) {
	struct config_key k[8];

	/* a slot nobody filled in is skipped: a wrong count crashed Settings on save */
	{
		struct config_key empty[2];
		memset(empty, 0, sizeof(empty));
		empty[0].key = "accent";
		snprintf(empty[0].value, sizeof(empty[0].value), "red");
		FILE *in = tmpfile(), *out = tmpfile();
		fputs("accent = blue\nother = 1\n", in);
		rewind(in);
		config_merge(in, out, empty, 2);
		fclose(in);
		fclose(out);
	}

	/* what was edited by hand, or is more than this version knows, survives
	 * another key's change: 3 tiles (it loads as 8), 120 s (it loads as 180),
	 * a speed not on the picker */
	const char *hand =
		"# ztMETRO configuration\n"
		"accent = cobalt\n"
		"quick = wifi,audio,settings\n"
		"timeout=120\n"
		"menu_speed=60\n"
		"unknown_key = 1\n";
	set(&k[0], "accent", "cobalt");
	set(&k[1], "quick", "wifi,settings,data,bluetooth,airplane,rotation,flashlight,brightness");
	set(&k[2], "timeout", "180");
	set(&k[3], "menu_speed", "75");
	assert(config_keys_changed(k, 4) == 0);
	snprintf(k[0].value, sizeof(k[0].value), "crimson");
	assert(config_keys_changed(k, 4) == 1);
	assert(!strcmp(merge(hand, k, 4),
		"# ztMETRO configuration\n"
		"accent = crimson\n"
		"quick = wifi,audio,settings\n"
		"timeout=120\n"
		"menu_speed=60\n"
		"unknown_key = 1\n"));

	/* the pull-down set haptics after we loaded: it stays */
	set(&k[0], "accent", "cobalt");
	set(&k[1], "haptics", "on");
	snprintf(k[0].value, sizeof(k[0].value), "lime");
	assert(!strcmp(merge("haptics = off\naccent = cobalt\n", k, 2), "haptics = off\naccent = lime\n"));

	/* after a write the new value is the one to compare with */
	config_keys_written(k, 2);
	assert(config_keys_changed(k, 2) == 0);
	assert(!strcmp(k[0].loaded, "lime"));

	/* a key not set yet is added only once it changes */
	set(&k[0], "accent", "cobalt");
	set(&k[1], "lock", "on");
	snprintf(k[1].value, sizeof(k[1].value), "off");
	assert(!strcmp(merge("accent=x\n", k, 2), "accent=x\nlock = off\n"));
	assert(!strcmp(merge(NULL, k, 2), "lock = off\n"));

	/* a changed key's repeats go; an unchanged key's lines are not ours to tidy */
	set(&k[0], "a", "1");
	set(&k[1], "b", "1");
	snprintf(k[0].value, sizeof(k[0].value), "2");
	assert(!strcmp(merge("a = 0\nb = 5\na = 9\nb = 6\n", k, 2), "a = 2\nb = 5\nb = 6\n"));

	/* a key that only starts the same is another key; comments are not keys */
	set(&k[0], "lock", "on");
	snprintf(k[0].value, sizeof(k[0].value), "off");
	assert(!strcmp(merge("lock_wallpaper = /x\n#lock = on\n lock\t= on\n", k, 1),
		"lock_wallpaper = /x\n#lock = on\nlock = off\n"));

	/* a last line with no newline does not swallow what is added after it */
	set(&k[0], "a", "1");
	snprintf(k[0].value, sizeof(k[0].value), "2");
	assert(!strcmp(merge("z = 1", k, 1), "z = 1\na = 2\n"));

	/* the same value, written anyway when asked to */
	set(&k[0], "brightness", "40");
	assert(config_keys_changed(k, 1) == 0);
	k[0].force = true;
	assert(config_keys_changed(k, 1) == 1);
	assert(!strcmp(merge("brightness = 22\n", k, 1), "brightness = 40\n"));
	config_keys_written(k, 1);
	assert(!k[0].force && config_keys_changed(k, 1) == 0);

	/* a line longer than the buffer passes whole, and its rest is not taken for a key */
	static char huge[CONFIG_LINE_MAX * 3], expect[CONFIG_LINE_MAX * 4];
	memset(huge, 'x', CONFIG_LINE_MAX + 50);
	strcpy(huge + CONFIG_LINE_MAX + 50, "accent = no\n");
	set(&k[0], "accent", "cobalt");
	snprintf(k[0].value, sizeof(k[0].value), "lime");
	snprintf(expect, sizeof(expect), "%saccent = lime\n", huge);
	assert(!strcmp(merge(huge, k, 1), expect));

	puts("PASS: Settings writes only the keys it changed");
	return 0;
}
