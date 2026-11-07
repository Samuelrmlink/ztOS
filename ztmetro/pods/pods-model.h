// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What ztos-podsd knows of each pair of AirPods, and what a packet, an advert or a
 * command does to it: no socket, no D-Bus and no clock (the caller says what time it
 * is), so all of it runs in tests/pods-proto.c with no phone. What has to go out goes
 * into a struct pods_out for the caller to send, in order.
 */
#ifndef PODS_MODEL_H
#define PODS_MODEL_H

#include <stdio.h>
#include <stdlib.h>

#include "pods-proto.h"

#define PODS_MAX 6
#define PODS_NEARBY 4
#define PODS_NEARBY_S 30		/* a nearby pair not heard for this long has gone */
#define PODS_MODE_HOLD_MS 2500		/* a report of the old mode can follow a change */
#define PODS_OFF_DEBOUNCE_MS 300	/* one pod in an ear: "Off" for 50 ms, and back */
#define PODS_DUMP_WAIT_MS 25000		/* the startup dump has taken 20 s: then rows open anyway */
#define PODS_MAX_SETTING_ID 0x42

/* ---- models ---- */

enum {
	CAP_BASIC = 1 << 0, CAP_MODES = 1 << 1, CAP_ADAPTIVE = 1 << 2, CAP_CONVERSATION = 1 << 3,
	CAP_PERSONAL_VOLUME = 1 << 4, CAP_STEM = 1 << 5, CAP_SWIPE = 1 << 6, CAP_SLEEP = 1 << 7,
	CAP_CASE_SOUND = 1 << 8,
};

#define CAPS_PRO2 (CAP_BASIC | CAP_MODES | CAP_ADAPTIVE | CAP_CONVERSATION | CAP_PERSONAL_VOLUME | CAP_STEM | \
	CAP_SWIPE | CAP_SLEEP | CAP_CASE_SOUND)

/* Product ids as advertised, and as BlueZ's Modalias has them. Only the two Pro models
 * are seen on hardware anywhere we have read; the rest is other projects' tables. */
struct pods_model { unsigned product; const char *id, *label; unsigned caps; };
static const struct pods_model __attribute__((unused)) PODS_MODELS[] = {
	{ 0x2002, "airpods1", "AirPods (1st generation)", CAP_BASIC },
	{ 0x200f, "airpods2", "AirPods (2nd generation)", CAP_BASIC },
	{ 0x2013, "airpods3", "AirPods (3rd generation)", CAP_BASIC },
	{ 0x2019, "airpods4", "AirPods 4", CAP_BASIC | CAP_PERSONAL_VOLUME | CAP_SLEEP },
	{ 0x201b, "airpods4-anc", "AirPods 4 with ANC", CAP_BASIC | CAP_MODES | CAP_ADAPTIVE | CAP_CONVERSATION |
		CAP_PERSONAL_VOLUME | CAP_STEM | CAP_SLEEP },
	{ 0x200e, "pro1", "AirPods Pro (1st generation)", CAP_BASIC | CAP_MODES },
	{ 0x2014, "pro2", "AirPods Pro 2 (Lightning)", CAPS_PRO2 },
	{ 0x2024, "pro2-usbc", "AirPods Pro 2 (USB-C)", CAPS_PRO2 },
	{ 0x2027, "pro3", "AirPods Pro 3", CAPS_PRO2 & ~CAP_CASE_SOUND },
	{ 0x200a, "max", "AirPods Max", CAP_BASIC | CAP_MODES },
	{ 0x201f, "max-usbc", "AirPods Max (USB-C)", CAP_BASIC | CAP_MODES },
};

static inline const struct pods_model *pods_model_find(unsigned product) {
	for (size_t i = 0; i < sizeof(PODS_MODELS) / sizeof(PODS_MODELS[0]); i++) {
		if (PODS_MODELS[i].product == product) return &PODS_MODELS[i];
	}
	return NULL;
}

/* "bluetooth:v004Cp2024d0100" or "usb:v05ACp2024d0100": the product, or 0 if not Apple's */
static inline unsigned pods_modalias_product(const char *m) {
	unsigned vendor, product;
	if (!m) return 0;
	if (sscanf(m, "bluetooth:v%4xp%4x", &vendor, &product) == 2 && vendor == 0x004c) return product;
	if (sscanf(m, "usb:v%4xp%4x", &vendor, &product) == 2 && vendor == 0x05ac) return product;
	return 0;
}

/* ---- settings: all of them one packet, sub-command 0x09 ---- */

enum pods_kind { PK_BOOL, PK_PERCENT, PK_ENUM, PK_MASK };
struct pods_setting {
	const char *key;
	uint8_t id, index;	/* the value's byte: two settings can share a packet */
	enum pods_kind kind;
	unsigned caps;
	uint8_t values[3];	/* PK_ENUM: the ones it takes, up to three */
	bool dup;		/* reported twice over: a second byte follows the first */
};

