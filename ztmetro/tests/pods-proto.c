// SPDX-License-Identifier: GPL-3.0-or-later
/* The AirPods protocol layer against bytes: no phone, no pods, no libraries. Packets are
 * constructed from the layouts in pods/README.md, or are the published examples it names;
 * none of them has been seen on this phone. Keys here are made up. */
#define _DEFAULT_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "../pods/pods-model.h"
#include "../pods/pods-bar-state.h"

#define B(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })

static const uint8_t IRK_WIRE[16] = { 0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec };
static const uint8_t IRK[16] = { 0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05, 0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b };
static const uint8_t ENC[16] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f };
static const uint8_t RPA[6] = { 0x70, 0x81, 0x94, 0x0d, 0xfb, 0xaa };	/* Core spec's sample: resolves with IRK */
#define MAC "AA:BB:CC:DD:EE:FF"
#define PKT(...) B(0x04, 0x00, 0x04, 0x00, __VA_ARGS__)

static void test_aes(void) {
	/* FIPS-197 Appendix C.1 */
	uint8_t key[16], pt[16], out[16], back[16];
	for (int i = 0; i < 16; i++) { key[i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
	static const uint8_t ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
	pods_aes_encrypt(key, pt, out);
	assert(!memcmp(out, ct, 16));
	pods_aes_decrypt(key, ct, back);
	assert(!memcmp(back, pt, 16));
}

static void test_rpa(void) {
	assert(ble_rpa_resolves(IRK, RPA));
	assert(!ble_rpa_resolves(IRK_WIRE, RPA));	/* as the packet has it: nothing resolves */
	uint8_t other[6];
	memcpy(other, RPA, 6);
	other[5] ^= 1;
	assert(!ble_rpa_resolves(IRK, other));
	other[5] ^= 1;
	other[0] = 0xc0 | (other[0] & 0x3f);		/* a static address is no RPA */
	assert(!ble_rpa_resolves(IRK, other));
}

static void test_battery(void) {
	struct aap_part p[POD_PARTS];
	unsigned op;
	const uint8_t *pl;
	size_t n;

	/* LibrePods' example: right 100 discharging, left 99 charging, case 17 discharging */
	const uint8_t a[] = { 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03, 0x02, 0x01, 0x64, 0x02, 0x01, 0x04, 0x01, 0x63, 0x01, 0x01, 0x08, 0x01, 0x11, 0x02, 0x01 };
	assert(aap_split(a, sizeof(a), &op, &pl, &n) && op == AAP_OP_BATTERY);
	assert(aap_parse_battery(pl, n, p));
	assert(p[POD_RIGHT].level == 100 && !p[POD_RIGHT].charging);
	assert(p[POD_LEFT].level == 99 && p[POD_LEFT].charging);
	assert(p[POD_CASE].level == 17 && !p[POD_CASE].charging);

	/* the case away, then a pod in the closed case: placeholders 00 and FF are no levels */
	const uint8_t b[] = { 0x03, 0x04, 0x01, 0x4e, 0x02, 0x01, 0x02, 0x01, 0x00, 0x04, 0x01, 0x08, 0x01, 0xff, 0x04, 0x01 };
	assert(aap_parse_battery(b, sizeof(b), p));
	assert(p[POD_LEFT].level == 78 && p[POD_RIGHT].present && p[POD_RIGHT].level == -1 && p[POD_CASE].level == -1);

	/* left first or right first: read by code, not by place */
	const uint8_t c[] = { 0x02, 0x02, 0x01, 0x10, 0x02, 0x01, 0x04, 0x01, 0x20, 0x02, 0x01 };
	assert(aap_parse_battery(c, sizeof(c), p));
	assert(p[POD_RIGHT].level == 16 && p[POD_LEFT].level == 32 && !p[POD_CASE].present);

	/* a headset's one part, any count, a count that lies, an unknown component */
	const uint8_t d[] = { 0x09, 0x01, 0x01, 0x32, 0x02, 0x01, 0x40, 0x01, 0x10, 0x02, 0x01, 0x08, 0x01 };
	assert(aap_parse_battery(d, sizeof(d), p));
	assert(p[POD_HEADSET].level == 50 && !p[POD_CASE].present);
	assert(!aap_parse_battery(d, 0, p));
}

static void test_split(void) {
	unsigned op;
	const uint8_t *pl;
	size_t n;
	/* byte 4 means nothing without the header */
	assert(!aap_split(B(0x01, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00), &op, &pl, &n));
	assert(!aap_split(B(0x04, 0x00, 0x04), &op, &pl, &n));
	assert(aap_is_handshake_answer(B(0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00)));
	assert(!aap_is_handshake_answer(B(0x04, 0x00, 0x04, 0x00)));
}

static void test_builders(void) {
	uint8_t out[160];
	assert(sizeof(AAP_HANDSHAKE) == 16 && AAP_HANDSHAKE[4] == 0x01 && AAP_HANDSHAKE[6] == 0x02);
	assert(sizeof(AAP_FEATURES_REQ) == 14 && AAP_FEATURES_REQ[6] == 0xff && AAP_FEATURES_REQ[13] == 0);
	size_t n = aap_build_rename(out, sizeof(out), "Pods");
	static const uint8_t want[] = { 0x04, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x01, 0x04, 0x00, 'P', 'o', 'd', 's' };
	assert(n == sizeof(want) && !memcmp(out, want, n));
	char name[140];
	memset(name, 'x', 127);
	name[127] = 0;
	assert(aap_build_rename(out, sizeof(out), name) == 9 + 127);
	name[127] = 'x';
	name[128] = 0;
	assert(aap_build_rename(out, sizeof(out), name) == 0);		/* the pods would cut it at a byte */
	assert(aap_build_rename(out, sizeof(out), "") == 0);
	assert(aap_build_rename(out, sizeof(out), "a\nb") == 0);
	assert(aap_build_rename(out, sizeof(out), "\xc3") == 0);	/* half a character */
	assert(aap_build_rename(out, sizeof(out), "Caf\xc3\xa9 \xf0\x9f\x8e\xa7") == 9 + 10);
}

static void test_info(void) {
	/* 02 LL LL 04 00, then the fields; serials made up */
	uint8_t p[256] = { 0x02, 0x00, 0x00, 0x04, 0x00 };
	size_t n = 5;
	const char *fields[] = { "Sam's Pods", "A3048", "Apple Inc.", "SERIAL0000", "90.3431", "90.3431", "1.0.0",
		"com.apple.accessory.updater.app.71", "SERIALL", "SERIALR", "9442752" };
	for (size_t i = 0; i < 11; i++) {
		strcpy((char *)p + n, fields[i]);
		n += strlen(fields[i]) + 1;
	}
	struct aap_info info;
	assert(aap_parse_info(p, n, &info));
	assert(!strcmp(info.name, "Sam's Pods") && !strcmp(info.model, "A3048"));
	assert(!strcmp(info.firmware, "9A348"));	/* 0x9015C0, LinuxPods' reading */
	assert(!strstr((char *)&info, "SERIAL"));
	char fw[16];
	assert(aap_firmware_unpack(8454371, fw, sizeof(fw)) && !strcmp(fw, "8B14c"));
	/* truncated, and no name: not taken */
	assert(!aap_parse_info(p, 3, &info));
}

static size_t key_packet(uint8_t *out) {
	static const uint8_t head[] = { 0x04, 0x00, 0x04, 0x00, 0x31, 0x00, 0x02, 0x01, 0x00, 0x10, 0x00 };
	size_t n = 0;
	memcpy(out, head, sizeof(head));
	n = sizeof(head);
	memcpy(out + n, IRK_WIRE, 16);
	n += 16;
	static const uint8_t next[] = { 0x04, 0x00, 0x10, 0x00 };
	memcpy(out + n, next, 4);
	n += 4;
	memcpy(out + n, ENC, 16);
	return n + 16;
}

static void test_keys(void) {
	uint8_t pkt[64], irk[16], enc[16];
	size_t n = key_packet(pkt);
	assert(n == 47);
	assert(aap_parse_keys(pkt + 6, n - 6, irk, enc));
	assert(!memcmp(irk, IRK, 16) && !memcmp(enc, ENC, 16));
	assert(!aap_parse_keys(pkt + 6, 20, irk, enc));		/* cut short */
}

/* An advert as a pod would send it: exact levels encrypted under ENC. */
static size_t advert(uint8_t *m, uint8_t status, uint8_t nib, uint8_t bits, uint8_t lid, int self, int other, int kase,
		const uint8_t suffix[3]) {
	uint8_t pt[16] = { 0x04, (uint8_t)self, (uint8_t)other, (uint8_t)kase, 0xa1, 0xa2, 0xa3 };
	memcpy(pt + 7, suffix, 3);
	m[0] = 0x07;
	m[1] = 25;
	uint8_t *p = m + 2;
	p[0] = 0x01;
	p[1] = 0x24;
	p[2] = 0x20;
	p[3] = status;
	p[4] = nib;
	p[5] = bits;
	p[6] = lid;
	p[7] = 0;
	p[8] = 0x04;
	pods_aes_encrypt(ENC, pt, p + 9);
	return 27;
}

static const uint8_t NO_SUFFIX[3] = { 0, 0, 0 }, SUFFIX[3] = { 0xdd, 0xee, 0xff };

static void test_clear(void) {
	uint8_t m[40];
	struct ble_clear c;
	/* left pod sends, both worn: low nibble its own */
	advert(m, 0x2b, 0x78, 0x07, 0x11, 0, 0, 0, NO_SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c));
	assert(c.product == 0x2024 && c.sender_left && !c.sender_in_case);
	assert(c.level[POD_LEFT] == 80 && c.level[POD_RIGHT] == 70 && c.level[POD_CASE] == 70);
	assert(c.in_ear[POD_LEFT] && c.in_ear[POD_RIGHT] && c.lid == -1);
	/* the same bytes from the right pod name the pods the other way */
	advert(m, 0x0b, 0x78, 0x1f, 0x11, 0, 0, 0, NO_SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c));
	assert(!c.sender_left && c.level[POD_RIGHT] == 80 && c.level[POD_LEFT] == 70);
	assert(c.level[POD_CASE] == -1 && c.charging[POD_RIGHT] && !c.charging[POD_LEFT]);
	/* nibble A is 100; the lid only counts with a pod in the case */
	advert(m, 0x64, 0xaa, 0x45, 0x39, 0, 0, 0, SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c));
	assert(c.level[POD_LEFT] == 100 && c.level[POD_RIGHT] == 100 && c.lid == 1 && c.charging[POD_CASE]);
	advert(m, 0x64, 0xaa, 0x45, 0x31, 0, 0, 0, SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c) && c.lid == 0);
	advert(m, 0x01, 0xaa, 0x45, 0x39, 0, 0, 0, NO_SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c) && c.lid == -1);
	/* in-ear bits swap with the sender in the case */
	advert(m, 0x62, 0xaa, 0x45, 0x31, 0, 0, 0, NO_SUFFIX);
	assert(ble_parse_clear(ble_find_proximity(m, 27), &c));
	assert(c.in_ear[POD_RIGHT] && !c.in_ear[POD_LEFT]);

	/* another message first, then ours; other kinds and forms ignored */
	uint8_t list[60] = { 0x10, 0x02, 0x01, 0x02 };
	advert(list + 4, 0x2b, 0x78, 0x07, 0x11, 0, 0, 0, NO_SUFFIX);
	assert(ble_find_proximity(list, 31) == list + 6);
	assert(!ble_find_proximity(B(0x10, 0x02, 0x01, 0x02)));
	assert(!ble_find_proximity(B(0x07, 0x19, 0x01)));	/* cut short */
	advert(m, 0x2b, 0x78, 0x07, 0x11, 0, 0, 0, NO_SUFFIX);
	m[2] = 0;						/* pairing mode: another layout */
	assert(!ble_find_proximity(m, 27));
}

