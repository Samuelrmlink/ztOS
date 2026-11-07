/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Writing the config file: only what this run of Settings changed.
 *
 * The file is shared. The shell keeps keys in it too (the pull-down's haptics
 * and brightness, the volume), other versions know keys and values this one
 * does not, and people edit it by hand. So each key's value is remembered as it
 * was formatted at load; a key still at that value keeps its line as it stands,
 * whatever the line says. Kept out of ztsettings.c so that it can be tested on
 * any machine (tests/settings-config.c).
 */
#ifndef ZTSETTINGS_CONFIG_MERGE_H
#define ZTSETTINGS_CONFIG_MERGE_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_VALUE_MAX 300
#define CONFIG_LINE_MAX 1024

struct config_key {
	const char *key;
	char value[CONFIG_VALUE_MAX];	/* what it should say now */
	char loaded[CONFIG_VALUE_MAX];	/* what it said at load, or last written */
	bool force;			/* written even though it reads the same */
};

static inline bool config_key_changed(const struct config_key *k) {
	return k->force || strcmp(k->value, k->loaded) != 0;
}

static inline int config_keys_changed(const struct config_key *keys, int n) {
	int changed = 0;
	for (int i = 0; i < n; i++) changed += config_key_changed(&keys[i]);
	return changed;
}

/* The key a line sets, into `out`; false if it sets none. */
static inline bool config_line_key(const char *line, char *out, size_t size) {
	const char *eq = strchr(line, '=');
	if (line[0] == '#' || !eq) return false;
	const char *s = line, *e = eq;
	while (s < e && (*s == ' ' || *s == '\t')) s++;
	while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
	if ((size_t)(e - s) >= size) return false;
	memcpy(out, s, (size_t)(e - s));
	out[e - s] = 0;
	return true;
}

/* in (may be NULL: no file yet) copied to out, the changed keys' lines replaced:
 * in the place of the first, any repeats of it dropped, a key not there yet at
 * the end. Every other line goes through as it is. */
static inline void config_merge(FILE *in, FILE *out, struct config_key *keys, int n) {
	char line[CONFIG_LINE_MAX], name[128];
	bool written[n > 0 ? n : 1];
	bool line_start = true, ended = true;
	memset(written, 0, sizeof(written));
	while (in && fgets(line, sizeof(line), in)) {
		bool ours = false;
		if (line_start && config_line_key(line, name, sizeof(name))) {
			for (int i = 0; i < n; i++) {
				if (strcmp(name, keys[i].key) != 0 || !config_key_changed(&keys[i])) continue;
				if (!written[i]) fprintf(out, "%s = %s\n", keys[i].key, keys[i].value);
				written[i] = ours = true;
			}
		}
		size_t len = strlen(line);
		ended = len > 0 && line[len - 1] == '\n';
		line_start = ended;
		if (!ours) {
			fputs(line, out);
		} else {
			/* a line too long for the buffer: the rest of it goes too */
			while (!ended && fgets(line, sizeof(line), in)) {
				len = strlen(line);
				ended = len > 0 && line[len - 1] == '\n';
			}
			line_start = ended = true;
		}
	}
	if (!ended) fputc('\n', out);	/* a last line with no end of its own */
	for (int i = 0; i < n; i++) {
		if (!written[i] && config_key_changed(&keys[i])) {
			fprintf(out, "%s = %s\n", keys[i].key, keys[i].value);
		}
	}
}

/* The file now says what the keys say. */
static inline void config_keys_written(struct config_key *keys, int n) {
	for (int i = 0; i < n; i++) {
		memcpy(keys[i].loaded, keys[i].value, sizeof(keys[i].loaded));
		keys[i].force = false;
	}
}

#endif