/* The rest of the family is left out on purpose: see README.md. */
static const struct pods_setting __attribute__((unused)) PODS_SETTINGS[] = {
	{ "conversation", 0x28, 0, PK_BOOL, CAP_CONVERSATION, {0}, false },
	{ "adaptive_strength", 0x2e, 0, PK_PERCENT, CAP_ADAPTIVE, {0}, false },
	{ "mode_cycle", 0x1a, 0, PK_MASK, CAP_MODES, {0}, false },
	{ "one_bud_anc", 0x1b, 0, PK_BOOL, CAP_MODES, {0}, false },
	{ "personalized_volume", 0x26, 0, PK_BOOL, CAP_PERSONAL_VOLUME, {0}, false },
	{ "ear_detection", 0x0a, 0, PK_BOOL, CAP_BASIC, {0}, false },
	{ "mic", 0x01, 0, PK_ENUM, CAP_BASIC, {0, 1, 2}, false },
	{ "hold_right", 0x16, 0, PK_ENUM, CAP_STEM, {1, 5, 1}, false },
	{ "hold_left", 0x16, 1, PK_ENUM, CAP_STEM, {1, 5, 1}, false },
	{ "press_speed", 0x17, 0, PK_ENUM, CAP_STEM, {0, 1, 2}, false },
	{ "hold_duration", 0x18, 0, PK_ENUM, CAP_STEM, {0, 1, 2}, false },
	{ "swipe_volume", 0x25, 0, PK_BOOL, CAP_SWIPE, {0}, false },
	{ "swipe_length", 0x23, 0, PK_ENUM, CAP_SWIPE, {0, 1, 2}, false },
	{ "chime_volume", 0x1f, 0, PK_PERCENT, CAP_CASE_SOUND, {0}, true },
	{ "case_tone", 0x31, 0, PK_BOOL, CAP_CASE_SOUND, {0}, false },
	{ "case_tone_volume", 0x40, 0, PK_PERCENT, CAP_CASE_SOUND, {0}, false },
	{ "sleep_detection", 0x35, 0, PK_BOOL, CAP_SLEEP, {0}, false },
	{ "connect_auto", 0x20, 0, PK_BOOL, CAP_BASIC, {0}, false },
};
#define PODS_N_SETTINGS (sizeof(PODS_SETTINGS) / sizeof(PODS_SETTINGS[0]))

/* ---- a pair ---- */

enum pods_link { PODS_LINK_NONE, PODS_LINK_CONNECTED, PODS_LINK_WAIT_ANSWER, PODS_LINK_READY };

struct pods_battery { int level; bool charging; long seen; char source; };	/* level -1: never read */

struct pods_device {
	char mac[18];
	uint8_t mac_b[6];
	bool in_bluez;			/* BlueZ lists it with the AirPods service */
	bool has_keys;
	uint8_t irk[16], enc[16];	/* irk most significant byte first */
	char name[AAP_MAX_NAME + 1], model_number[16], firmware[32];
	unsigned product;
	int color;			/* -1 unknown */

	enum pods_link link;
	uint32_t link_up_ms;
	struct pods_battery part[POD_PARTS];
	int lid;			/* 0 open, 1 closed, -1 unknown */
	long lid_seen;
	int connection;			/* the advert's: -1 unknown */
	int primary;			/* POD_LEFT, POD_RIGHT, -1 */
	int place[2];			/* primary pod's, secondary's: EAR_*, -1 */

	int mode;			/* AAP_MODE_*, 0 unknown */
	uint32_t mode_sent_ms;
	int mode_pending;		/* a report of Off, waiting to see if it lasts */
	uint32_t mode_pending_ms;
	bool settings_known;		/* the dump came, or we stopped waiting */

	bool known[PODS_MAX_SETTING_ID];
	uint8_t raw[PODS_MAX_SETTING_ID][4];
};

struct pods_nearby { unsigned product; int color, level[3], lid; bool charging[3]; long seen; };

struct pods_world {
	struct pods_device dev[PODS_MAX];
	int n;
	struct pods_nearby nearby[PODS_NEARBY];
	int n_nearby;
};

#define PODS_OUT_MAX 4
struct pods_out {
	int n;
	size_t len[PODS_OUT_MAX];
	uint8_t pkt[PODS_OUT_MAX][AAP_MAX_NAME + 16];
};

static inline void pods_out_add(struct pods_out *o, const uint8_t *b, size_t n) {
	if (o->n < PODS_OUT_MAX && n <= sizeof(o->pkt[0])) {
		memcpy(o->pkt[o->n], b, n);
		o->len[o->n++] = n;
	}
}

static inline struct pods_device *pods_find(struct pods_world *w, const char *mac) {
	for (int i = 0; i < w->n; i++) {
		if (!strcmp(w->dev[i].mac, mac)) return &w->dev[i];
	}
	return NULL;
}

static inline struct pods_device *pods_add(struct pods_world *w, const char *mac) {
	struct pods_device *d = pods_find(w, mac);
	uint8_t bytes[6];
	if (d || w->n >= PODS_MAX || !pods_mac_parse(mac, bytes)) return d;
	d = &w->dev[w->n++];
	memset(d, 0, sizeof(*d));
	snprintf(d->mac, sizeof(d->mac), "%s", mac);
	memcpy(d->mac_b, bytes, 6);
	for (int i = 0; i < POD_PARTS; i++) d->part[i].level = -1;
	d->color = d->lid = d->connection = d->primary = d->place[0] = d->place[1] = -1;
	return d;
}