static struct pods_device *keyed(struct pods_world *w) {
	memset(w, 0, sizeof(*w));
	struct pods_device *d = pods_add(w, MAC);
	memcpy(d->irk, IRK, 16);
	memcpy(d->enc, ENC, 16);
	d->has_keys = true;
	return d;
}

static void test_advert(void) {
	static struct pods_world w;
	struct pods_device *d = keyed(&w);
	uint8_t m[40];
	int which = -1;

	/* the owner's requirement: exact levels, with no connection. Out of the case: zeros. */
	advert(m, 0x21, 0x78, 0x4f, 0x11, 0x80 | 87, 76, 0x80 | 55, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 1000, &which) == PODS_ADV_READING && which == 0);
	assert(d->part[POD_LEFT].level == 87 && d->part[POD_LEFT].charging && d->part[POD_LEFT].source == 'b');
	assert(d->part[POD_RIGHT].level == 76 && !d->part[POD_RIGHT].charging);
	assert(d->part[POD_CASE].level == 55 && d->part[POD_CASE].charging && d->part[POD_CASE].seen == 1000);
	assert(d->product == 0x2024 && w.n_nearby == 0);

	/* from the right pod, the same bytes the other way; the MAC's suffix is good too */
	advert(m, 0x04, 0x78, 0x0f, 0x31, 11, 22, 33, SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 1001, &which) == PODS_ADV_READING);
	assert(d->part[POD_RIGHT].level == 11 && d->part[POD_LEFT].level == 22 && d->part[POD_CASE].level == 33);
	assert(d->lid == 0);

	/* case away: above 100 is "not available": its last reading stays, with its age */
	advert(m, 0x21, 0x78, 0x0f, 0x11, 50, 50, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 2000, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 33 && d->part[POD_CASE].seen == 1001 && d->part[POD_LEFT].seen == 2000);

	/* the address resolves and the block does not open: dropped, nothing changes */
	uint8_t bad[3] = { 1, 2, 3 };
	advert(m, 0x21, 0x78, 0x0f, 0x11, 1, 1, 1, bad);
	assert(pods_advert(&w, RPA, m, 27, 3000, &which) == PODS_ADV_MISMATCH);
	assert(d->part[POD_LEFT].level == 50);

	/* a stranger's address: not ours, whatever it claims. Only "nearby". */
	uint8_t stranger[6] = { 0x40, 1, 2, 3, 4, 5 };
	advert(m, 0x21, 0x78, 0x0f, 0x11, 1, 1, 1, NO_SUFFIX);
	assert(pods_advert(&w, stranger, m, 27, 4000, &which) == PODS_ADV_NEARBY);
	assert(d->part[POD_LEFT].level == 50 && w.n_nearby == 1 && w.nearby[0].level[POD_LEFT] == 80);
	/* the other pod's advert updates the one row */
	assert(pods_advert(&w, stranger, m, 27, 4001, &which) == PODS_ADV_NEARBY && w.n_nearby == 1);
	/* not one of Apple's proximity messages */
	assert(pods_advert(&w, stranger, B(0x10, 0x02, 0x01, 0x02), 5000, &which) == PODS_ADV_IGNORED);

	/* no keys at all: nearby, never a reading for the device */
	static struct pods_world w2;
	memset(&w2, 0, sizeof(w2));
	struct pods_device *nokeys = pods_add(&w2, MAC);
	advert(m, 0x21, 0x78, 0x0f, 0x11, 1, 1, 1, NO_SUFFIX);
	assert(pods_advert(&w2, RPA, m, 27, 1, &which) == PODS_ADV_NEARBY && nokeys->part[POD_LEFT].level == -1);

	/* with AAP reporting a pod, the advert does not overwrite it, however new */
	d->link = PODS_LINK_READY;
	d->aap_live[POD_LEFT] = d->aap_live[POD_RIGHT] = d->aap_live[POD_CASE] = true;
	d->part[POD_LEFT] = (struct pods_battery){ 42, false, 100, 'a' };
	d->part[POD_CASE] = (struct pods_battery){ 60, false, 5990, 'a' };
	advert(m, 0x21, 0x78, 0x0f, 0x11, 1, 1, 1, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 6000, &which) == PODS_ADV_READING && d->part[POD_LEFT].level == 42);
	assert(d->part[POD_CASE].level == 60);		/* the link's word on the case is fresh */
	/* ... the case's is only good for so long, since the link reports on events */
	assert(pods_advert(&w, RPA, m, 27, 5990 + PODS_AAP_CASE_FRESH_S, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 1 && d->part[POD_CASE].source == 'b' && d->part[POD_LEFT].level == 42);
	/* the link says the case is Disconnected (lid shut): a broadcast may say */
	d->aap_live[POD_CASE] = false;
	d->part[POD_CASE] = (struct pods_battery){ 60, false, 7000, 'a' };
	assert(pods_advert(&w, RPA, m, 27, 7001, &which) == PODS_ADV_READING && d->part[POD_CASE].level == 1);
}

