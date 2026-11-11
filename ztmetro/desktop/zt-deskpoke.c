// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zt-deskpoke: drives the desktop's pointer the way ztdesk does (a
 * wlr-virtual-pointer on the desktop's output, absolute positions), from a
 * script on the command line. For tests, on the phone and in desktop/dev.
 *
 *   zt-deskpoke OUTPUT W H 'CMD; CMD; ...'
 *
 *   move X Y          the pointer to X, Y (output points)
 *   down [B] | up [B] a button (1 left, 2 right, 3 middle; default left)
 *   drag X0 Y0 X1 Y1 [MS [STEPS]]   press at X0 Y0, move there in STEPS over MS, release
 *   click X Y [B]     press, 40 ms, release
 *   key KEY [down|up] a key by its evdev code (56 is Alt), on a virtual keyboard
 *   sleep MS
 *
 * W and H are the output's size in points ("output NAME KIND W H SCALE" in
 * $XDG_RUNTIME_DIR/ztmetro-desktop.state, divided by its scale).
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwp_virtual_keyboard_manager_v1 *vkm;
static struct zwlr_virtual_pointer_v1 *ptr;
static struct zwp_virtual_keyboard_v1 *kbd;
static struct { struct wl_output *o; char name[64]; } outs[8];
static int n_outs;
static double W, H;

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void out_name(void *data, struct wl_output *o, const char *name) {
	snprintf(((char *)data), 64, "%s", name);
}
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		int32_t sub, const char *make, const char *model, int32_t tr) {}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {}
static void out_done(void *d, struct wl_output *o) {}
static void out_scale(void *d, struct wl_output *o, int32_t s) {}
static void out_desc(void *d, struct wl_output *o, const char *s) {}
static const struct wl_output_listener out_listener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_desc,
};

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
	if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	} else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) && version >= 2) {
		vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
	} else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		vkm = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name) && version >= 4 && n_outs < 8) {
		outs[n_outs].o = wl_registry_bind(reg, name, &wl_output_interface, 4);
		wl_output_add_listener(outs[n_outs].o, &out_listener, outs[n_outs].name);
		n_outs++;
	}
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = { global, global_remove };

static void flush_sleep(int ms) {
	wl_display_roundtrip(dpy);
	if (ms > 0) usleep((useconds_t)ms * 1000);
}

static void move(double x, double y) {
	zwlr_virtual_pointer_v1_motion_absolute(ptr, now_ms(), (uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y),
		(uint32_t)W, (uint32_t)H);
	zwlr_virtual_pointer_v1_frame(ptr);
}

static uint32_t btn_code(int b) {
	return b == 2 ? BTN_RIGHT : b == 3 ? BTN_MIDDLE : BTN_LEFT;
}

static void button(int b, bool down) {
	zwlr_virtual_pointer_v1_button(ptr, now_ms(), btn_code(b),
		down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(ptr);
}

static void keyboard_setup(void) {
	if (kbd || !vkm) return;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t size = strlen(text) + 1;
	int fd = memfd_create("deskpoke-keymap", MFD_CLOEXEC);
	if (write(fd, text, size) != (ssize_t)size) {}
	kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);
	zwp_virtual_keyboard_v1_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
}

static void run(char *cmd) {
	double a, b, c, e;
	int n, m, k;
	char word[16];
	while (*cmd == ' ') cmd++;
	if (sscanf(cmd, "move %lf %lf", &a, &b) == 2) {
		move(a, b);
		flush_sleep(0);
	} else if (!strncmp(cmd, "down", 4) || !strncmp(cmd, "up", 2)) {
		int bt = 1;
		sscanf(cmd + (cmd[0] == 'd' ? 4 : 2), "%d", &bt);
		button(bt, cmd[0] == 'd');
		flush_sleep(0);
	} else if ((n = sscanf(cmd, "drag %lf %lf %lf %lf %d %d", &a, &b, &c, &e, &m, &k)) >= 4) {
		if (n < 5) m = 400;
		if (n < 6) k = 20;
		move(a, b);
		flush_sleep(30);
		button(1, true);
		flush_sleep(60);
		for (int i = 1; i <= k; i++) {
			move(a + (c - a) * i / k, b + (e - b) * i / k);
			flush_sleep(m / k);
		}
		flush_sleep(60);
		button(1, false);
		flush_sleep(30);
	} else if ((n = sscanf(cmd, "click %lf %lf %d", &a, &b, &m)) >= 2) {
		move(a, b);
		flush_sleep(30);
		button(n == 3 ? m : 1, true);
		flush_sleep(40);
		button(n == 3 ? m : 1, false);
		flush_sleep(0);
	} else if ((n = sscanf(cmd, "key %d %15s", &m, word)) >= 1) {
		keyboard_setup();
		if (!kbd) return;
		bool down = n < 2 || strcmp(word, "up");
		bool up = n < 2 || !strcmp(word, "up");
		if (down) zwp_virtual_keyboard_v1_key(kbd, now_ms(), (uint32_t)m, WL_KEYBOARD_KEY_STATE_PRESSED);
		if (m == KEY_LEFTALT) zwp_virtual_keyboard_v1_modifiers(kbd, down && !up ? 8 : 0, 0, 0, 0);
		if (up) zwp_virtual_keyboard_v1_key(kbd, now_ms(), (uint32_t)m, WL_KEYBOARD_KEY_STATE_RELEASED);
		flush_sleep(0);
	} else if (sscanf(cmd, "sleep %d", &m) == 1) {
		flush_sleep(m);
	} else if (*cmd) {
		fprintf(stderr, "zt-deskpoke: what is '%s'?\n", cmd);
	}
}

int main(int argc, char **argv) {
	if (argc != 5) {
		fprintf(stderr, "usage: zt-deskpoke OUTPUT W H 'CMD; CMD...'\n");
		return 2;
	}
	W = atof(argv[2]), H = atof(argv[3]);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "zt-deskpoke: no display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	struct wl_output *out = NULL;
	for (int i = 0; i < n_outs; i++) {
		if (!strcmp(outs[i].name, argv[1])) out = outs[i].o;
	}
	if (!vpm || !seat || !out) {
		fprintf(stderr, "zt-deskpoke: no virtual pointer manager, seat or output '%s'\n", argv[1]);
		return 1;
	}
	ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(vpm, seat, out);
	flush_sleep(50);	/* the shell attaches the new pointer */
	char *script = strdup(argv[4]), *save = NULL;
	for (char *c = strtok_r(script, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
		run(c);
	}
	free(script);
	zwlr_virtual_pointer_v1_destroy(ptr);
	if (kbd) zwp_virtual_keyboard_v1_destroy(kbd);
	wl_display_roundtrip(dpy);
	wl_display_disconnect(dpy);
	return 0;
}