static inline void pods_remove(struct pods_world *w, const char *mac) {
	struct pods_device *d = pods_find(w, mac);
	if (!d) return;
	int i = (int)(d - w->dev);
	memmove(d, d + 1, sizeof(*d) * (size_t)(w->n - i - 1));
	w->n--;
}

static inline const struct pods_setting *pods_setting_find(const char *key) {
	for (size_t i = 0; i < PODS_N_SETTINGS; i++) {
		if (!strcmp(PODS_SETTINGS[i].key, key)) return &PODS_SETTINGS[i];
	}
	return NULL;
}

static inline unsigned pods_caps(const struct pods_device *d) {
	const struct pods_model *m = pods_model_find(d->product);
	return m ? m->caps : 0;
}

/* The model's table says it has it, or the pods themselves reported it. An unknown or newer
 * model still shows what it has; a known one does not lose a row the dump leaves out, since
 * several settings are never in the dump (0x1A, 0x0A, 0x16 on the captures we have read). */
static inline bool pods_setting_visible(const struct pods_device *d, const struct pods_setting *s) {
	return (pods_caps(d) & s->caps) || d->known[s->id];
}

/* The value as the app shows it, -1 unknown */
static inline int pods_setting_value(const struct pods_device *d, const struct pods_setting *s) {
	if (!d->known[s->id]) return -1;
	uint8_t v = d->raw[s->id][s->index];
	if (s->kind == PK_BOOL) return v == AAP_ON ? 1 : v == AAP_OFF ? 0 : -1;
	return v;
}

/* ---- the link: what comes in, what goes out ---- */

/* The channel is up. Nothing else may go out until the handshake is answered. */
static inline void pods_link_open(struct pods_device *d, uint32_t now, struct pods_out *o) {
	d->link = PODS_LINK_WAIT_ANSWER;
	d->link_up_ms = now;
	d->settings_known = false;
	d->mode = d->mode_pending = 0;
	d->primary = d->place[0] = d->place[1] = -1;
	pods_out_add(o, AAP_HANDSHAKE, sizeof(AAP_HANDSHAKE));
}

/* The channel has gone. What was read stays, with when it was read; `connected`: BlueZ
 * still has the pair. */
static inline void pods_link_gone(struct pods_device *d, bool connected) {
	d->link = connected ? PODS_LINK_CONNECTED : PODS_LINK_NONE;
	d->settings_known = false;
	d->mode = d->mode_pending = 0;
	d->primary = d->place[0] = d->place[1] = -1;
}

enum { PODS_EV_CHANGED = 1, PODS_EV_KEYS = 2, PODS_EV_SAVE = 4 };

/* What happened to the mode, which the pods report in ways that need care: right after a
 * change a report of the old one can come, and an Off can be a blip of one pod in an ear. */
static inline bool pods_mode_report(struct pods_device *d, uint32_t now, int mode) {
	if (mode < AAP_MODE_OFF || mode > AAP_MODE_ADAPTIVE) return false;
	if (d->mode_sent_ms && now - d->mode_sent_ms < PODS_MODE_HOLD_MS) {
		if (mode == d->mode) d->mode_sent_ms = 0;
		return false;
	}
	d->mode_sent_ms = 0;
	if (mode == d->mode) {
		d->mode_pending = 0;
		return false;
	}
	if (mode == AAP_MODE_OFF && d->mode) {
		d->mode_pending = mode;
		d->mode_pending_ms = now;
		return false;
	}
	d->mode_pending = 0;
	d->mode = mode;
	return true;
}

/* Time passing on an open link: returns true if something a reader sees changed */
static inline bool pods_tick(struct pods_device *d, uint32_t now) {
	bool changed = false;
	if (d->mode_pending && now - d->mode_pending_ms >= PODS_OFF_DEBOUNCE_MS) {
		d->mode = d->mode_pending;
		d->mode_pending = 0;
		changed = true;
	}
	if (d->link == PODS_LINK_READY && !d->settings_known && now - d->link_up_ms >= PODS_DUMP_WAIT_MS) {
		d->settings_known = true;
		changed = true;
	}
	return changed;
}