/* The case's own advert (first byte 06), constructed from LibrePods' reading of it. */
static size_t case_advert(uint8_t *m, const uint8_t *key, uint8_t head, int kase, int left, int right) {
	uint8_t pt[16] = { 0x11, 0x20, 0x33, (uint8_t)kase, (uint8_t)left, (uint8_t)right, 7, 7, 7 };
	m[0] = 0x07;
	m[1] = head;		/* (top bits flags) */
	m[2] = 0x06;
	pods_aes_encrypt(key, pt, m + 3);
	return 19;
}

static void test_case_advert(void) {
	static struct pods_world w;
	struct pods_device *d = keyed(&w);
	uint8_t m[40], stranger[6] = { 0x12, 1, 2, 3, 4, 5 };	/* not the pods' RPA: it needn't be */
	int which = -1;

	/* not linked: charging is read, and left and right, from the case itself */
	size_t n = case_advert(m, ENC, 0x11, 0x80 | 64, 33, 0xff);
	assert(ble_find_case(m, n) == m + 2 && !ble_find_proximity(m, n));
	assert(pods_advert(&w, stranger, m, n, 100, &which) == PODS_ADV_READING && which == 0);
	assert(d->part[POD_CASE].level == 64 && d->part[POD_CASE].charging && d->part[POD_CASE].source == 'k');
	assert(d->part[POD_LEFT].level == 33 && d->part[POD_RIGHT].level == -1);	/* FF: none */
	/* flags in the length's top bits are not length */
	n = case_advert(m, ENC, 0x31, 50, 0xff, 0xff);
	assert(pods_advert(&w, stranger, m, n, 101, &which) == PODS_ADV_READING && d->part[POD_CASE].level == 50);
	assert(!d->part[POD_CASE].charging);

	/* another pair's case: nothing, and not "nearby" either */
	static const uint8_t OTHER[16] = { 1, 2, 3 };
	n = case_advert(m, OTHER, 0x11, 10, 10, 10);
	assert(pods_advert(&w, stranger, m, n, 102, &which) == PODS_ADV_IGNORED);
	assert(d->part[POD_CASE].level == 50 && w.n_nearby == 0);

	/* THE case asked for: left pod worn and linked, right in a closed case. The link says the
	 * case and right are Disconnected, the charger goes in, and the case's advert tells. */
	d->link = PODS_LINK_READY;
	struct pods_out o = { 0 };
	assert(pods_link_packet(d, 1, 200, PKT(0x04, 0x00, 0x03, 0x04, 0x01, 0x46, 0x02, 0x01, 0x02, 0x01, 0x00, 0x04, 0x01, 0x08, 0x01, 0x00, 0x04, 0x01), &o));
	assert(d->aap_live[POD_LEFT] && !d->aap_live[POD_RIGHT] && !d->aap_live[POD_CASE]);
	n = case_advert(m, ENC, 0x11, 0x80 | 77, 12, 0x80 | 99);
	assert(pods_advert(&w, stranger, m, n, 300, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 77 && d->part[POD_CASE].charging && d->part[POD_CASE].seen == 300);
	assert(d->part[POD_RIGHT].level == 99 && d->part[POD_RIGHT].source == 'k');
	assert(d->part[POD_LEFT].level == 70 && d->part[POD_LEFT].source == 'a');	/* linked: stays */
}

/* Real bytes, heard from this phone off a Pro 2 USB-C (A3047) on 2026-11-07, left pod worn and
 * linked, right pod in the closed case. Only the clear part can be checked here: the key that
 * opens the rest stays on the phone. There it opened to left 99%, FF for right and case, zeros
 * for the MAC's suffix, as the layouts say. */
static void test_captured(void) {
	static const uint8_t real[] = { 0x07, 0x19, 0x01, 0x24, 0x20, 0x22, 0xf9, 0x8f, 0x11, 0x00, 0x04, 0xc2, 0x0a, 0xfc, 0xd5,
		0xf3, 0xfe, 0xf8, 0x19, 0x3a, 0x3c, 0x16, 0x25, 0x1a, 0xf5, 0xa5, 0x68 };
	struct ble_clear c;
	assert(ble_parse_clear(ble_find_proximity(real, sizeof(real)), &c));
	assert(c.product == 0x2024 && c.sender_left && c.in_ear[POD_LEFT] && !c.in_ear[POD_RIGHT]);
	assert(c.level[POD_LEFT] == 90 && c.level[POD_RIGHT] == -1 && c.level[POD_CASE] == -1 && c.lid == -1);
	assert(c.connection == 4);
}

/* The case's advert as heard (desk-handled.txt, 2026-11-07): a Find My field, then ours, from
 * D7:97:.., lid closed, right pod charging in it. The key stays on the phone, where this
 * ciphertext opened to the plaintext below; here that plaintext goes under a test key, behind
 * the real Find My field. */
static void test_captured_case(void) {
	static const uint8_t real[] = { 0x12, 0x02, 0xac, 0x00, 0x07, 0x11, 0x06, 0x7d, 0xff, 0xfa, 0x5b, 0x6d, 0x16, 0x0e, 0x89, 0x14,
		0xcc, 0x26, 0x53, 0xed, 0x3c, 0x9b, 0x2f };
	assert(ble_find_case(real, sizeof(real)) == real + 6 && !ble_find_proximity(real, sizeof(real)));
	static const uint8_t PLAIN_CLOSED[16] = { 0x18, 0x20, 0x0d, 0x15, 0xff, 0xe1, 0x5a, 0x86, 0, 0, 0, 0, 0x70, 0x05, 0, 0 };	/* 97, charging */
	static const uint8_t PLAIN_OPEN[16] = { 0x18, 0x20, 0x05, 0x15, 0xff, 0xe0, 0x5a, 0x86, 0, 0, 0, 0, 0x6d, 0x05, 0, 0 };
	uint8_t m[23];
	memcpy(m, real, 7);
	pods_aes_encrypt(ENC, PLAIN_CLOSED, m + 7);
	static struct pods_world w;
	struct pods_device *d = keyed(&w);
	const uint8_t static_addr[6] = { 0xd7, 0x97, 0x43, 0x5f, 0x39, 0xb4 };	/* the IRK doesn't resolve it */
	int which;
	assert(!ble_rpa_resolves(IRK, static_addr));

	/* the coordinator's question: left linked and reporting; case and right Disconnected */
	d->link = PODS_LINK_READY;
	d->aap_live[POD_LEFT] = true;
	d->part[POD_LEFT] = (struct pods_battery){ 93, false, 10, 'a' };
	d->part[POD_RIGHT] = (struct pods_battery){ 90, true, 10, 'a' };
	assert(pods_advert(&w, static_addr, m, sizeof(m), 1000, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 21 && !d->part[POD_CASE].charging && d->part[POD_CASE].source == 'k');
	assert(d->part[POD_RIGHT].level == 97 && d->part[POD_RIGHT].charging && d->part[POD_RIGHT].source == 'k');
	assert(d->part[POD_LEFT].level == 93 && d->part[POD_LEFT].source == 'a');
	assert(d->lid == 1 && d->ble_place[POD_RIGHT] == EAR_CASE_CLOSED);
	/* the lid opens */
	pods_aes_encrypt(ENC, PLAIN_OPEN, m + 7);
	assert(pods_advert(&w, static_addr, m, sizeof(m), 1001, &which) == PODS_ADV_READING);
	assert(d->lid == 0 && d->ble_place[POD_RIGHT] == EAR_CASE_OPEN && d->part[POD_RIGHT].level == 96);

	/* a 1% reading from the case isn't called coarse in the file readers see */
	char *j = NULL;
	size_t jn = 0;
	FILE *f = open_memstream(&j, &jn);
	pods_json(f, &w, true, false, 1001, NULL);
	fclose(f);
	assert(strstr(j, "\"right\":{\"level\":96,\"charging\":true,\"seen\":1001,\"source\":\"case\"}"));
	assert(strstr(j, "\"lid\":\"open\""));
	free(j);
}

/* In use where, and where each pod is, from the pods' own adverts. */
static void test_offline_status(void) {
	static struct pods_world w;
	struct pods_device *d = keyed(&w);
	uint8_t m[40];
	int which;
	memcpy(w.own_mac, "\x01\x02\x03\xa1\xa2\xa3", 6);	/* advert()'s host is a1 a2 a3 */
	w.has_own = true;
	/* conn 04, our address: it was this phone */
	advert(m, 0x20, 0xf9, 0x8f, 0x11, 93, 0xff, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 100, &which) == PODS_ADV_READING);
	assert(d->host == PODS_HOST_THIS && d->connection == 4);
	assert(d->ble_place[POD_LEFT] == EAR_OUT && d->ble_place[POD_RIGHT] == -1);	/* nothing said of the right */
	/* another's */
	w.own_mac[5] = 0;
	assert(pods_advert(&w, RPA, m, 27, 101, &which) == PODS_ADV_READING && d->host == PODS_HOST_OTHER);
	/* in ear */
	advert(m, 0x22, 0xf9, 0x8f, 0x11, 93, 0xff, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 102, &which) == PODS_ADV_READING && d->ble_place[POD_LEFT] == EAR_IN);
	/* one pod in the open case, from the worn one (0x31) and from itself (0x51) */
	advert(m, 0x31, 0xa2, 0x4f, 0x11, 93, 96, 21, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 103, &which) == PODS_ADV_READING);
	assert(d->ble_place[POD_RIGHT] == EAR_CASE_OPEN && d->ble_place[POD_LEFT] == EAR_OUT);
	advert(m, 0x51, 0x99, 0x2f, 0x31, 96, 93, 21, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 104, &which) == PODS_ADV_READING);
	assert(d->ble_place[POD_RIGHT] == EAR_CASE_OPEN && d->ble_place[POD_LEFT] == EAR_OUT && d->lid == 0);
	/* a pod in a closed case says 00 while its partner is in use: not "nothing" until it lasts */
	uint8_t *conn = m + 2 + 8;
	advert(m, 0x04, 0xf9, 0x2f, 0x39, 96, 0xff, 21, SUFFIX);
	*conn = 0;
	assert(pods_advert(&w, RPA, m, 27, 105, &which) == PODS_ADV_READING && d->host == PODS_HOST_OTHER);
	assert(pods_advert(&w, RPA, m, 27, 120, &which) == PODS_ADV_READING && d->host == PODS_HOST_NONE);

	/* what shows after the link has gone: the mode, kept; a restart keeps it */
	static struct pods_world back;
	struct pods_out o = { 0 };
	d->link = PODS_LINK_READY;
	d->mode = AAP_MODE_ANC;
	d->known[0x28] = true;
	d->raw[0x28][0] = AAP_ON;
	pods_link_packet(d, 1, 500, PKT(0x09, 0x00, 0x25, 0x01, 0, 0, 0), &o);
	assert(d->link_seen == 500);
	pods_link_gone(d, false);
	assert(d->mode == 0 && d->last_mode == AAP_MODE_ANC);
	assert(pods_cmd_mode(d, 1, "off", &o) != NULL);		/* and it can't be changed */
	char *j = NULL;
	size_t jn = 0;
	FILE *f = open_memstream(&j, &jn);
	pods_json(f, &w, true, false, 600, NULL);
	fclose(f);
	assert(strstr(j, "\"last_mode\":\"anc\"") && strstr(j, "\"mode\":null") && strstr(j, "\"conversation\":1"));
	assert(strstr(j, "\"link_seen\":500") && strstr(j, "\"host\":\"none\"") && strstr(j, "\"connect\":\"idle\""));
	free(j);
	f = open_memstream(&j, &jn);
	pods_readings_write(f, &w);
	fclose(f);
	for (char *line = strtok(j, "\n"); line; line = strtok(NULL, "\n")) pods_readings_line(&back, line);
	free(j);
	assert(back.dev[0].last_mode == AAP_MODE_ANC && back.dev[0].link_seen == 500);
}

