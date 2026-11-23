/* make check-statusbar: the status bar's order, visibility and per-state
 * colours (statusbar.h) as the file and the control pipe give them, and
 * their keys written through config-merge.h without a SIGHUP. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../statusbar.h"
#include "../settings/config-merge.h"

#define INK 0xffffff
#define DIM 0x808080
#define ACC 0x1ba1e2

static uint32_t bt(const struct zt_statusbar *s, int state, bool *shown) {
	uint32_t c = 0x123456;
	*shown = sb_resolve(s, SB_COLOURED_BT, state, INK, DIM, ACC, &c);
	return c;
}

static bool is_sb(const char *key) {
	return sb_key_index(key) >= 0;
}

int main(void) {
	struct zt_statusbar s;
	sb_defaults(&s);
	char out[256];
	bool shown;

	/* defaults: the bar from before 8.77 */
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "signal,data,wifi,location,bluetooth") == 0);
	assert(bt(&s, SB_BT_OFF, &shown) && !shown);
	assert(bt(&s, SB_BT_ON, &shown) == DIM && shown);
	assert(bt(&s, SB_BT_CONNECTED, &shown) == INK && shown);
	assert(bt(&s, SB_BT_STREAMING, &shown) == INK && shown);
	assert(s.bt_stream == SB_STREAM_DOT && s.bt_dot == SB_C_ACCENT);

	/* the states from ztos-bt */
	assert(sb_bt_state(false, true, true) == SB_BT_OFF);
	assert(sb_bt_state(true, false, false) == SB_BT_ON);
	assert(sb_bt_state(true, true, false) == SB_BT_CONNECTED);
	assert(sb_bt_state(true, true, true) == SB_BT_STREAMING);

	/* order and visibility; unknown, fixed and repeated names passed over,
	 * the missing ones at the end, shown */
	sb_icons_parse(&s, "bluetooth, -wifi,clock,bogus,bluetooth,signal");
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "bluetooth,-wifi,signal,data,location") == 0);
	assert(!s.shown[SB_WIFI] && s.shown[SB_BT] && s.shown[SB_BATTERY] && s.shown[SB_CLOCK]);
	assert(sb_place(&s, SB_BT) == 0 && sb_place(&s, SB_CLOCK) == -1);
	sb_icons_parse(&s, "");
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "signal,data,wifi,location,bluetooth") == 0);
	/* (an older file's one icon: the rest follow) */
	sb_icons_parse(&s, "!location");
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "-location,signal,data,wifi,bluetooth") == 0);

	/* drag: moved, the ones between move along */
	sb_defaults(&s);
	sb_move(&s, 4, 0);
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "bluetooth,signal,data,wifi,location") == 0);
	sb_move(&s, 0, 4);
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "signal,data,wifi,location,bluetooth") == 0);
	sb_move(&s, 1, 2);
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "signal,wifi,data,location,bluetooth") == 0);
	sb_move(&s, 1, 9);	/* out of range: nothing */
	sb_move(&s, -1, 0);
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "signal,wifi,data,location,bluetooth") == 0);

	/* colours: names, #rrggbb, bad values ignored */
	int c;
	assert(sb_colour_parse("green", &c) && c == 0x60a917);
	assert(sb_colour_parse("#00ff00", &c) && c == 0x00ff00);
	assert(sb_colour_parse("00FF00", &c) && c == 0x00ff00);
	assert(sb_colour_parse("white", &c) && c == SB_C_INK);
	assert(sb_colour_parse("Accent", &c) && c == SB_C_ACCENT);
	assert(!sb_colour_parse("#12345", &c) && !sb_colour_parse("bogus", &c) && !sb_colour_parse("", &c));
	sb_colour_format(0x00ff00, out, sizeof(out));
	assert(strcmp(out, "#00ff00") == 0);
	sb_colour_format(0x60a917, out, sizeof(out));
	assert(strcmp(out, "green") == 0);

	/* every key round-trips, and only these are taken */
	sb_defaults(&s);
	assert(sb_n_keys() == 7);
	assert(sb_key_index("statusbar_bt_streaming") >= 0 && sb_key_index("statusbar_bt_bogus") < 0);
	assert(sb_key_index("tile_opacity") < 0);
	assert(!sb_parse(&s, "tile_opacity", "50"));
	assert(sb_parse(&s, "statusbar_bt_connected", "green"));
	assert(sb_parse(&s, "statusbar_bt_streaming", "nonsense"));	/* taken, ignored */
	assert(sb_parse(&s, "statusbar_bt_stream", "pulse"));
	assert(sb_parse(&s, "statusbar_bt_dot", "#ff8800"));
	assert(sb_parse(&s, "statusbar_bt_off", "dim"));
	assert(s.bt_stream == SB_STREAM_PULSE && s.bt_dot == 0xff8800);
	/* streaming follows connected while it is default */
	assert(bt(&s, SB_BT_CONNECTED, &shown) == 0x60a917 && shown);
	assert(bt(&s, SB_BT_STREAMING, &shown) == 0x60a917 && shown);
	assert(bt(&s, SB_BT_OFF, &shown) == DIM && shown);
	assert(sb_parse(&s, "statusbar_bt_streaming", "accent"));
	assert(bt(&s, SB_BT_STREAMING, &shown) == ACC);
	assert(sb_parse(&s, "statusbar_bt_on", "hidden"));
	bt(&s, SB_BT_ON, &shown);
	assert(!shown);
	for (int k = 0; k < sb_n_keys(); k++) {
		struct zt_statusbar t;
		sb_defaults(&t);
		char v[256];
		sb_format(&s, k, v, sizeof(v));
		assert(sb_parse(&t, sb_key_name(k), v));
		char w[256];
		sb_format(&t, k, w, sizeof(w));
		assert(strcmp(v, w) == 0);
	}
	assert(sb_parse(&s, "statusbar_bt_stream", "color") && s.bt_stream == SB_STREAM_COLOUR);
	assert(sb_parse(&s, "statusbar_bt_stream", "wobble") && s.bt_stream == SB_STREAM_COLOUR);

	/* the pipe: changed or not */
	sb_defaults(&s);
	assert(sb_line(&s, "statusbar_bt_streaming green\n"));
	assert(!sb_line(&s, "statusbar_bt_streaming green"));
	assert(!sb_line(&s, "tile_opacity 50"));
	assert(!sb_line(&s, "statusbar_bt_streaming"));
	assert(sb_line(&s, "statusbar_icons wifi,-signal"));
	sb_icons_format(&s, out, sizeof(out));
	assert(strcmp(out, "wifi,-signal,data,location,bluetooth") == 0);

	/* the pulse: smooth, full at its start, never below its floor */
	assert(fabs(sb_pulse_alpha(0, SB_PULSE_MS) - 1.0) < 1e-9);
	assert(fabs(sb_pulse_alpha(SB_PULSE_MS / 2, SB_PULSE_MS) - SB_PULSE_LOW) < 1e-9);
	double prev = sb_pulse_alpha(0, SB_PULSE_MS);
	for (double t = 0; t < 3 * SB_PULSE_MS; t += SB_PULSE_TICK_MS) {
		double a = sb_pulse_alpha(t, SB_PULSE_MS);
		assert(a >= SB_PULSE_LOW - 1e-9 && a <= 1.0 + 1e-9);
		/* no step at the redraw rate is a blink */
		assert(fabs(a - prev) < 0.07);
		prev = a;
	}

	/* written without a SIGHUP: only these keys changed */
	struct config_key keys[8];
	int n = 0;
	sb_defaults(&s);
	for (int k = 0; k < sb_n_keys(); k++, n++) {
		keys[n].key = sb_key_name(k);
		sb_format(&s, k, keys[n].value, sizeof(keys[n].value));
		strcpy(keys[n].loaded, keys[n].value);
		keys[n].force = false;
	}
	keys[n].key = "tile_opacity";
	strcpy(keys[n].value, "100");
	strcpy(keys[n].loaded, "100");
	keys[n++].force = false;
	assert(!config_only_changed_if(keys, n, is_sb));
	strcpy(keys[3].value, "green");
	assert(config_only_changed_if(keys, n, is_sb));
	strcpy(keys[n - 1].value, "90");
	assert(!config_only_changed_if(keys, n, is_sb));

	/* an older file with none of the keys: the defaults, which the merge
	 * leaves out of the file until one changes */
	static char result[4096];
	const char *old = "accent = cobalt\ntheme = dark\n";
	FILE *in = fmemopen((void *)old, strlen(old), "r"), *o = fmemopen(result, sizeof(result), "w");
	for (int i = 0; i < n; i++) strcpy(keys[i].value, keys[i].loaded);
	config_merge(in, o, keys, n);
	fclose(in);
	fclose(o);
	assert(strstr(result, "statusbar_") == NULL);

	puts("statusbar: ok");
	return 0;
}