/* One packet from the pods. Returns PODS_EV_*. */
static inline int pods_link_packet(struct pods_device *d, uint32_t now, long wall, const uint8_t *b, size_t n,
		struct pods_out *o) {
	if (d->link == PODS_LINK_WAIT_ANSWER) {
		if (!aap_is_handshake_answer(b, n)) return 0;
		d->link = PODS_LINK_READY;
		pods_out_add(o, AAP_NOTIFY_REQ, sizeof(AAP_NOTIFY_REQ));
		pods_out_add(o, AAP_FEATURES_REQ, sizeof(AAP_FEATURES_REQ));
		pods_out_add(o, AAP_KEY_REQ, sizeof(AAP_KEY_REQ));
		return PODS_EV_CHANGED;
	}
	if (d->link != PODS_LINK_READY) return 0;
	unsigned op;
	const uint8_t *p;
	size_t len;
	if (!aap_split(b, n, &op, &p, &len)) return 0;
	switch (op) {
	case AAP_OP_BATTERY: {
		struct aap_part parts[POD_PARTS];
		if (!aap_parse_battery(p, len, parts)) return 0;
		int ev = 0;
		for (int i = 0; i < POD_PARTS; i++) {
			/* a part with nothing to report keeps its last reading, and its age */
			if (!parts[i].present || parts[i].level < 0) continue;
			d->part[i] = (struct pods_battery){ parts[i].level, parts[i].charging, wall, 'a' };
			ev = PODS_EV_CHANGED | PODS_EV_SAVE;
		}
		return ev;
	}
	case AAP_OP_EAR: {
		int a, c;
		if (!aap_parse_ear(p, len, &a, &c)) return 0;
		d->place[0] = a;
		d->place[1] = c;
		return PODS_EV_CHANGED;
	}
	case AAP_OP_PRIMARY: {
		int primary;
		if (!aap_parse_primary(p, len, &primary)) return 0;
		d->primary = primary;
		return PODS_EV_CHANGED;
	}
	case AAP_OP_SETTING: {
		uint8_t id, value[4];
		/* byte 6 before anything: 0x25's 01 is no listening mode */
		if (!aap_parse_setting(p, len, &id, value)) return 0;
		if (id == AAP_SET_MODE) {
			bool first = !d->settings_known;
			d->settings_known = true;	/* the dump's first setting */
			return (pods_mode_report(d, now, value[0]) || first) ? PODS_EV_CHANGED : 0;
		}
		if (id < PODS_MAX_SETTING_ID) {
			if (d->known[id] && !memcmp(d->raw[id], value, 4)) return 0;
			d->known[id] = true;
			memcpy(d->raw[id], value, 4);
			return PODS_EV_CHANGED | PODS_EV_SAVE;
		}
		return 0;
	}
	case AAP_OP_INFO: {
		struct aap_info info;
		if (!aap_parse_info(p, len, &info)) return 0;
		snprintf(d->name, sizeof(d->name), "%s", info.name);
		snprintf(d->model_number, sizeof(d->model_number), "%s", info.model);
		snprintf(d->firmware, sizeof(d->firmware), "%s", info.firmware);
		return PODS_EV_CHANGED | PODS_EV_SAVE;
	}
	case AAP_OP_KEYS: {
		uint8_t irk[16], enc[16];
		if (!aap_parse_keys(p, len, irk, enc)) return 0;
		if (d->has_keys && !memcmp(d->irk, irk, 16) && !memcmp(d->enc, enc, 16)) return 0;
		memcpy(d->irk, irk, 16);
		memcpy(d->enc, enc, 16);
		d->has_keys = true;
		return PODS_EV_CHANGED | PODS_EV_KEYS;
	}
	}
	return 0;
}

/* ---- commands ---- */

static inline const char *pods_mode_name(int mode) {
	switch (mode) {
	case AAP_MODE_OFF: return "off";
	case AAP_MODE_ANC: return "anc";
	case AAP_MODE_TRANSPARENCY: return "transparency";
	case AAP_MODE_ADAPTIVE: return "adaptive";
	}
	return NULL;
}

static inline void pods_send_setting(struct pods_out *o, uint8_t id, uint8_t v) {
	uint8_t pkt[11], value[4] = { v };
	aap_build_setting(pkt, id, value);
	pods_out_add(o, pkt, sizeof(pkt));
}

/* All return NULL when it went, or why not, for the reply. */
static inline const char *pods_cmd_mode(struct pods_device *d, uint32_t now, const char *name, struct pods_out *o) {
	if (d->link != PODS_LINK_READY) return "not-linked";
	unsigned caps = pods_caps(d);
	int mode = 0;
	for (int m = AAP_MODE_OFF; m <= AAP_MODE_ADAPTIVE; m++) {
		if (!strcmp(name, pods_mode_name(m))) mode = m;
	}
	if (!mode) return "bad-value";
	if (caps && !(caps & CAP_MODES)) return "unsupported";
	if (mode == AAP_MODE_ADAPTIVE && caps && !(caps & CAP_ADAPTIVE)) return "unsupported";
	/* a bare Off gets an error chime on Pro 2 USB-C and Pro 3: the gate goes right before it */
	if (mode == AAP_MODE_OFF) pods_send_setting(o, AAP_SET_ALLOW_OFF, AAP_ON);
	pods_send_setting(o, AAP_SET_MODE, (uint8_t)mode);
	/* recorded as sent: nothing reliably answers a change */
	d->mode = mode;
	d->mode_sent_ms = now ? now : 1;
	d->mode_pending = 0;
	return NULL;
}