static void test_coarse(void) {
	static struct pods_world w;
	struct pods_device *d = keyed(&w);
	uint8_t m[40];
	int which;
	/* the block says nothing of the case, the clear nibble says 50-59: 57 keeps its precision */
	d->part[POD_CASE] = (struct pods_battery){ 57, false, 100, 'a' };
	advert(m, 0x21, 0x78, 0x45, 0x11, 50, 50, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 200, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 57 && d->part[POD_CASE].source == 'a' && d->part[POD_CASE].seen == 200);
	assert(d->part[POD_CASE].charging);
	/* it disagrees: the step, and said to be one */
	advert(m, 0x21, 0x78, 0x03, 0x11, 50, 50, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 300, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 30 && d->part[POD_CASE].source == 'c');
	/* FF in both: nothing to say */
	advert(m, 0x21, 0x78, 0x0f, 0x11, 50, 50, 0xff, NO_SUFFIX);
	assert(pods_advert(&w, RPA, m, 27, 400, &which) == PODS_ADV_READING);
	assert(d->part[POD_CASE].level == 30 && d->part[POD_CASE].seen == 300);
}

/* A whole link, as the captures in LinuxPods' docs have it, with no I/O. */
static void test_session(void) {
	static struct pods_world w;
	memset(&w, 0, sizeof(w));
	struct pods_device *d = pods_add(&w, MAC);
	d->product = 0x2024;
	struct pods_out o = { 0 };
	uint32_t t = 1000;

	pods_link_open(d, t, &o);
	assert(o.n == 1 && o.len[0] == 16 && !memcmp(o.pkt[0], AAP_HANDSHAKE, 16));
	/* nothing goes out before the answer, whatever comes */
	memset(&o, 0, sizeof(o));
	assert(pods_link_packet(d, t, 1, PKT(0x2b, 0x00, 0x01), &o) == 0 && o.n == 0);
	assert(pods_cmd_mode(d, t, "anc", &o) != NULL && o.n == 0);
	assert(!strcmp(pods_cmd_set(d, "conversation", "1", &o), "not-linked") && o.n == 0);

	t += 1200;
	assert(pods_link_packet(d, t, 1, B(0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00), &o) & PODS_EV_CHANGED);
	assert(o.n == 3 && d->link == PODS_LINK_READY);
	assert(!memcmp(o.pkt[0], AAP_NOTIFY_REQ, 10) && o.len[0] == 10);
	assert(!memcmp(o.pkt[1], AAP_FEATURES_REQ, 14) && o.len[1] == 14);
	assert(!memcmp(o.pkt[2], AAP_KEY_REQ, 8) && o.len[2] == 8);
	memset(&o, 0, sizeof(o));

	/* battery, right primary */
	assert(pods_link_packet(d, t, 50, PKT(0x04, 0x00, 0x03, 0x02, 0x01, 0x46, 0x02, 0x01, 0x04, 0x01, 0x50, 0x02, 0x01, 0x08, 0x01, 0x00, 0x04, 0x01), &o) & PODS_EV_SAVE);
	assert(d->part[POD_RIGHT].level == 70 && d->part[POD_LEFT].level == 80 && d->part[POD_CASE].level == -1);
	assert(d->part[POD_LEFT].source == 'a');
	/* primary, then where each pod is: right in the ear, left in the open case */
	pods_link_packet(d, t, 50, PKT(0x08, 0x00, 0x02, 0x00, 0x01, 0x00), &o);
	pods_link_packet(d, t, 50, PKT(0x06, 0x00, 0x00, 0x02), &o);
	assert(d->primary == POD_RIGHT && d->place[0] == EAR_IN && d->place[1] == EAR_CASE_OPEN);
	/* opcodes we have no use for, and short packets, do nothing */
	assert(pods_link_packet(d, t, 50, PKT(0x0c, 0x00, 1, 2, 3, 4, 5, 6, 1, 2), &o) == 0);
	assert(pods_link_packet(d, t, 50, PKT(0x02, 0x00, 0), &o) == 0);
	assert(pods_link_packet(d, t, 50, PKT(0x09, 0x00), &o) == 0);
	assert(pods_link_packet(d, t, 50, PKT(0x04), &o) == 0);
	assert(o.n == 0);

	/* the keys: reported as new once, not again */
	uint8_t kp[64];
	size_t kn = key_packet(kp);
	assert(pods_link_packet(d, t, 50, kp, kn, &o) & PODS_EV_KEYS);
	assert(d->has_keys && !memcmp(d->irk, IRK, 16));
	assert(pods_link_packet(d, t, 50, kp, kn, &o) == 0);

	/* the dump: the mode first. 0x25's 01 is no mode: byte 6 first. */
	assert(!d->settings_known && d->mode == 0);
	assert(pods_link_packet(d, t, 50, PKT(0x09, 0x00, 0x0d, 0x04, 0, 0, 0), &o) & PODS_EV_CHANGED);
	assert(d->settings_known && d->mode == AAP_MODE_ADAPTIVE);
	pods_link_packet(d, t, 50, PKT(0x09, 0x00, 0x25, 0x01, 0, 0, 0), &o);
	assert(d->mode == AAP_MODE_ADAPTIVE);
	const struct pods_setting *swipe = pods_setting_find("swipe_volume");
	assert(pods_setting_value(d, swipe) == 1);
	pods_link_packet(d, t, 50, PKT(0x09, 0x00, 0x28, 0x02, 0, 0, 0), &o);
	assert(pods_setting_value(d, pods_setting_find("conversation")) == 0);
	pods_link_packet(d, t, 50, PKT(0x09, 0x00, 0x1f, 0x50, 0x50, 0, 0), &o);
	assert(pods_setting_value(d, pods_setting_find("chime_volume")) == 80);

	/* commands: on is 01, off is 02 */
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "conversation", "1", &o) == NULL);
	assert(o.n == 1 && !memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x28\x01\x00\x00\x00", 11));
	assert(pods_setting_value(d, pods_setting_find("conversation")) == 1);
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "conversation", "0", &o) == NULL);
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x28\x02\x00\x00\x00", 11));
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "adaptive_strength", "50", &o) == NULL);
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x2e\x32\x00\x00\x00", 11));
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "adaptive_strength", "101", &o) != NULL && o.n == 0);
	assert(pods_cmd_set(d, "conversation", "maybe", &o) != NULL && o.n == 0);
	assert(pods_cmd_set(d, "no_such", "1", &o) != NULL && o.n == 0);
	/* chime: both its bytes; press and hold: this pod's, leaving the other's */
	assert(pods_cmd_set(d, "chime_volume", "20", &o) == NULL);
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x1f\x14\x14\x00\x00", 11));
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "hold_left", "5", &o) == NULL);	/* never reported: both */
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x16\x05\x05\x00\x00", 11));
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "hold_right", "1", &o) == NULL);
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x16\x01\x05\x00\x00", 11));
	assert(pods_cmd_set(d, "hold_right", "3", &o) != NULL);
	memset(&o, 0, sizeof(o));
	/* the cycle: at least two; with Off in it, the gate first */
	assert(pods_cmd_set(d, "mode_cycle", "2", &o) != NULL && o.n == 0);
	assert(pods_cmd_set(d, "mode_cycle", "6", &o) == NULL && o.n == 1);
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_set(d, "mode_cycle", "7", &o) == NULL && o.n == 2);
	assert(o.pkt[0][6] == AAP_SET_ALLOW_OFF && o.pkt[1][6] == 0x1a && o.pkt[1][7] == 7);
	memset(&o, 0, sizeof(o));

	/* modes, and Off's gate right before it */
	assert(pods_cmd_mode(d, t, "anc", &o) == NULL);
	assert(o.n == 1 && !memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x0d\x02\x00\x00\x00", 11));
	assert(d->mode == AAP_MODE_ANC);	/* recorded as sent */
	memset(&o, 0, sizeof(o));
	t += 5000;
	assert(pods_cmd_mode(d, t, "off", &o) == NULL && o.n == 2);
	assert(!memcmp(o.pkt[0], "\x04\x00\x04\x00\x09\x00\x34\x01\x00\x00\x00", 11));
	assert(!memcmp(o.pkt[1], "\x04\x00\x04\x00\x09\x00\x0d\x01\x00\x00\x00", 11));
	memset(&o, 0, sizeof(o));
	/* the stale report of the old mode, then the echo */
	pods_link_packet(d, t + 330, 50, PKT(0x09, 0x00, 0x0d, 0x02, 0, 0, 0), &o);
	assert(d->mode == AAP_MODE_OFF);
	pods_link_packet(d, t + 1400, 50, PKT(0x09, 0x00, 0x0d, 0x01, 0, 0, 0), &o);
	assert(d->mode == AAP_MODE_OFF && d->mode_sent_ms == 0);
	/* a stem press changes it, reported */
	t += 10000;
	assert(pods_link_packet(d, t, 50, PKT(0x09, 0x00, 0x0d, 0x03, 0, 0, 0), &o) & PODS_EV_CHANGED);
	assert(d->mode == AAP_MODE_TRANSPARENCY);
	/* one pod out: Off for 50 ms, then back. It never reaches the state. */
	pods_link_packet(d, t + 1, 50, PKT(0x09, 0x00, 0x0d, 0x01, 0, 0, 0), &o);
	assert(d->mode == AAP_MODE_TRANSPARENCY && !pods_tick(d, t + 40));
	pods_link_packet(d, t + 50, 50, PKT(0x09, 0x00, 0x0d, 0x03, 0, 0, 0), &o);
	assert(!pods_tick(d, t + 2000) && d->mode == AAP_MODE_TRANSPARENCY);
	/* an Off that lasts is one */
	pods_link_packet(d, t + 3000, 50, PKT(0x09, 0x00, 0x0d, 0x01, 0, 0, 0), &o);
	assert(pods_tick(d, t + 3000 + PODS_OFF_DEBOUNCE_MS) && d->mode == AAP_MODE_OFF);

	/* a rename goes, and shows */
	memset(&o, 0, sizeof(o));
	assert(pods_cmd_rename(d, "Pods", &o) == NULL && o.n == 1 && o.pkt[0][4] == 0x1a && !strcmp(d->name, "Pods"));
	assert(pods_cmd_rename(d, "", &o) != NULL);

	/* the link goes: readings stay, control state does not */
	pods_link_gone(d, false);
	assert(d->link == PODS_LINK_NONE && d->mode == 0 && !d->settings_known && d->part[POD_LEFT].level == 80);
	assert(pods_cmd_mode(d, t, "anc", &o) != NULL);
	/* and the next link does the handshake again */
	memset(&o, 0, sizeof(o));
	pods_link_open(d, t, &o);
	assert(o.n == 1 && d->link == PODS_LINK_WAIT_ANSWER);
	/* no dump on this link: the rows open anyway, in time */
	pods_link_packet(d, t, 1, B(0x01, 0x00, 0x04, 0x00), &o);
	assert(!pods_tick(d, t + 1000) && !d->settings_known);
	assert(pods_tick(d, t + PODS_DUMP_WAIT_MS) && d->settings_known);
}

