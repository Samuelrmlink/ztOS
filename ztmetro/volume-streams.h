/* The three volumes: Media, Calls + notifications (the ring) and Alarm.
 *
 * Media is the active output's own volume (ztos-audiod, 3.0a6): what the
 * keys have always moved, and what a Bluetooth speaker's buttons move.
 *
 * The ring and the alarm are levels of their own, kept where they always were:
 *	ring	~/.config/ztmetro/config	"ring_volume = N"
 *	alarm	~/.config/ztclock/settings	"volume N"
 * Every sound at one of them plays as its own PipeWire stream, marked with
 * node properties (pw-play -P, or a Pulse proplist):
 *	media.role		Notification or Alarm
 *	ztos.stream		ring or alarm
 *	ztos.volume.share	what part of the level this sound takes (the
 *				alarm's ramp, a key's tap): 1 if absent
 *	ztos.volume.boost	true: the output is turned up while it plays if
 *				it can't be heard at its level otherwise
 *	state.restore-props	false: WirePlumber neither keeps nor restores it
 *
 * Levels are percent on wpctl's scale, as Media's: the gain is the cube.
 * The output's volume is applied after the stream's, so a ring stream's own
 * gain is its level over the output's: the ring sounds the same whatever
 * Media is at. That works in floating point all the way through the speaker's
 * chain (so above 1 is fine, to VS_GAIN_MAX: +40 dB); a Bluetooth output's volume is
 * the device's own, applied after the encoder, so there the gain stops at 1
 * and ztos-audiod turns the output up for a ring or an alarm instead
 * (ztos.volume.boost).
 *
 * The program starting a sound sets its volume from this (so the first moment
 * is right); ztos-audiod (rootfs/overlay/usr/local/bin/ztos-audiod, the same
 * formulas) keeps it right while it plays: a level or the output's volume
 * changing.
 *
 * The alarm never goes under VS_ALARM_MIN: an alarm left at 0 by accident
 * would not wake anyone. Its vibration is the alarm's own setting.
 *
 * Kept apart from the programs so the host can test it (tests/volume-streams.c).
 */
#ifndef ZT_VOLUME_STREAMS_H
#define ZT_VOLUME_STREAMS_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum vs_stream { VS_MEDIA, VS_RING, VS_ALARM, VS_STREAMS };

#define VS_RING_DEFAULT		70
#define VS_ALARM_DEFAULT	70	/* clock-store.h's CLOCK_VOLUME_DEFAULT */
#define VS_ALARM_MIN		20
#define VS_GAIN_MAX		100.0	/* a stream's own gain, through a software volume (+40 dB) */
#define VS_VOLUME_MAX		10.0	/* what PipeWire takes for "volume" and each channelVolume */
#define VS_GAIN_MAX_HW		1.0	/* through an output whose volume is the device's */

static inline const char *vs_label(enum vs_stream s) {
	return s == VS_RING ? "Calls + notifications" : s == VS_ALARM ? "Alarm" : "Media";
}

static inline const char *vs_word(enum vs_stream s) {
	return s == VS_RING ? "ring" : s == VS_ALARM ? "alarm" : "media";
}

static inline const char *vs_role(enum vs_stream s) {
	return s == VS_ALARM ? "Alarm" : s == VS_RING ? "Notification" : "Music";
}

static inline int vs_clamp(enum vs_stream s, int percent) {
	int low = s == VS_ALARM ? VS_ALARM_MIN : 0;
	return percent < low ? low : percent > 100 ? 100 : percent;
}

static inline double vs_cube(double percent) {
	double x = percent / 100.0;
	return x * x * x;
}

/* The output a stream plays through, as ztos-audiod last wrote it ("active"). */
struct vs_device {
	bool known;
	int percent;
	bool mute;
	bool hardware;	/* its volume is the device's own (Bluetooth) */
};

/* A stream's own gain (linear, PipeWire's) for a level, through that output. */
static inline double vs_gain(int percent, const struct vs_device *d) {
	double target = vs_cube(percent);
	if (percent <= 0) return 0.0;
	if (!d || !d->known) return target;	/* nothing known: as before, under the output's volume */
	double cap = d->hardware ? VS_GAIN_MAX_HW : VS_GAIN_MAX;
	double out = d->mute ? 0.0 : vs_cube(d->percent);
	if (out <= 0.0) return cap;
	double g = target / out;
	return g > cap ? cap : g;
}

/* Whether the output is too low to carry the level even at the stream's most:
 * a sound with ztos.volume.boost has it turned up (ztos-audiod). */
static inline bool vs_needs_boost(int percent, const struct vs_device *d) {
	if (percent <= 0 || !d || !d->known) return false;
	double cap = d->hardware ? VS_GAIN_MAX_HW : VS_GAIN_MAX;
	double out = d->mute ? 0.0 : vs_cube(d->percent);
	return out * cap < vs_cube(percent) * 0.97;
}

/* "active" out of ztos-audio-state.json: {"name", "desc", "kind", "percent", "mute"} or null. */
static inline bool vs_device_parse(const char *json, struct vs_device *d) {
	memset(d, 0, sizeof(*d));
	const char *a = json ? strstr(json, "\"active\":") : NULL;
	if (!a) return false;
	a += 9;
	while (*a == ' ') a++;
	if (*a != '{') return false;
	const char *end = strchr(a, '}');
	if (!end) return false;
	const char *p = strstr(a, "\"percent\":");
	if (!p || p > end) return false;
	p += 10;
	while (*p == ' ') p++;
	if (*p < '0' || *p > '9') return false;
	d->percent = atoi(p);
	const char *m = strstr(a, "\"mute\":");
	if (m && m < end) {
		m += 7;
		while (*m == ' ') m++;
		d->mute = !strncmp(m, "true", 4);
	}
	const char *k = strstr(a, "\"kind\":");
	if (k && k < end) {
		k += 7;
		while (*k == ' ') k++;
		d->hardware = !strncmp(k, "\"bluetooth\"", 11);
	}
	d->known = true;
	return true;
}