static inline const char *pods_cmd_set(struct pods_device *d, const char *key, const char *value, struct pods_out *o) {
	if (d->link != PODS_LINK_READY) return "not-linked";
	const struct pods_setting *s = pods_setting_find(key);
	if (!s) return "unknown-setting";
	if (!pods_setting_visible(d, s)) return "unsupported";
	char *end;
	long v = strtol(value, &end, 10);
	if (end == value || *end) return "bad-value";
	uint8_t byte;
	switch (s->kind) {
	case PK_BOOL:
		if (v != 0 && v != 1) return "bad-value";
		byte = v ? AAP_ON : AAP_OFF;
		break;
	case PK_PERCENT:
		if (v < 0 || v > 100) return "bad-value";
		byte = (uint8_t)v;
		break;
	case PK_ENUM:
		if (v < 0 || v > 255 || (v != s->values[0] && v != s->values[1] && v != s->values[2])) return "bad-value";
		byte = (uint8_t)v;
		break;
	case PK_MASK: {
		/* the press-and-hold cycle: at least two modes to go round */
		int bits = 0;
		for (int i = 0; i < 4; i++) bits += (int)(v >> i) & 1;
		if (v < 0 || v > 15 || bits < 2) return "bad-value";
		byte = (uint8_t)v;
		if (v & 1) pods_send_setting(o, AAP_SET_ALLOW_OFF, AAP_ON);
		break;
	}
	default:
		return "bad-value";
	}
	uint8_t value4[4], pkt[11];
	memcpy(value4, d->raw[s->id], 4);
	if (!d->known[s->id]) {
		memset(value4, 0, 4);
		/* the other pod's half of a shared packet, not known: as this one */
		if (s->id == 0x16) value4[0] = value4[1] = byte;
	}
	value4[s->index] = byte;
	if (s->dup && d->known[s->id] && d->raw[s->id][1]) value4[1] = byte;
	aap_build_setting(pkt, s->id, value4);
	pods_out_add(o, pkt, sizeof(pkt));
	/* stored: most settings do not report a change, and some are never in the dump */
	d->known[s->id] = true;
	memcpy(d->raw[s->id], value4, 4);
	return NULL;
}

static inline const char *pods_cmd_rename(struct pods_device *d, const char *name, struct pods_out *o) {
	if (d->link != PODS_LINK_READY) return "not-linked";
	uint8_t pkt[AAP_MAX_NAME + 16];
	size_t n = aap_build_rename(pkt, sizeof(pkt), name);
	if (!n) return "bad-name";
	pods_out_add(o, pkt, n);
	/* nothing answers it; the next link's 0x1D says it took */
	snprintf(d->name, sizeof(d->name), "%s", name);
	return NULL;
}

/* ---- the broadcast ---- */

static inline void pods_nearby_note(struct pods_world *w, const struct ble_clear *c, long wall) {
	/* one row per kind of pair: a pair's two pods are not told apart, nor can they be */
	int slot = -1, oldest = 0;
	for (int i = 0; i < w->n_nearby; i++) {
		if (w->nearby[i].product == c->product && w->nearby[i].color == c->color) slot = i;
		if (w->nearby[i].seen < w->nearby[oldest].seen) oldest = i;
	}
	if (slot < 0) slot = w->n_nearby < PODS_NEARBY ? w->n_nearby++ : oldest;
	struct pods_nearby *b = &w->nearby[slot];
	b->product = c->product;
	b->color = c->color;
	b->lid = c->lid;
	b->seen = wall;
	for (int i = 0; i < 3; i++) {
		b->level[i] = c->level[i];
		b->charging[i] = c->charging[i];
	}
}

enum { PODS_ADV_IGNORED, PODS_ADV_NEARBY, PODS_ADV_READING, PODS_ADV_MISMATCH };

/* An advert: `addr` it came from, `mfr` Apple's manufacturer data. Only a pair whose IRK
 * resolves the address and whose ENC_KEY opens it counts as that pair: cleartext can be
 * forged, and says nothing of which pair it is. *which is set for PODS_ADV_READING. */
static inline int pods_advert(struct pods_world *w, const uint8_t addr[6], const uint8_t *mfr, size_t n, long wall,
		int *which) {
	const uint8_t *payload = ble_find_proximity(mfr, n);
	struct ble_clear c;
	if (!payload || !ble_parse_clear(payload, &c)) return PODS_ADV_IGNORED;
	for (int i = 0; i < w->n; i++) {
		struct pods_device *d = &w->dev[i];
		if (!d->has_keys || !ble_rpa_resolves(d->irk, addr)) continue;
		struct ble_exact e;
		if (!ble_decrypt(d->enc, payload, d->mac_b, c.sender_left, &e)) return PODS_ADV_MISMATCH;
		if (which) *which = i;
		d->product = c.product;
		d->color = c.color;
		d->connection = c.connection;
		if (c.lid >= 0) {
			d->lid = c.lid;
			d->lid_seen = wall;
		}
		/* with an open link AAP is the truth, and it is live; this lags it */
		if (d->link == PODS_LINK_READY) return PODS_ADV_READING;
		for (int part = 0; part < 3; part++) {
			if (e.level[part] < 0) continue;	/* above 100: not available, keep the last */
			d->part[part] = (struct pods_battery){ e.level[part], e.charging[part], wall, 'b' };
		}
		return PODS_ADV_READING;
	}
	pods_nearby_note(w, &c, wall);
	return PODS_ADV_NEARBY;
}