static void test_gating(void) {
	static struct pods_world w;
	memset(&w, 0, sizeof(w));
	struct pods_device *d = pods_add(&w, MAC);
	struct pods_out o = { 0 };
	d->link = PODS_LINK_READY;

	/* first generation: no modes, no Conversation Awareness */
	d->product = 0x2002;
	assert(!strcmp(pods_cmd_mode(d, 1, "anc", &o), "unsupported"));
	assert(!strcmp(pods_cmd_set(d, "conversation", "1", &o), "unsupported"));
	assert(pods_setting_visible(d, pods_setting_find("ear_detection")));
	assert(!pods_setting_visible(d, pods_setting_find("swipe_volume")));
	/* Pro 1: modes, not Adaptive */
	d->product = 0x200e;
	assert(pods_cmd_mode(d, 1, "anc", &o) == NULL);
	assert(!strcmp(pods_cmd_mode(d, 1, "adaptive", &o), "unsupported"));
	/* both Pro 2 have the same */
	const struct pods_model *a = pods_model_find(0x2014), *b = pods_model_find(0x2024);
	assert(a && b && a->caps == b->caps && (b->caps & CAP_ADAPTIVE) && (b->caps & CAP_SWIPE));
	/* a model we have never heard of shows what it reports, no more */
	d->product = 0x2999;
	assert(!pods_setting_visible(d, pods_setting_find("swipe_volume")));
	d->known[0x25] = true;
	assert(pods_setting_visible(d, pods_setting_find("swipe_volume")));

	assert(pods_modalias_product("bluetooth:v004Cp2024d0100") == 0x2024);
	assert(pods_modalias_product("usb:v05ACp200Ed0100") == 0x200e);
	assert(pods_modalias_product("usb:v1D6Bp0246d0557") == 0);
	assert(pods_modalias_product(NULL) == 0);

	char mac[18];
	assert(pods_path_mac("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", mac) && !strcmp(mac, MAC));
	assert(pods_path_mac("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/sep1/fd0", mac) && !strcmp(mac, MAC));
	assert(!pods_path_mac("/org/bluez/hci0", mac));
	assert(!pods_path_mac("/org/bluez/hci0/dev_AA_BB", mac));
}

