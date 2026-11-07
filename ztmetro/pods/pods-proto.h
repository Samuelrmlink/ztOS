// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AirPods' two ways of talking, as bytes and nothing else: no socket, no D-Bus, no
 * clock. AAP is the control channel over Classic Bluetooth (L2CAP PSM 0x1001); the
 * proximity pairing advertisement is what the pods broadcast over BLE, connected or
 * not. The facts come from LibrePods, LinuxPods and apple-wireshark (see README.md,
 * which also says which of them nobody has checked on this phone). The code is ours.
 */
#ifndef PODS_PROTO_H
#define PODS_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pods-aes.h"

#define AAP_PSM 0x1001
#define PODS_SERVICE_UUID "74ec2172-0bad-4d01-8f77-997b2be0722a"
#define APPLE_COMPANY_ID 0x004c
#define AAP_MAX_NAME 127	/* the pods cut a longer name, at a byte */

enum {
	AAP_OP_CAPS = 0x02, AAP_OP_BATTERY = 0x04, AAP_OP_EAR = 0x06, AAP_OP_PRIMARY = 0x08,
	AAP_OP_SETTING = 0x09, AAP_OP_DEVICE_IN_USE = 0x0c, AAP_OP_AUDIO_SOURCE = 0x0e, AAP_OP_NOTIFY = 0x0f,
	AAP_OP_RENAME = 0x1a, AAP_OP_INFO = 0x1d, AAP_OP_KEY_REQUEST = 0x30, AAP_OP_KEYS = 0x31,
	AAP_OP_FEATURES = 0x4d,
};

/* what the pods say a setting is, and what sets it: the same packet */
enum { AAP_SET_MODE = 0x0d, AAP_SET_ALLOW_OFF = 0x34 };
enum { AAP_MODE_OFF = 1, AAP_MODE_ANC = 2, AAP_MODE_TRANSPARENCY = 3, AAP_MODE_ADAPTIVE = 4 };
/* on and off, in most settings of the family; not 1 and 0 */
enum { AAP_ON = 1, AAP_OFF = 2 };

/* Nothing but this goes out until it is answered: the pods drop what comes before. */
static const uint8_t __attribute__((unused)) AAP_HANDSHAKE[16] = { 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00 };
static const uint8_t __attribute__((unused)) AAP_NOTIFY_REQ[10] = { 0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff, 0xff, 0xff };
static const uint8_t __attribute__((unused)) AAP_FEATURES_REQ[14] = { 0x04, 0x00, 0x04, 0x00, 0x4d, 0x00, 0xff };
static const uint8_t __attribute__((unused)) AAP_KEY_REQ[8] = { 0x04, 0x00, 0x04, 0x00, 0x30, 0x00, 0x05, 0x00 };

static inline bool aap_is_handshake_answer(const uint8_t *b, size_t n) {
	return n >= 4 && b[0] == 0x01 && b[1] == 0x00 && b[2] == 0x04 && b[3] == 0x00;
}

/* The opcode and what follows it. An opcode byte means nothing without the header. */
static inline bool aap_split(const uint8_t *b, size_t n, unsigned *op, const uint8_t **p, size_t *plen) {
	if (n < 6 || b[0] != 0x04 || b[1] != 0x00 || b[2] != 0x04 || b[3] != 0x00) return false;
	*op = (unsigned)b[4] | (unsigned)b[5] << 8;
	*p = b + 6;
	*plen = n - 6;
	return true;
}

/* ---- from the pods ---- */

enum { POD_LEFT, POD_RIGHT, POD_CASE, POD_HEADSET, POD_PARTS };
struct aap_part { bool present; int level; bool charging; };	/* level -1: the pods have none to give */

/* Entries by their code, never their place: the primary pod comes first. Status 04 beside
 * a level is a placeholder, seen as 00 and as FF: no 0%. Any count, any subset. */