/* ---- when to look for broadcasts ---- */

struct pods_scan_in {
	bool powered, screen_on, ui_open, audio_active;
	int keyed_unlinked;	/* pairs with keys and no open link: a broadcast would tell us something */
	int known, linked;
};
struct pods_scan_plan { int on_s, every_s; };	/* 0: no scanning */

/* Discovery holds up reconnecting LE devices and costs radio time beside A2DP, so bursts,
 * and only when a broadcast has something to say to someone. */
static inline struct pods_scan_plan pods_scan_policy(const struct pods_scan_in *s) {
	struct pods_scan_plan none = { 0, 0 }, ui = { 4, 10 }, background = { 3, 60 };
	if (!s->powered || !s->screen_on) return none;
	if (s->known > 0 && s->linked == s->known) return none;	/* AAP says it all */
	if (s->ui_open) return ui;
	if (s->keyed_unlinked > 0 && !s->audio_active) return background;
	return none;
}

/* ---- what readers see ---- */

static inline void pods_json_string(FILE *f, const char *s) {
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
		else if (c < 0x20) fprintf(f, "\\u%04x", c);
		else fputc(c, f);
	}
	fputc('"', f);
}

static inline void pods_json_part(FILE *f, const char *name, const struct pods_battery *b, bool *comma) {
	fprintf(f, "%s\"%s\":{", *comma ? "," : "", name);
	*comma = true;
	if (b->level >= 0) {
		fprintf(f, "\"level\":%d,\"charging\":%s,\"seen\":%ld,\"source\":\"%s\"}", b->level,
			b->charging ? "true" : "false", b->seen, b->source == 'a' ? "aap" : "ble");
	} else {
		fputs("\"level\":null}", f);
	}
}

static inline const char *pods_place_name(int place) {
	switch (place) {
	case EAR_IN: return "in_ear";
	case EAR_OUT: return "out";
	case EAR_CASE_OPEN: return "case";
	case EAR_CASE_CLOSED: return "case_closed";
	}
	return "unknown";
}

static inline void pods_json_device(FILE *f, const struct pods_device *d, const char *bluez_name) {
	const struct pods_model *m = pods_model_find(d->product);
	unsigned caps = m ? m->caps : 0;
	fprintf(f, "{\"mac\":\"%s\",\"name\":", d->mac);
	pods_json_string(f, d->name[0] ? d->name : bluez_name && bluez_name[0] ? bluez_name : "AirPods");
	fprintf(f, ",\"model\":\"%s\",\"model_label\":\"%s\",\"product\":%u,\"tested\":%s,\"model_number\":",
		m ? m->id : "unknown", m ? m->label : "AirPods", d->product,
		d->product == 0x2024 || d->product == 0x2014 ? "true" : "false");
	pods_json_string(f, d->model_number);
	fputs(",\"firmware\":", f);
	pods_json_string(f, d->firmware);
	fprintf(f, ",\"color\":%d,\"link\":\"%s\",\"ready\":%s,\"has_keys\":%s,\"in_bluez\":%s,\"battery\":{", d->color,
		d->link == PODS_LINK_NONE ? "none" : d->link == PODS_LINK_READY ? "aap" : "connected",
		d->link == PODS_LINK_READY ? "true" : "false", d->has_keys ? "true" : "false",
		d->in_bluez ? "true" : "false");
	bool comma = false;
	pods_json_part(f, "left", &d->part[POD_LEFT], &comma);
	pods_json_part(f, "right", &d->part[POD_RIGHT], &comma);
	pods_json_part(f, "case", &d->part[POD_CASE], &comma);
	if (d->part[POD_HEADSET].level >= 0) pods_json_part(f, "headset", &d->part[POD_HEADSET], &comma);
	fprintf(f, "},\"lid\":\"%s\",\"lid_seen\":%ld,\"connection\":%d,", d->lid == 0 ? "open" : d->lid == 1 ? "closed" : "unknown",
		d->lid_seen, d->connection);
	/* the ear report gives the primary pod first: without the primary it names nobody */
	const char *left = "unknown", *right = "unknown";
	if (d->primary >= 0) {
		left = pods_place_name(d->place[d->primary == POD_LEFT ? 0 : 1]);
		right = pods_place_name(d->place[d->primary == POD_LEFT ? 1 : 0]);
	}
	fprintf(f, "\"primary\":\"%s\",\"ear\":{\"left\":\"%s\",\"right\":\"%s\"},\"caps\":[",
		d->primary == POD_LEFT ? "left" : d->primary == POD_RIGHT ? "right" : "unknown", left, right);
	static const struct { unsigned cap; const char *name; } CAPS[] = {
		{ CAP_MODES, "modes" }, { CAP_ADAPTIVE, "adaptive" }, { CAP_CONVERSATION, "conversation" },
		{ CAP_PERSONAL_VOLUME, "personalized_volume" }, { CAP_STEM, "stem" }, { CAP_SWIPE, "swipe" },
		{ CAP_SLEEP, "sleep" }, { CAP_CASE_SOUND, "case_sound" },
	};
	comma = false;
	for (size_t i = 0; i < sizeof(CAPS) / sizeof(CAPS[0]); i++) {
		/* a pair that reports a mode has modes, whatever the table says of its model */
		bool has = (caps & CAPS[i].cap) || (CAPS[i].cap == CAP_MODES && d->mode);
		if (has) {
			fprintf(f, "%s\"%s\"", comma ? "," : "", CAPS[i].name);
			comma = true;
		}
	}
	fprintf(f, "],\"mode\":%s%s%s,\"settings_known\":%s,\"visible\":[", d->mode ? "\"" : "",
		d->mode ? pods_mode_name(d->mode) : "null", d->mode ? "\"" : "", d->settings_known ? "true" : "false");
	comma = false;
	for (size_t i = 0; i < PODS_N_SETTINGS; i++) {
		if (!pods_setting_visible(d, &PODS_SETTINGS[i])) continue;
		fprintf(f, "%s\"%s\"", comma ? "," : "", PODS_SETTINGS[i].key);
		comma = true;
	}
	fputs("],\"settings\":{", f);
	comma = false;
	for (size_t i = 0; i < PODS_N_SETTINGS; i++) {
		int v = pods_setting_value(d, &PODS_SETTINGS[i]);
		if (v < 0) continue;
		fprintf(f, "%s\"%s\":%d", comma ? "," : "", PODS_SETTINGS[i].key, v);
		comma = true;
	}
	fputs("}}", f);
}