static void test_scan(void) {
	struct pods_scan_in s = { .powered = true, .screen_on = true, .known = 1, .keyed_unlinked = 1 };
	struct pods_scan_plan p = pods_scan_policy(&s);
	assert(p.on_s == 3 && p.every_s == 60);
	s.ui_open = true;
	p = pods_scan_policy(&s);
	assert(p.on_s == 4 && p.every_s == 10);
	/* Bluetooth off: never, and nothing here turns it on */
	s.powered = false;
	assert(pods_scan_policy(&s).on_s == 0);
	s.powered = true;
	/* the screen off: never */
	s.screen_on = false;
	assert(pods_scan_policy(&s).on_s == 0);
	s.screen_on = true;
	/* every known pair linked: still, slowly: the case's broadcasts say what the link can't */
	s.linked = 1;
	s.keyed_unlinked = 0;
	s.keyed_linked = 1;
	s.ui_open = false;
	p = pods_scan_policy(&s);
	assert(p.on_s == 3 && p.every_s == 60);
	s.ui_open = true;
	p = pods_scan_policy(&s);
	assert(p.on_s == 4 && p.every_s == 10);
	s.keyed_linked = 0;
	/* no keys: only while the app is open */
	struct pods_scan_in n = { .powered = true, .screen_on = true };
	assert(pods_scan_policy(&n).on_s == 0);
	n.ui_open = true;
	assert(pods_scan_policy(&n).on_s == 4);
}