static inline bool aap_parse_battery(const uint8_t *p, size_t n, struct aap_part out[POD_PARTS]) {
	memset(out, 0, sizeof(struct aap_part) * POD_PARTS);
	if (n < 1) return false;
	size_t count = p[0];
	for (size_t i = 0; i < count && 1 + (i + 1) * 5 <= n; i++) {
		const uint8_t *e = p + 1 + i * 5;
		int part;
		switch (e[0]) {
		case 0x01: part = POD_HEADSET; break;	/* one battery: named in LibrePods, never seen by us */
		case 0x02: part = POD_RIGHT; break;
		case 0x04: part = POD_LEFT; break;
		case 0x08: part = POD_CASE; break;
		default: continue;
		}
		out[part].present = true;
		out[part].charging = e[3] == 0x01;
		out[part].level = (e[3] == 0x04 || e[3] == 0x00 || e[2] > 100) ? -1 : e[2];
	}
	return true;
}

enum { EAR_IN = 0, EAR_OUT = 1, EAR_CASE_OPEN = 2, EAR_CASE_CLOSED = 3 };
static inline bool aap_parse_ear(const uint8_t *p, size_t n, int *primary_place, int *secondary_place) {
	if (n < 2 || p[0] > 3 || p[1] > 3) return false;
	*primary_place = p[0];
	*secondary_place = p[1];
	return true;
}
static inline bool aap_parse_primary(const uint8_t *p, size_t n, int *primary) {
	if (n < 1 || (p[0] != 1 && p[0] != 2)) return false;
	*primary = p[0] == 1 ? POD_LEFT : POD_RIGHT;
	return true;
}

static inline bool aap_parse_setting(const uint8_t *p, size_t n, uint8_t *id, uint8_t value[4]) {
	if (n < 2) return false;
	*id = p[0];
	memset(value, 0, 4);
	memcpy(value, p + 1, n - 1 > 4 ? 4 : n - 1);
	return true;
}

static inline bool pods_utf8_ok(const char *s, size_t n) {
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (!len || i + len > n || c < 0x20 || c == 0x7f) return false;
		for (size_t j = 1; j < len; j++) {
			if (((unsigned char)s[i + j] & 0xc0) != 0x80) return false;
		}
		i += len;
	}
	return true;
}

struct aap_info { char name[AAP_MAX_NAME + 1], model[16], firmware[32], hardware[16]; };

/* Field 10, packed: 0x9015C0 reads 9A348. Read off one value by LinuxPods. */
static inline bool aap_firmware_unpack(unsigned long v, char *out, size_t size) {
	if (!v) return false;
	unsigned build = (v >> 4) & 0xfff, low = v & 0xf;
	char suffix[2] = { low ? (char)('a' + low - 1) : 0, 0 };
	snprintf(out, size, "%lu%c%u%s", v >> 20, (char)('A' + ((v >> 16) & 0xf)), build, suffix);
	return true;
}

/* After five bytes of header, NUL-separated: name, model, maker, serial, two versions,
 * hardware, updater name, the pods' serials, packed version. The serials stay here: they
 * identify a person's pods. */
static inline bool aap_parse_info(const uint8_t *p, size_t n, struct aap_info *out) {
	memset(out, 0, sizeof(*out));
	if (n < 6) return false;
	size_t at = 5;
	char active[32] = "";
	for (int field = 0; field <= 10 && at < n; field++) {
		const uint8_t *end = memchr(p + at, 0, n - at);
		size_t len = end ? (size_t)(end - (p + at)) : n - at;
		const char *s = (const char *)p + at;
		if (pods_utf8_ok(s, len)) {
			if (field == 0 && len <= AAP_MAX_NAME) memcpy(out->name, s, len);
			if (field == 1 && len < sizeof(out->model)) memcpy(out->model, s, len);
			if (field == 4 && len < sizeof(active)) memcpy(active, s, len);
			if (field == 6 && len < sizeof(out->hardware)) memcpy(out->hardware, s, len);
			if (field == 10 && len > 0 && len < 16) {
				char num[16] = "";
				unsigned long v = 0;
				memcpy(num, s, len);
				if (sscanf(num, "%lu", &v) == 1) aap_firmware_unpack(v, out->firmware, sizeof(out->firmware));
			}
		}
		at += len + 1;
	}
	if (!out->firmware[0]) memcpy(out->firmware, active, sizeof(out->firmware));
	return out->name[0] != 0;
}