/* No keys and no serials in here: anything of the user's can read this file. */
static inline void pods_json(FILE *f, const struct pods_world *w, bool powered, bool scanning, long wall,
		const char *(*bluez_name)(const char *mac)) {
	fprintf(f, "{\"version\":1,\"bluetooth\":%s,\"scanning\":%s,\"devices\":[", powered ? "true" : "false",
		scanning ? "true" : "false");
	for (int i = 0; i < w->n; i++) {
		if (i) fputc(',', f);
		pods_json_device(f, &w->dev[i], bluez_name ? bluez_name(w->dev[i].mac) : NULL);
	}
	fputs("],\"nearby\":[", f);
	bool comma = false;
	for (int i = 0; i < w->n_nearby; i++) {
		const struct pods_nearby *b = &w->nearby[i];
		if (wall - b->seen > PODS_NEARBY_S) continue;
		const struct pods_model *m = pods_model_find(b->product);
		fprintf(f, "%s{\"model_label\":\"%s\",\"seen\":%ld,\"lid\":\"%s\",\"battery\":{", comma ? "," : "",
			m ? m->label : "AirPods", b->seen, b->lid == 0 ? "open" : b->lid == 1 ? "closed" : "unknown");
		comma = true;
		bool c2 = false;
		static const char *const NAMES[3] = { "left", "right", "case" };
		for (int part = 0; part < 3; part++) {
			struct pods_battery t = { b->level[part], b->charging[part], b->seen, 'b' };
			pods_json_part(f, NAMES[part], &t, &c2);
		}
		fputs("}}", f);
	}
	fputs("]}\n", f);
}

/* $XDG_RUNTIME_DIR/ztos-pods: what the status bar and Settings' Bluetooth page read, in the
 * style of ztos-bt, for programs that have no JSON parser. The open link, if any; every pair
 * that has connected here, for Settings' button. See pods-bar-state.h. */
static inline void pods_bar_write(FILE *f, const struct pods_world *w) {
	const struct pods_device *linked = NULL;
	for (int i = 0; i < w->n; i++) {
		if (w->dev[i].link == PODS_LINK_READY && !linked) linked = &w->dev[i];
		if (w->dev[i].link == PODS_LINK_READY || w->dev[i].has_keys) fprintf(f, "known=%s\n", w->dev[i].mac);
	}
	fprintf(f, "link=%d\n", linked != NULL);
	if (!linked) return;
	const struct pods_battery *p = linked->part;
	if (p[POD_LEFT].level >= 0) fprintf(f, "left=%d\n", p[POD_LEFT].level);
	if (p[POD_RIGHT].level >= 0) fprintf(f, "right=%d\n", p[POD_RIGHT].level);
	if (p[POD_CASE].level >= 0) fprintf(f, "case=%d\n", p[POD_CASE].level);
	if (p[POD_HEADSET].level >= 0) fprintf(f, "left=%d\n", p[POD_HEADSET].level);
	fprintf(f, "charge_left=%d\ncharge_right=%d\n", p[POD_LEFT].charging, p[POD_RIGHT].charging);
	/* the ear report gives the primary pod first: without the primary it names nobody.
	 * In use means out of the case, in an ear or a hand: one out with the other in the
	 * case is on its own. Both in the case shows both, as there's no better answer. */
	if (linked->primary >= 0) {
		int place_left = linked->place[linked->primary == POD_LEFT ? 0 : 1];
		int place_right = linked->place[linked->primary == POD_LEFT ? 1 : 0];
		bool case_left = place_left == EAR_CASE_OPEN || place_left == EAR_CASE_CLOSED;
		bool case_right = place_right == EAR_CASE_OPEN || place_right == EAR_CASE_CLOSED;
		if (place_left >= 0 && place_right >= 0) {
			if (!case_left && case_right) fputs("single=left\n", f);
			else if (case_left && !case_right) fputs("single=right\n", f);
		}
	}
	fprintf(f, "charging=%d\n", p[POD_LEFT].charging || p[POD_RIGHT].charging || p[POD_HEADSET].charging);
}