static inline bool vs_device_read(struct vs_device *d) {
	char path[300], text[16384];
	const char *run = getenv("XDG_RUNTIME_DIR");
	memset(d, 0, sizeof(*d));
	snprintf(path, sizeof(path), "%s/ztos-audio-state.json", run ? run : "/run/user/1000");
	FILE *f = fopen(path, "r");
	if (!f) return false;
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	return vs_device_parse(text, d);
}

/* ---- the levels, where they are kept ------------------------------------- */
static inline void vs_path(enum vs_stream s, char *out, size_t size) {
	const char *home = getenv("HOME");
	snprintf(out, size, "%s/.config/%s", home ? home : "", s == VS_ALARM ? "ztclock/settings" : "ztmetro/config");
}

/* One line of either file: "ring_volume = N" or "volume N". */
static inline bool vs_line_level(enum vs_stream s, const char *line, int *out) {
	int v;
	char key[32];
	if (s == VS_RING) {
		if (sscanf(line, " ring_volume = %d", &v) != 1) return false;
	} else if (s == VS_ALARM) {
		if (sscanf(line, "%31s %d", key, &v) != 2 || strcmp(key, "volume")) return false;
	} else {
		return false;
	}
	*out = v;
	return true;
}

static inline int vs_level_read(enum vs_stream s) {
	char path[400], line[256];
	int level = s == VS_ALARM ? VS_ALARM_DEFAULT : VS_RING_DEFAULT, v;
	vs_path(s, path, sizeof(path));
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f))
		if (vs_line_level(s, line, &v)) level = v;
	if (f) fclose(f);
	return vs_clamp(s, level);
}

/* The alarm's level into ztclock's settings, the rest of the file as it was.
 * (The ring's is the shell's config, which config_keep writes.) */
static inline bool vs_alarm_write(int percent) {
	char path[400], tmp[410], line[256];
	int v;
	vs_path(VS_ALARM, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
	if (!out) {
		if (in) fclose(in);
		return false;
	}
	while (in && fgets(line, sizeof(line), in))
		if (!vs_line_level(VS_ALARM, line, &v)) fputs(line, out);
	fprintf(out, "volume %d\n", vs_clamp(VS_ALARM, percent));
	if (in) fclose(in);
	return fclose(out) == 0 && rename(tmp, path) == 0;
}

/* ---- starting a sound on one --------------------------------------------- */
/* pw-play's --volume for a sound taking share of the stream's level */
static inline double vs_play_volume_at(int percent, double share) {
	struct vs_device d;
	vs_device_read(&d);
	double g = vs_gain(percent, &d) * share;
	return g > VS_VOLUME_MAX ? VS_VOLUME_MAX : g;	/* ztos-audiod adds the rest on the channels */
}

static inline double vs_play_volume(enum vs_stream s, double share) {
	return vs_play_volume_at(vs_level_read(s), share);
}

/* pw-play's -P */
static inline void vs_play_props(enum vs_stream s, double share, bool boost, char *out, size_t size) {
	snprintf(out, size, "{ ztos.stream = %s ztos.volume.share = %.3f ztos.volume.boost = %s state.restore-props = false }",
		vs_word(s), share, boost ? "true" : "false");
}

/* exec pw-play for a file on a stream (in a child: it returns only if exec fails) */
static inline void vs_exec_play_at(enum vs_stream s, int percent, double share, bool boost, const char *path) {
	char volume[24], props[200];
	snprintf(volume, sizeof(volume), "%.4f", vs_play_volume_at(vs_clamp(s, percent), share));
	vs_play_props(s, share, boost, props, sizeof(props));
	execlp("pw-play", "pw-play", "--media-role", vs_role(s), "--volume", volume, "-P", props, path, (char *)NULL);
}

static inline void vs_exec_play(enum vs_stream s, double share, bool boost, const char *path) {
	vs_exec_play_at(s, vs_level_read(s), share, boost, path);
}

/* ---- which one the volume keys move -------------------------------------- */
/* A playback stream counts as media playing unless it is one of these: the
 * ring's and the alarm's own (ztos.stream), system sounds and calls by their
 * role (Pulse names them: Notification is "event", Communication "phone"), and
 * ztOS's own routing (ztos-speaker.out, ztos-eq: node names "ztos-"), which
 * runs whenever anything plays. A corked (paused) stream plays nothing. */
static inline bool vs_counts_as_media(const char *role, const char *ztos_stream, const char *node_name, bool corked) {
	if (corked) return false;
	if (ztos_stream && *ztos_stream) return false;
	if (node_name && !strncmp(node_name, "ztos-", 5)) return false;
	if (role) {
		static const char *const not_media[] = { "event", "phone", "Notification", "Communication", "Alarm", "alarm" };
		for (size_t i = 0; i < sizeof(not_media) / sizeof(not_media[0]); i++)
			if (!strcmp(role, not_media[i])) return false;
	}
	return true;
}

/* The stream a first press of a volume key moves. An alarm ringing: its own
 * level (the ring's would change nothing that can be heard). Then media if
 * anything plays: ztos-sounds' count when it is running, else what MPRIS says. */
static inline enum vs_stream vs_key_stream(bool alarm_ringing, bool audio_known, bool audio_active, bool media_playing) {
	if (alarm_ringing) return VS_ALARM;
	if (audio_known ? audio_active : media_playing) return VS_MEDIA;
	return VS_RING;
}
#endif