/* The IRK comes least significant byte first, as SMP carries keys; ah() takes it the
 * other way round, so it is kept reversed. As it comes, it resolves nothing. */
static inline bool aap_parse_keys(const uint8_t *p, size_t n, uint8_t irk[16], uint8_t enc[16]) {
	bool have_irk = false, have_enc = false;
	if (n < 1) return false;
	size_t at = 1;
	for (int i = 0; i < p[0] && at + 4 <= n; i++) {
		size_t len = (size_t)p[at + 2] | (size_t)p[at + 3] << 8;
		if (at + 4 + len > n) return false;
		if (p[at] == 0x01 && len == 16) {
			for (int b = 0; b < 16; b++) irk[b] = p[at + 4 + 15 - b];
			have_irk = true;
		} else if (p[at] == 0x04 && len == 16) {
			memcpy(enc, p + at + 4, 16);
			have_enc = true;
		}
		at += 4 + len;
	}
	return have_irk && have_enc;
}

/* ---- to the pods ---- */

static inline size_t aap_build_setting(uint8_t out[11], uint8_t id, const uint8_t value[4]) {
	static const uint8_t head[6] = { 0x04, 0x00, 0x04, 0x00, AAP_OP_SETTING, 0x00 };
	memcpy(out, head, 6);
	out[6] = id;
	memcpy(out + 7, value, 4);
	return 11;
}

/* 0 if the name is empty, too long, broken UTF-8 or holds a control character */
static inline size_t aap_build_rename(uint8_t *out, size_t cap, const char *name) {
	size_t len = strlen(name);
	if (!len || len > AAP_MAX_NAME || cap < 9 + len || !pods_utf8_ok(name, len)) return 0;
	static const uint8_t head[6] = { 0x04, 0x00, 0x04, 0x00, AAP_OP_RENAME, 0x00 };
	memcpy(out, head, 6);
	out[6] = 0x01;
	out[7] = (uint8_t)len;
	out[8] = 0x00;
	memcpy(out + 9, name, len);
	return 9 + len;
}

/* ---- the broadcast ---- */

#define BLE_PROXIMITY_TYPE 0x07
#define BLE_PROXIMITY_LEN 25

/* Apple's manufacturer data is a list of type, length, value. NULL unless it holds a
 * paired-form proximity pairing message. */
static inline const uint8_t *ble_find_proximity(const uint8_t *m, size_t n) {
	for (size_t at = 0; at + 2 <= n;) {
		size_t len = m[at + 1];
		if (at + 2 + len > n) return NULL;
		if (m[at] == BLE_PROXIMITY_TYPE) {
			/* first byte 00 is pairing mode, another layout */
			return len == BLE_PROXIMITY_LEN && m[at + 2] == 0x01 ? m + at + 2 : NULL;
		}
		at += 2 + len;
	}
	return NULL;
}

/* What anyone nearby can read. Levels in 10% steps, -1 unknown. Which pair: not said. */
struct ble_clear {
	unsigned product;
	bool sender_left, sender_in_case, both_in_case;
	int level[3];			/* POD_LEFT, POD_RIGHT, POD_CASE */
	bool charging[3];
	bool in_ear[2];			/* POD_LEFT, POD_RIGHT */
	int lid;			/* 0 open, 1 closed, -1 no pod in the case to say */
	int color, connection;
};

static inline int ble_nibble(unsigned v) { return v == 0xf ? -1 : v >= 10 ? 100 : (int)v * 10; }

/* One value per pod is named relative to the sender: the sender has the low battery
 * nibble and charging bit 4; the same bit is the left pod in one advert and the right
 * in the next. */
