/* volume-streams.h on the host: which stream the keys move, what counts as
 * media playing, the gains, and the levels kept across restarts.
 * make check-volume-streams */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../volume-streams.h"

static bool near(double a, double b) {
	return fabs(a - b) <= 1e-6 * (fabs(b) > 1 ? fabs(b) : 1);
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static char *read_file(const char *path) {
	static char buf[4096];
	FILE *f = fopen(path, "r");
	assert(f);
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

int main(void) {
	/* ---- what counts as media playing */
	assert(vs_counts_as_media("Music", NULL, "mpv", false));
	assert(vs_counts_as_media(NULL, NULL, "Brave", false));	/* a browser sets no role */
	assert(vs_counts_as_media("video", NULL, NULL, false));
	assert(!vs_counts_as_media("Music", NULL, "mpv", true));	/* paused */
	assert(!vs_counts_as_media("event", NULL, NULL, false));	/* a system sound, Pulse's name */
	assert(!vs_counts_as_media("Notification", NULL, NULL, false));
	assert(!vs_counts_as_media("phone", NULL, NULL, false));	/* a call */
	assert(!vs_counts_as_media("Alarm", NULL, NULL, false));
	/* the ring's and alarm's own pw-play: once role Music, now marked */
	assert(!vs_counts_as_media("Music", "ring", "pw-play", false));
	assert(!vs_counts_as_media("Alarm", "alarm", "pw-play", false));
	assert(!vs_counts_as_media(NULL, NULL, "ztos-speaker.out", false));	/* the routing */
	assert(!vs_counts_as_media(NULL, NULL, "ztos-eq.out", false));

	/* ---- which stream the keys move */
	assert(vs_key_stream(false, true, false, false) == VS_RING);	/* nothing plays */
	assert(vs_key_stream(false, true, true, false) == VS_MEDIA);	/* something plays */
	assert(vs_key_stream(false, true, false, true) == VS_RING);	/* MPRIS says playing, PipeWire says not: PipeWire */
	assert(vs_key_stream(false, false, false, true) == VS_MEDIA);	/* no ztos-sounds: MPRIS */
	assert(vs_key_stream(false, false, false, false) == VS_RING);
	assert(vs_key_stream(true, true, true, true) == VS_ALARM);	/* an alarm ringing */

	/* ---- the levels */
	assert(vs_clamp(VS_ALARM, 0) == VS_ALARM_MIN && vs_clamp(VS_ALARM, 5) == VS_ALARM_MIN);
	assert(vs_clamp(VS_ALARM, 55) == 55 && vs_clamp(VS_RING, 0) == 0 && vs_clamp(VS_RING, 130) == 100);
	assert(!strcmp(vs_role(VS_ALARM), "Alarm") && !strcmp(vs_role(VS_RING), "Notification"));

	/* ---- the gains */
	struct vs_device spk = { true, 30, false, false }, bt = { true, 30, false, true };
	assert(near(vs_gain(57, &spk) * vs_cube(30), vs_cube(57)));
	assert(near(vs_gain(57, &bt), 1.0) && vs_needs_boost(57, &bt));
	assert(!vs_needs_boost(57, &spk));
	struct vs_device low = { true, 5, false, false }, muted = { true, 50, true, false };
	assert(vs_gain(57, &low) == VS_GAIN_MAX && vs_needs_boost(57, &low));
	assert(vs_gain(57, &muted) == VS_GAIN_MAX && vs_needs_boost(20, &muted));
	assert(near(vs_gain(57, NULL), vs_cube(57)) && vs_gain(0, &spk) == 0.0);

	/* ---- ztos-audiod's state */
	struct vs_device d;
	assert(vs_device_parse("{\"version\": 1, \"active\": {\"name\": \"ztos-speaker\", \"desc\": \"Speaker\", "
		"\"kind\": \"speaker\", \"percent\": 34, \"mute\": false}, \"outputs\": []}", &d));
	assert(d.known && d.percent == 34 && !d.mute && !d.hardware);
	assert(vs_device_parse("{\"active\": {\"name\": \"bluez_output.X\", \"kind\": \"bluetooth\", \"percent\": 7, \"mute\": true}}", &d));
	assert(d.hardware && d.mute && d.percent == 7);
	assert(!vs_device_parse("{\"version\": 1, \"active\": null, \"outputs\": []}", &d) && !d.known);
	assert(!vs_device_parse("{\"active\": {\"name\": \"x\", \"percent\": null}}", &d));
	assert(!vs_device_parse(NULL, &d));

	/* ---- kept in their files, across a restart */
	char home[] = "/tmp/vs-test-XXXXXX", path[300];
	assert(mkdtemp(home));
	setenv("HOME", home, 1);
	snprintf(path, sizeof(path), "%s/.config", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/ztmetro", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/ztclock", home);
	mkdir(path, 0755);
	assert(vs_level_read(VS_RING) == VS_RING_DEFAULT && vs_level_read(VS_ALARM) == VS_ALARM_DEFAULT);
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home);
	write_file(path, "accent = red\nring_volume = 57\nmedia_volume = 30\n");
	assert(vs_level_read(VS_RING) == 57);
	snprintf(path, sizeof(path), "%s/.config/ztclock/settings", home);
	write_file(path, "volume 35\nautostop 1\n");
	assert(vs_level_read(VS_ALARM) == 35);
	assert(vs_alarm_write(64));
	assert(vs_level_read(VS_ALARM) == 64);
	assert(strstr(read_file(path), "autostop 1\n") && !strstr(read_file(path), "volume 35"));
	assert(vs_alarm_write(3));	/* the floor is kept on the way in too */
	assert(vs_level_read(VS_ALARM) == VS_ALARM_MIN && strstr(read_file(path), "volume 20\n"));
	write_file(path, "volume 0\n");	/* the clock app's slider can still say 0 */
	assert(vs_level_read(VS_ALARM) == VS_ALARM_MIN);
	unlink(path);
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home);
	unlink(path);

	/* ---- what pw-play is given */
	char props[200];
	vs_play_props(VS_ALARM, 0.3, true, props, sizeof(props));
	assert(strstr(props, "ztos.stream = alarm") && strstr(props, "ztos.volume.share = 0.300") &&
		strstr(props, "ztos.volume.boost = true") && strstr(props, "state.restore-props = false"));
	setenv("XDG_RUNTIME_DIR", home, 1);	/* no ztos-audiod state: the level alone */
	assert(near(vs_play_volume_at(57, 0.5), vs_cube(57) * 0.5));
	snprintf(path, sizeof(path), "%s/ztos-audio-state.json", home);
	write_file(path, "{\"active\": {\"kind\": \"speaker\", \"percent\": 5, \"mute\": false}}");
	assert(vs_play_volume_at(57, 1.0) == VS_VOLUME_MAX);	/* the rest from ztos-audiod */
	unlink(path);
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/.config/ztmetro", home); rmdir(dir);
	snprintf(dir, sizeof(dir), "%s/.config/ztclock", home); rmdir(dir);
	snprintf(dir, sizeof(dir), "%s/.config", home); rmdir(dir);
	rmdir(home);
	printf("volume-streams: ok\n");
	return 0;
}
