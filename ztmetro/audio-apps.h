/* What ztos-audiod says about each app, for the Sound app's "apps" page: reading
 * the state file's "apps", the choices of output, the line under an app's name, and
 * the commands. Kept apart from the app so the host can test it (tests/audio-apps.c).
 * As in audio-state.h, the file comes from outside: anything malformed or cut
 * short is refused whole and the last good list stands. */
#ifndef ZT_AUDIO_APPS_H
#define ZT_AUDIO_APPS_H
#include "audio-state.h"

#define AUDIO_MAX_APPS 24
#define AUDIO_APP_CHOICES (AUDIO_MAX_OUTPUTS + 2)	/* Default, the outputs, one that's away */

struct audio_app {
	char key[256], name[96];
	char output[160];	/* "default" or an output's name */
	char output_desc[96];
	char playing_on[160];	/* "" when it isn't linked to anything */
	int percent;		/* 0..100, -1 not known */
	bool mute, running, output_present;
};

struct audio_apps {
	int n;
	struct audio_app app[AUDIO_MAX_APPS];
};

/* One of "apps". *usable needs a whole key and output, since both are sent back to
 * the daemon and a shortened one would be some other app, or none. */
static inline bool aj_app(struct audio_json *j, struct audio_app *a, bool *usable) {
	memset(a, 0, sizeof(*a));
	a->percent = -1;
	*usable = false;
	if (!aj_eat(j, '{')) return false;
	if (aj_eat(j, '}')) return true;
	bool key_cut = false, out_cut = false, cut;
	do {
		char field[16];
		if (!aj_string(j, field, sizeof(field), &cut) || !aj_eat(j, ':')) return false;
		bool ok;
		if (cut) ok = aj_skip(j, AUDIO_JSON_DEPTH);
		else if (!strcmp(field, "key")) ok = aj_text_field(j, a->key, sizeof(a->key), &key_cut);
		else if (!strcmp(field, "name")) ok = aj_text_field(j, a->name, sizeof(a->name), &cut);
		else if (!strcmp(field, "output")) ok = aj_text_field(j, a->output, sizeof(a->output), &out_cut);
		else if (!strcmp(field, "output_desc")) ok = aj_text_field(j, a->output_desc, sizeof(a->output_desc), &cut);
		else if (!strcmp(field, "playing_on")) ok = aj_text_field(j, a->playing_on, sizeof(a->playing_on), &cut);
		else if (!strcmp(field, "percent")) ok = aj_percent_field(j, &a->percent);
		else if (!strcmp(field, "mute")) ok = aj_bool_field(j, &a->mute);
		else if (!strcmp(field, "running")) ok = aj_bool_field(j, &a->running);
		else if (!strcmp(field, "output_present")) ok = aj_bool_field(j, &a->output_present);
		else ok = aj_skip(j, AUDIO_JSON_DEPTH);
		if (!ok) return false;
	} while (aj_eat(j, ','));
	if (!aj_eat(j, '}')) return false;
	if (!a->output[0]) snprintf(a->output, sizeof(a->output), "default");
	if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%.95s", a->key);
	*usable = a->key[0] && !key_cut && !out_cut;
	return true;
}

/* The state file's text: out is replaced only if all of it is good. A daemon too old
 * to send "apps" is a good file with no apps. */
static inline bool audio_apps_parse(const char *text, size_t len, struct audio_apps *out) {
	struct audio_json j = { text, text + len };
	static struct audio_apps fresh;	/* a large thing for a stack, and one UI thread */
	fresh.n = 0;
	if (len > AUDIO_STATE_MAX_BYTES || !aj_eat(&j, '{')) return false;
	do {
		char key[16];
		bool cut;
		if (!aj_string(&j, key, sizeof(key), &cut) || !aj_eat(&j, ':')) return false;
		if (!cut && !strcmp(key, "version")) {
			int v = 0;
			if (!aj_number(&j, &v) || v != 1) return false;
		} else if (!cut && !strcmp(key, "apps")) {
			fresh.n = 0;
			if (!aj_eat(&j, '[')) return false;
			if (aj_eat(&j, ']')) continue;
			do {
				struct audio_app a;
				bool usable;
				if (!aj_app(&j, &a, &usable)) return false;
				if (usable && fresh.n < AUDIO_MAX_APPS) fresh.app[fresh.n++] = a;
			} while (aj_eat(&j, ','));
			if (!aj_eat(&j, ']')) return false;
		} else if (!aj_skip(&j, AUDIO_JSON_DEPTH)) {
			return false;
		}
	} while (aj_eat(&j, ','));
	if (!aj_eat(&j, '}')) return false;
	aj_ws(&j);
	if (j.p != j.end) return false;
	*out = fresh;
	return true;
}