static char *to_string(void (*write)(FILE *, void *), void *arg) {
	static char buf[16384];
	FILE *f = tmpfile();
	assert(f);
	write(f, arg);
	rewind(f);
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

static void w_json(FILE *f, void *w) { pods_json(f, w, true, false, 5000, NULL); }
static void w_bar(FILE *f, void *w) { pods_bar_write(f, w); }
static void w_keys(FILE *f, void *w) { pods_keys_write(f, w); }
static void w_readings(FILE *f, void *w) { pods_readings_write(f, w); }

static void test_files(void) {
	static struct pods_world w, back;
	struct pods_device *d = keyed(&w);
	d->product = 0x2024;
	snprintf(d->name, sizeof(d->name), "Sam \"x\" Pods");
	d->part[POD_LEFT] = (struct pods_battery){ 80, true, 1000, 'a' };
	d->part[POD_CASE] = (struct pods_battery){ 33, false, 900, 'b' };
	d->known[0x28] = true;
	d->raw[0x28][0] = AAP_OFF;
	d->lid = 1;
	d->lid_seen = 900;
	d->link = PODS_LINK_READY;
	d->mode = AAP_MODE_ANC;
	d->primary = POD_LEFT;
	d->place[0] = EAR_IN;
	d->place[1] = EAR_CASE_CLOSED;
	d->settings_known = true;

	const char *j = to_string(w_json, &w);
	assert(strstr(j, "\"name\":\"Sam \\\"x\\\" Pods\""));
	assert(strstr(j, "\"model\":\"pro2-usbc\""));
	assert(strstr(j, "\"left\":{\"level\":80,\"charging\":true,\"seen\":1000,\"source\":\"aap\"}"));
	assert(strstr(j, "\"right\":{\"level\":null}"));	/* null, never 0 */
	assert(strstr(j, "\"mode\":\"anc\""));
	assert(strstr(j, "\"conversation\":0"));
	assert(strstr(j, "\"ear\":{\"left\":\"in_ear\",\"right\":\"case_closed\"}"));
	/* the keys stay out of what any reader sees */
	char irk_hex[33], enc_hex[33];
	pods_hex(irk_hex, IRK, 16);
	pods_hex(enc_hex, ENC, 16);
	assert(!strstr(j, irk_hex) && !strstr(j, enc_hex));

	const char *b = to_string(w_bar, &w);
	assert(strstr(b, "link=1\n") && strstr(b, "left=80\n") && strstr(b, "case=33\n") && strstr(b, "charging=1\n"));
	assert(strstr(b, "known=" MAC "\n") && !strstr(b, "\nright="));
	char tmp[] = "/tmp/pods-bar-XXXXXX";
	int fd = mkstemp(tmp);
	assert(fd >= 0);
	FILE *f = fdopen(fd, "w+");
	fputs(b, f);
	rewind(f);
	struct pods_bar bar;
	pods_bar_parse(f, &bar);
	fclose(f);
	unlink(tmp);
	assert(bar.link && bar.left == 80 && bar.right == -1 && bar.kase == 33 && bar.charging);
	assert(pods_bar_knows(&bar, MAC) && !pods_bar_knows(&bar, "00:00:00:00:00:00"));
	/* the writer: a pod out of the case, ear or hand, is in use, by which is primary */
	{
		struct pods_device *d = &w.dev[0];
		int primary = d->primary, p0 = d->place[0], p1 = d->place[1];
		const struct { int primary, p0, p1; const char *want; } ears[] = {
			{ POD_RIGHT, EAR_IN, EAR_CASE_CLOSED, "single=right\n" },
			{ POD_LEFT, EAR_CASE_CLOSED, EAR_IN, "single=right\n" },
			{ POD_LEFT, EAR_OUT, EAR_CASE_OPEN, "single=left\n" },
			{ POD_LEFT, EAR_IN, EAR_OUT, NULL },
			{ POD_LEFT, EAR_IN, EAR_IN, NULL },
			{ POD_LEFT, EAR_OUT, EAR_OUT, NULL },
			{ POD_LEFT, EAR_CASE_OPEN, EAR_CASE_CLOSED, NULL },
			{ -1, EAR_IN, EAR_CASE_OPEN, NULL },
			{ POD_LEFT, EAR_IN, -1, NULL },
		};
		for (size_t k = 0; k < sizeof(ears) / sizeof(ears[0]); k++) {
			d->primary = ears[k].primary;
			d->place[0] = ears[k].p0;
			d->place[1] = ears[k].p1;
			const char *got = to_string(w_bar, &w);
			if (ears[k].want) assert(strstr(got, ears[k].want));
			else assert(!strstr(got, "single="));
		}
		d->primary = primary;
		d->place[0] = p0;
		d->place[1] = p1;
	}
	/* which pods are in use, by what the file says */
	{
		const struct { const char *text; int single; bool cl, cr; } cases[] = {
			{ "link=1\nsingle=right\ncharge_left=1\ncharge_right=0\n", 1, true, false },
			{ "link=1\nsingle=left\ncharge_left=0\ncharge_right=1\n", 0, false, true },
			{ "link=1\nsingle=both\n", -1, false, false },
		};
		for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
			FILE *m = fmemopen((void *)cases[k].text, strlen(cases[k].text), "r");
			struct pods_bar v;
			pods_bar_parse(m, &v);
			fclose(m);
			assert(v.single == cases[k].single && v.charge_left == cases[k].cl && v.charge_right == cases[k].cr);
		}
	}

	/* out of the keys file and back; a broken line is skipped */
	char *k = to_string(w_keys, &w);
	assert(strstr(k, irk_hex));
	memset(&back, 0, sizeof(back));
	assert(pods_keys_line(&back, k));
	assert(!pods_keys_line(&back, "AA:BB:CC:DD:EE:00 zz 11"));
	assert(back.n == 1 && back.dev[0].has_keys && !memcmp(back.dev[0].irk, IRK, 16) && !memcmp(back.dev[0].enc, ENC, 16));

	/* readings, a line at a time: no keys in them */
	char *r = to_string(w_readings, &w);
	assert(!strstr(r, irk_hex) && !strstr(r, enc_hex));
	for (char *line = strtok(r, "\n"); line; line = strtok(NULL, "\n")) pods_readings_line(&back, line);
	struct pods_device *c = pods_find(&back, MAC);
	assert(c && !strcmp(c->name, d->name) && c->product == 0x2024);
	assert(c->part[POD_LEFT].level == 80 && c->part[POD_LEFT].seen == 1000 && c->part[POD_LEFT].source == 'a');
	assert(c->part[POD_CASE].level == 33 && c->part[POD_RIGHT].level == -1 && c->lid == 1);
	assert(pods_setting_value(c, pods_setting_find("conversation")) == 0);
	/* a restart starts with no link */
	assert(c->link == PODS_LINK_NONE && c->mode == 0);

	/* forgetting */
	pods_add(&back, "11:22:33:44:55:66");
	pods_remove(&back, MAC);
	assert(back.n == 1 && !strcmp(back.dev[0].mac, "11:22:33:44:55:66"));
	assert(!pods_add(&back, "not a mac"));

	/* nearby: shown while fresh, then gone */
	memset(&w, 0, sizeof(w));
	struct ble_clear cl = { .product = 0x2024, .lid = -1, .level = { 80, 70, -1 } };
	pods_nearby_note(&w, &cl, 4990);
	assert(strstr(to_string(w_json, &w), "AirPods Pro 2 (USB-C)"));
	w.nearby[0].seen = 4000;
	assert(!strstr(to_string(w_json, &w), "AirPods Pro 2 (USB-C)"));
	/* no link anywhere: the bar has nothing to show */
	assert(strstr(to_string(w_bar, &w), "link=0\n"));
}

int main(void) {
	test_aes();
	test_rpa();
	test_split();
	test_battery();
	test_builders();
	test_info();
	test_keys();
	test_clear();
	test_advert();
	test_case_advert();
	test_coarse();
	test_captured();
	test_captured_case();
	test_offline_status();
	test_session();
	test_gating();
	test_scan();
	test_files();
	puts("pods-proto: ok");
	return 0;
}