static inline bool ble_parse_clear(const uint8_t *p, struct ble_clear *c) {
	memset(c, 0, sizeof(*c));
	if (p[0] != 0x01) return false;
	c->product = (unsigned)p[1] | (unsigned)p[2] << 8;
	unsigned st = p[3];
	c->sender_left = st & 0x20;
	c->sender_in_case = st & 0x40;
	c->both_in_case = st & 0x04;
	int self = c->sender_left ? POD_LEFT : POD_RIGHT, other = c->sender_left ? POD_RIGHT : POD_LEFT;
	c->level[self] = ble_nibble(p[4] & 0xf);
	c->level[other] = ble_nibble(p[4] >> 4);
	c->level[POD_CASE] = ble_nibble(p[5] & 0xf);
	c->charging[self] = p[5] & 0x10;
	c->charging[other] = p[5] & 0x20;
	c->charging[POD_CASE] = p[5] & 0x40;
	/* bits 1 and 3 swap with the sender in the case */
	int bit1 = c->sender_in_case ? other : self, bit3 = c->sender_in_case ? self : other;
	c->in_ear[bit1] = st & 0x02;
	c->in_ear[bit3] = st & 0x08;
	c->lid = (c->sender_in_case || c->both_in_case) ? ((p[6] & 0x08) ? 1 : 0) : -1;
	c->color = p[7];
	c->connection = p[8];
	return true;
}

/* An advert's address against an IRK (most significant byte first): Core spec Vol 3
 * Part H 2.2.2. addr as written, AA:BB:.. is addr[0], addr[1].. */
static inline bool ble_rpa_resolves(const uint8_t irk[16], const uint8_t addr[6]) {
	if ((addr[0] & 0xc0) != 0x40) return false;
	uint8_t in[16] = { 0 }, out[16];
	memcpy(in + 13, addr, 3);
	pods_aes_encrypt(irk, in, out);
	return memcmp(out + 13, addr + 3, 3) == 0;
}

struct ble_exact { int level[3]; bool charging[3]; };

/* Bytes 9-24, one AES block under ENC_KEY. AES always "works", so the plaintext is checked
 * by the pair's MAC: its last three bytes at 7-9, or zeros unless both pods are in the case.
 * Not by "magic bytes", which reject good decryptions. */
static inline bool ble_decrypt(const uint8_t enc[16], const uint8_t *payload, const uint8_t mac[6],
		bool sender_left, struct ble_exact *out) {
	uint8_t pt[16];
	static const uint8_t zero[3];
	pods_aes_decrypt(enc, payload + 9, pt);
	if (memcmp(pt + 7, mac + 3, 3) != 0 && memcmp(pt + 7, zero, 3) != 0) return false;
	int self = sender_left ? POD_LEFT : POD_RIGHT, other = sender_left ? POD_RIGHT : POD_LEFT;
	const int part[3] = { self, other, POD_CASE };
	for (int i = 0; i < 3; i++) {
		uint8_t b = pt[1 + i];
		int level = b & 0x7f;
		out->level[part[i]] = level > 100 ? -1 : level;
		out->charging[part[i]] = level <= 100 && (b & 0x80);
	}
	return true;
}

/* ---- addresses ---- */

static inline bool pods_mac_parse(const char *s, uint8_t out[6]) {
	unsigned v[6];
	if (!s || strlen(s) != 17 || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 5; i++) {
		if (s[i * 3 + 2] != ':') return false;
	}
	for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
	return true;
}

/* BlueZ's /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF, as AA:BB:CC:DD:EE:FF */
static inline bool pods_path_mac(const char *path, char mac[18]) {
	const char *d = path ? strstr(path, "/dev_") : NULL;
	if (!d || strlen(d + 5) < 17 || (d[22] != 0 && d[22] != '/')) return false;
	memcpy(mac, d + 5, 17);
	mac[17] = 0;
	for (int i = 2; i < 17; i += 3) {
		if (mac[i] != '_') return false;
		mac[i] = ':';
	}
	uint8_t bytes[6];
	return pods_mac_parse(mac, bytes);
}

#endif