static inline bool audio_app_is_default(const struct audio_app *a) {
	return !strcmp(a->output, "default");
}

/* ---- the output picker --------------------------------------------------- */
struct audio_app_choice {
	char label[128];
	char name[160];		/* what to send: "default" or an output's name */
};

/* Default first, saying where that is now; then the outputs; then the one this app
 * is set to if it's away, so the picker can still show the choice. Returns how many;
 * *selected is the app's. */
static inline int audio_app_choices(const struct audio_state *st, const struct audio_app *a,
		struct audio_app_choice *c, int *selected) {
	int n = 0, cur = audio_current(st);
	*selected = 0;
	if (cur >= 0) snprintf(c[n].label, sizeof(c[n].label), "Default (%.100s)", st->out[cur].desc);
	else snprintf(c[n].label, sizeof(c[n].label), "Default");
	snprintf(c[n++].name, sizeof(c[0].name), "default");
	for (int i = 0; i < st->n; i++) {
		snprintf(c[n].label, sizeof(c[n].label), "%.100s", st->out[i].desc[0] ? st->out[i].desc : st->out[i].name);
		snprintf(c[n].name, sizeof(c[n].name), "%s", st->out[i].name);
		if (!strcmp(st->out[i].name, a->output)) *selected = n;
		n++;
	}
	if (!audio_app_is_default(a) && audio_find(st, a->output) < 0) {
		snprintf(c[n].label, sizeof(c[n].label), "%.100s (not connected)", a->output_desc[0] ? a->output_desc : a->output);
		snprintf(c[n].name, sizeof(c[n].name), "%s", a->output);
		*selected = n++;
	}
	return n;
}

/* The line under an app's name. */
static inline void audio_app_status(const struct audio_state *st, const struct audio_app *a, char *out, size_t cap) {
	int on = a->playing_on[0] ? audio_find(st, a->playing_on) : -1;
	const char *where = on >= 0 ? (st->out[on].desc[0] ? st->out[on].desc : st->out[on].name) : NULL;
	bool away = !audio_app_is_default(a) && audio_find(st, a->output) < 0;
	const char *wanted = a->output_desc[0] ? a->output_desc : a->output;
	if (a->running && where && away) snprintf(out, cap, "playing on %s until %s returns", where, wanted);
	else if (a->running && where) snprintf(out, cap, "playing on %s", where);
	else if (a->running) snprintf(out, cap, "open, not linked to an output");
	else if (away) snprintf(out, cap, "not playing, %s isn't connected", wanted);
	else snprintf(out, cap, "not playing");
}

/* ---- the daemon's commands ----------------------------------------------- */
/* Words split as a shell does. An app's name is the app's to choose, quotes included. */
static inline bool audio_shell_quote(char *out, size_t cap, const char *s) {
	size_t at = 0;
	if (cap < 3) return false;
	out[at++] = '\'';
	for (; *s; s++) {
		if (*s == '\n') return false;
		if (*s == '\'') {
			if (at + 4 >= cap) return false;
			memcpy(out + at, "'\\''", 4);
			at += 4;
		} else {
			if (at + 1 >= cap) return false;
			out[at++] = *s;
		}
	}
	if (at + 2 > cap) return false;
	out[at++] = '\'';
	out[at] = 0;
	return true;
}

/* The longest: a key and an output, each with every character a quote. */
#define AUDIO_APP_CMD_MAX (32 + 4 * 256 + 4 * 160)

static inline bool audio_cmd_app(char *line, size_t cap, const char *cmd, const char *key, const char *value) {
	char k[4 * 256 + 3], v[4 * 160 + 3];
	if (!audio_shell_quote(k, sizeof(k), key) || !audio_shell_quote(v, sizeof(v), value)) return false;
	int n = snprintf(line, cap, "%s %s %s", cmd, k, v);
	return n > 0 && (size_t)n < cap;
}

static inline bool audio_cmd_app_volume(char *line, size_t cap, const char *key, int percent) {
	char v[16];
	snprintf(v, sizeof(v), "%d", percent);
	return audio_cmd_app(line, cap, "app-volume", key, v);
}

static inline bool audio_cmd_app_mute(char *line, size_t cap, const char *key, bool mute) {
	return audio_cmd_app(line, cap, "app-mute", key, mute ? "1" : "0");
}

static inline bool audio_cmd_app_output(char *line, size_t cap, const char *key, const char *output) {
	return audio_cmd_app(line, cap, "app-output", key, output);
}
#endif