/* ---- what is kept across restarts ---- */

static inline void pods_hex(char *out, const uint8_t *b, size_t n) {
	for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

static inline bool pods_unhex(const char *s, uint8_t *out, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		if (!s[i * 2] || !s[i * 2 + 1] || sscanf(s + i * 2, "%2x", &v) != 1) return false;
		out[i] = (uint8_t)v;
	}
	return s[n * 2] == 0;
}

/* The keys: one line a pair, MAC IRK ENC_KEY. In a file of mode 0600; never logged. */
static inline void pods_keys_write(FILE *f, const struct pods_world *w) {
	for (int i = 0; i < w->n; i++) {
		if (!w->dev[i].has_keys) continue;
		char a[33], b[33];
		pods_hex(a, w->dev[i].irk, 16);
		pods_hex(b, w->dev[i].enc, 16);
		fprintf(f, "%s %s %s\n", w->dev[i].mac, a, b);
	}
}

static inline bool pods_keys_line(struct pods_world *w, const char *line) {
	char mac[18], a[33], b[33];
	uint8_t irk[16], enc[16];
	if (sscanf(line, "%17s %32s %32s", mac, a, b) != 3 || !pods_unhex(a, irk, 16) || !pods_unhex(b, enc, 16)) return false;
	struct pods_device *d = pods_add(w, mac);
	if (!d) return false;
	memcpy(d->irk, irk, 16);
	memcpy(d->enc, enc, 16);
	d->has_keys = true;
	return true;
}

/* The rest: "MAC key value", the last reading of each part and what we last set. Not keys. */
static inline void pods_readings_write(FILE *f, const struct pods_world *w) {
	static const char *const PARTS[POD_PARTS] = { "left", "right", "case", "headset" };
	for (int i = 0; i < w->n; i++) {
		const struct pods_device *d = &w->dev[i];
		if (d->name[0]) fprintf(f, "%s name %s\n", d->mac, d->name);
		if (d->model_number[0]) fprintf(f, "%s model_number %s\n", d->mac, d->model_number);
		if (d->firmware[0]) fprintf(f, "%s firmware %s\n", d->mac, d->firmware);
		if (d->product) fprintf(f, "%s product %u\n", d->mac, d->product);
		if (d->color >= 0) fprintf(f, "%s color %d\n", d->mac, d->color);
		if (d->lid >= 0) fprintf(f, "%s lid %d %ld\n", d->mac, d->lid, d->lid_seen);
		for (int p = 0; p < POD_PARTS; p++) {
			const struct pods_battery *b = &d->part[p];
			if (b->level >= 0) fprintf(f, "%s part %s %d %d %ld %c\n", d->mac, PARTS[p], b->level, b->charging, b->seen, b->source);
		}
		for (int id = 0; id < PODS_MAX_SETTING_ID; id++) {
			if (!d->known[id]) continue;
			char hex[9];
			pods_hex(hex, d->raw[id], 4);
			fprintf(f, "%s setting %d %s\n", d->mac, id, hex);
		}
	}
}

static inline void pods_readings_line(struct pods_world *w, const char *line) {
	static const char *const PARTS[POD_PARTS] = { "left", "right", "case", "headset" };
	char mac[18], key[16], part[12], hex[9], source;
	int off = 0, a, b;
	long seen;
	if (sscanf(line, "%17s %15s %n", mac, key, &off) != 2 || !off) return;
	struct pods_device *d = pods_add(w, mac);
	if (!d) return;
	const char *rest = line + off;
	if (!strcmp(key, "name")) snprintf(d->name, sizeof(d->name), "%s", rest);
	else if (!strcmp(key, "model_number")) snprintf(d->model_number, sizeof(d->model_number), "%s", rest);
	else if (!strcmp(key, "firmware")) snprintf(d->firmware, sizeof(d->firmware), "%s", rest);
	else if (!strcmp(key, "product")) d->product = (unsigned)strtoul(rest, NULL, 10);
	else if (!strcmp(key, "color")) d->color = atoi(rest);
	else if (!strcmp(key, "lid") && sscanf(rest, "%d %ld", &a, &seen) == 2) { d->lid = a; d->lid_seen = seen; }
	else if (!strcmp(key, "part") && sscanf(rest, "%11s %d %d %ld %c", part, &a, &b, &seen, &source) == 5) {
		for (int p = 0; p < POD_PARTS; p++) {
			if (!strcmp(part, PARTS[p]) && a >= 0 && a <= 100) d->part[p] = (struct pods_battery){ a, b != 0, seen, source };
		}
	} else if (!strcmp(key, "setting") && sscanf(rest, "%d %8s", &a, hex) == 2 && a >= 0 && a < PODS_MAX_SETTING_ID) {
		if (pods_unhex(hex, d->raw[a], 4)) d->known[a] = true;
	}
}

#endif
