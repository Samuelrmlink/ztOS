// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host tests for MMS: the PDU codec (mmspdu.c) and the access point
 * auto-fill (apnconf.c). No network, no modem. Numbers and addresses here
 * are made up (555-01xx, example.invalid).
 *
 *	make -C ztmetro check-mms
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../apnconf.h"
#include "../mmspdu.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { failures++; \
	fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static const char *fixture, *extra;

static void apn_case(const char *mccmnc, const char *gid1, const char *imsi, const char *carrier,
	const char *apn, const char *mmsc, const char *mms_apn)
{
	struct sim_id sim;
	struct apn_choice a;
	memset(&sim, 0, sizeof(sim));
	sim_id_from_mccmnc(&sim, mccmnc, 0);
	snprintf(sim.gid1, sizeof(sim.gid1), "%s", gid1);
	snprintf(sim.imsi, sizeof(sim.imsi), "%s", imsi);
	const char *files[] = { fixture, extra, NULL };
	bool found = apn_lookup(files, &sim, &a);
	CHECK(found == (carrier != NULL));
	if (!found || !carrier)
		return;
	CHECK_STR(a.carrier, carrier);
	CHECK_STR(a.apn, apn);
	CHECK_STR(a.mmsc, mmsc);
	CHECK_STR(a.mms_apn, mms_apn);
}

static void test_apn(void)
{
	const char *tmo = "http://mms.msg.eng.t-mobile.com/mms/wapenc";
	const char *wholesale = "http://wholesale.mmsmvno.com/mms/wapenc";
	/* T-Mobile: the data access point carries MMS; the Wi-Fi-only "tmus" is left out */
	apn_case("310260", "", "", "T-Mobile US LTE", "fast.t-mobile.com", tmo, "fast.t-mobile.com");
	/* a GID that matches nothing is T-Mobile's own */
	apn_case("310260", "FFFFFFFF", "", "T-Mobile US LTE", "fast.t-mobile.com", tmo, "fast.t-mobile.com");
	/* Mint, under either MNC, by its GID1 (a prefix of what the SIM gives) */
	apn_case("310260", "756DFFFFFFFFFFFF", "", "Mint", "Wholesale", wholesale, "Wholesale");
	apn_case("310240", "756d", "", "Mint", "Wholesale", wholesale, "Wholesale");
	/* MetroPCS and Red Pocket, from the stock file */
	apn_case("310260", "6D38", "", "MetroPCS US LTE", "fast.metropcs.com",
		"http://metropcs.mmsmvno.com/mms/wapenc", "fast.metropcs.com");
	apn_case("310260", "4201", "", "RED POCKET", "Wholesale", wholesale, "Wholesale");
	/* an IMSI rule (made-up IMSI) */
	apn_case("310260", "", "310260970000001", "Project Fi - T", "h2g2", "http://mmsc1.g-mms.com/mms/wapenc", "h2g2");
	/* an MNC of two digits, from six digits of IMSI; MMS on its own access point */
	apn_case("001010", "", "", "Test & Data", "test.data", "http://mmsc.example.invalid:8002/mms", "test.mms");
	/* nothing for this network */
	apn_case("999999", "", "", NULL, NULL, NULL, NULL);

	struct sim_id sim;
	struct apn_choice a;
	memset(&sim, 0, sizeof(sim));
	const char *files[] = { fixture, extra, NULL };
	sim_id_from_mccmnc(&sim, "310260", 0);
	CHECK(apn_lookup(files, &sim, &a));
	CHECK_STR(a.protocol, "IPV6");
	CHECK_STR(a.ims_apn, "ims");
	CHECK_STR(a.auth, "none");
	CHECK_STR(a.mms_proxy, "");
	CHECK(a.mms_port == 0);
	snprintf(sim.gid1, sizeof(sim.gid1), "4201");
	CHECK(apn_lookup(files, &sim, &a));
	CHECK_STR(a.mms_proxy, "216.155.165.50");
	CHECK(a.mms_port == 8080);
	memset(&sim, 0, sizeof(sim));
	sim_id_from_mccmnc(&sim, "001010", 0);
	CHECK(apn_lookup(files, &sim, &a));
	CHECK_STR(a.user, "u");
	CHECK_STR(a.password, "p");
	CHECK_STR(a.auth, "pap");
	CHECK_STR(a.mms_proxy, "10.0.0.1");
	CHECK(a.mms_port == 8080);
	/* only the extra file (stock unreadable): T-Mobile still found */
	const char *only_extra[] = { "/nonexistent/apns-conf.xml", extra, NULL };
	memset(&sim, 0, sizeof(sim));
	sim_id_from_mccmnc(&sim, "310260", 0);
	CHECK(apn_lookup(only_extra, &sim, &a));
	CHECK_STR(a.apn, "fast.t-mobile.com");
}

static void test_uintvar(void)
{
	uint32_t vals[] = { 0, 1, 0x7f, 0x80, 16383, 16384, 0x0fffffff, 0xffffffff };
	for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		uint8_t b[5];
		size_t n = wsp_put_uintvar(b, vals[i]), at = 0;
		uint32_t back = 0;
		CHECK(wsp_get_uintvar(b, n, &at, &back) && back == vals[i] && at == n);
	}
	uint8_t b[5];
	CHECK(wsp_put_uintvar(b, 0x80) == 2 && b[0] == 0x81 && b[1] == 0x00);
}

/* m-notification-ind, by hand, as a network sends one */
static size_t notification(uint8_t *p)
{
	size_t n = 0;
	const char *from = "+15550100/TYPE=PLMN", *loc = "http://mms.example.invalid/m/abc123";
	p[n++] = 0x8c; p[n++] = 0x82;
	p[n++] = 0x98; memcpy(p + n, "T0001", 6); n += 6;
	p[n++] = 0x8d; p[n++] = 0x92;
	p[n++] = 0x89; p[n++] = (uint8_t)(strlen(from) + 2); p[n++] = 0x80;
	memcpy(p + n, from, strlen(from) + 1); n += strlen(from) + 1;
	p[n++] = 0x8a; p[n++] = 0x80;
	p[n++] = 0x8e; p[n++] = 0x02; p[n++] = 0x75; p[n++] = 0x30;	/* 30000 bytes */
	p[n++] = 0x88; p[n++] = 0x05; p[n++] = 0x81; p[n++] = 0x03; p[n++] = 0x03; p[n++] = 0xf4; p[n++] = 0x80;
	p[n++] = 0x83; memcpy(p + n, loc, strlen(loc) + 1); n += strlen(loc) + 1;
	return n;
}

static void hex(char *out, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02X", b[i]);
}

/* an SMS-DELIVER carrying ud, to port 2948, part `part` of `total` */
static void sms_deliver(char *out, const uint8_t *ud, size_t len, int part, int total)
{
	uint8_t b[200];
	size_t n = 0;
	b[n++] = 0x07; b[n++] = 0x91; memcpy(b + n, "\x51\x55\x10\x00\x00\xf0", 6); n += 6;	/* centre */
	b[n++] = 0x44;				/* SMS-DELIVER, a user data header */
	b[n++] = 10; b[n++] = 0x91; memcpy(b + n, "\x55\x55\x10\x01\x00", 5); n += 5;	/* +5555011000 */
	b[n++] = 0x00; b[n++] = 0x04;		/* PID, 8-bit data */
	memcpy(b + n, "\x62\x11\x21\x10\x00\x00\x00", 7); n += 7;
	size_t udh = total ? 11 : 6;
	b[n++] = (uint8_t)(1 + udh + len);
	b[n++] = (uint8_t)udh;
	b[n++] = 0x05; b[n++] = 0x04; b[n++] = 0x0b; b[n++] = 0x84; b[n++] = 0x23; b[n++] = 0xf0;
	if (total) {
		b[n++] = 0x00; b[n++] = 0x03; b[n++] = 0x42; b[n++] = (uint8_t)total; b[n++] = (uint8_t)part;
	}
	memcpy(b + n, ud, len); n += len;
	hex(out, b, n);
}

static void test_push(void)
{
	uint8_t ind[300], push[400];
	size_t ilen = notification(ind), plen = 0;
	push[plen++] = 0x01; push[plen++] = 0x06; push[plen++] = 0x03;
	push[plen++] = 0xbe; push[plen++] = 0xaf; push[plen++] = 0x84;	/* mms-message; X-Wap-Application-Id: mms.ua */
	memcpy(push + plen, ind, ilen); plen += ilen;

	/* split in two SMS, as networks do for notifications over 128 bytes */
	char h1[500], h2[500];
	size_t half = plen / 2;
	sms_deliver(h1, push, half, 1, 2);
	sms_deliver(h2, push + half, plen - half, 2, 2);
	struct sms_wap w1, w2;
	CHECK(sms_wap_push(h1, &w1) && w1.total == 2 && w1.part == 1 && w1.ref == 0x42 && w1.len == half);
	CHECK(sms_wap_push(h2, &w2) && w2.part == 2 && w2.len == plen - half);
	CHECK_STR(w1.from, "+5555011000");
	uint8_t whole[400];
	memcpy(whole, w1.data, w1.len);
	memcpy(whole + w1.len, w2.data, w2.len);
	const uint8_t *body;
	size_t blen;
	CHECK(wsp_push_body(whole, plen, &body, &blen) && blen == ilen);
	struct mms_message m;
	CHECK(mms_decode(body, blen, &m));
	CHECK(m.type == MMS_NOTIFICATION_IND);
	CHECK_STR(m.transaction_id, "T0001");
	CHECK_STR(m.from, "+15550100");
	CHECK_STR(m.content_location, "http://mms.example.invalid/m/abc123");
	CHECK(m.size == 30000);
	CHECK(m.expiry == 259200 && m.expiry_relative);
	CHECK(m.message_class == 0x80 && m.version == 0x92);

	/* not WAP push: a text, and 8-bit data to another port */
	char text[200];
	sms_deliver(text, (const uint8_t *)"hello", 5, 0, 0);
	text[2 * 9] = '0';	/* first octet 0x04: no header */
	text[2 * 9 + 1] = '4';
	CHECK(!sms_wap_push(text, &w1));
	char other[300];
	sms_deliver(other, push, 20, 0, 0);
	other[2 * 26 + 2] = 'C';	/* port 0x0B84 becomes 0x0C84 */
	CHECK(!sms_wap_push(other, &w1));
	/* every truncation fails cleanly (run under -fsanitize to mean it) */
	for (size_t cut = 0; cut < ilen; cut++)
		mms_decode(body, cut, &m);
	for (size_t cut = 0; cut + 2 < strlen(h1); cut += 2) {
		char t[500];
		memcpy(t, h1, cut);
		t[cut] = 0;
		sms_wap_push(t, &w1);
	}
}

static void test_send_req(void)
{
	const char *to[] = { "+15550100", "5550101" };
	uint8_t jpeg[300];
	for (size_t i = 0; i < sizeof(jpeg); i++)
		jpeg[i] = (uint8_t)i;
	jpeg[0] = 0xff; jpeg[1] = 0xd8;
	struct mms_attachment att = { "image/jpeg", "image_0.jpg", jpeg, sizeof(jpeg) };
	uint8_t pdu[4096];
	const char *text = "h\xc3\xa9llo, a picture";
	size_t n = mms_encode_send_req(pdu, sizeof(pdu), "zt1", to, 2, NULL, text, &att, 1);
	CHECK(n > sizeof(jpeg));
	struct mms_message m;
	CHECK(mms_decode(pdu, n, &m));
	CHECK(m.type == MMS_SEND_REQ);
	CHECK_STR(m.transaction_id, "zt1");
	CHECK(m.n_to == 2);
	CHECK_STR(m.to[0], "+15550100");
	CHECK_STR(m.to[1], "5550101");
	CHECK_STR(m.from, "");
	CHECK_STR(m.content_type, "application/vnd.wap.multipart.related");
	CHECK(m.n_parts == 3);
	CHECK_STR(m.parts[0].content_type, "application/smil");
	CHECK(strstr((const char *)m.parts[0].data, "<img src=\"image_0.jpg\"") != NULL);
	CHECK_STR(m.parts[1].content_type, "image/jpeg");
	CHECK_STR(m.parts[1].name, "image_0.jpg");
	CHECK_STR(m.parts[1].content_id, "<image_0>");
	CHECK(m.parts[1].len == sizeof(jpeg) && !memcmp(m.parts[1].data, jpeg, sizeof(jpeg)));
	CHECK_STR(m.parts[2].content_type, "text/plain");
	CHECK_STR(m.parts[2].charset, "utf-8");
	CHECK(m.parts[2].len == strlen(text) && !memcmp(m.parts[2].data, text, strlen(text)));
	/* the first bytes are fixed by the standard: type, tid, version */
	CHECK(pdu[0] == 0x8c && pdu[1] == 0x80 && pdu[2] == 0x98 && !memcmp(pdu + 3, "zt1", 4) && pdu[7] == 0x8d);
	/* too small a buffer: refused, not overrun */
	CHECK(mms_encode_send_req(pdu, 100, "zt1", to, 2, NULL, text, &att, 1) == 0);
	/* text only, a big attachment's length over 127 (two-octet uintvar) */
	n = mms_encode_send_req(pdu, sizeof(pdu), "zt2", to, 1, "subj\xc3\xa9", "just text", NULL, 0);
	CHECK(mms_decode(pdu, n, &m) && m.n_parts == 2 && m.n_to == 1);
	CHECK_STR(m.subject, "subj\xc3\xa9");
	for (size_t cut = 0; cut < n; cut++)
		mms_decode(pdu, cut, &m);
}

/* m-retrieve-conf as an MMSC sends one: general-form content types, an
 * application header, From with an address, a UTF-8 subject */
static void test_retrieve(void)
{
	uint8_t p[1024];
	size_t n = 0;
	p[n++] = 0x8c; p[n++] = 0x84;
	p[n++] = 0x98; memcpy(p + n, "R1", 3); n += 3;
	p[n++] = 0x8d; p[n++] = 0x93;
	p[n++] = 0x8b; memcpy(p + n, "msgid-1", 8); n += 8;
	p[n++] = 0x85; p[n++] = 0x04; p[n++] = 0x67; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x01;
	const char *from = "+15550102/TYPE=PLMN";
	p[n++] = 0x89; p[n++] = (uint8_t)(strlen(from) + 2); p[n++] = 0x80;
	memcpy(p + n, from, strlen(from) + 1); n += strlen(from) + 1;
	p[n++] = 0x97; memcpy(p + n, "+15550103/TYPE=PLMN", 20); n += 20;
	p[n++] = 0x97; memcpy(p + n, "+15550104/TYPE=PLMN", 20); n += 20;
	const char *subj = "caf\xc3\xa9";
	p[n++] = 0x96; p[n++] = (uint8_t)(strlen(subj) + 2); p[n++] = 0xea;
	memcpy(p + n, subj, strlen(subj) + 1); n += strlen(subj) + 1;
	memcpy(p + n, "X-Carrier\0yes", 14); n += 14;	/* an application header */
	/* multipart/related; start=<s>; type=application/smil */
	p[n++] = 0x84;
	size_t lenat = n++;
	p[n++] = 0xb3;
	p[n++] = 0x8a; memcpy(p + n, "<s>", 4); n += 4;
	p[n++] = 0x89; memcpy(p + n, "application/smil", 17); n += 17;
	p[lenat] = (uint8_t)(n - lenat - 1);
	p[n++] = 0x02;		/* two parts */
	/* text/plain; charset=us-ascii; Content-Location: text.txt */
	const char *t = "see you at 5";
	uint8_t h1[] = { 0x03, 0x83, 0x81, 0x83, 0x8e, 't', 'e', 'x', 't', '.', 't', 'x', 't', 0 };
	p[n++] = sizeof(h1); p[n++] = (uint8_t)strlen(t);
	memcpy(p + n, h1, sizeof(h1)); n += sizeof(h1);
	memcpy(p + n, t, strlen(t)); n += strlen(t);
	/* image/png; name="pic.png"; Content-ID "<p1>"; 200 bytes */
	uint8_t h2[] = { 0x0a, 0xa0, 0x85, 'p', 'i', 'c', '.', 'p', 'n', 'g', 0, 0xc0, '"', '<', 'p', '1', '>', 0 };
	p[n++] = sizeof(h2); p[n++] = 0x81; p[n++] = 0x48;	/* 200, as a uintvar */
	memcpy(p + n, h2, sizeof(h2)); n += sizeof(h2);
	memset(p + n, 0x89, 200); n += 200;

	struct mms_message m;
	CHECK(mms_decode(p, n, &m));
	CHECK(m.type == MMS_RETRIEVE_CONF);
	CHECK_STR(m.message_id, "msgid-1");
	CHECK_STR(m.from, "+15550102");
	CHECK(m.n_to == 2);
	CHECK_STR(m.to[1], "+15550104");
	CHECK_STR(m.subject, "caf\xc3\xa9");
	CHECK(m.date == 0x67000001);
	CHECK(m.n_parts == 2);
	CHECK_STR(m.parts[0].content_type, "text/plain");
	CHECK_STR(m.parts[0].charset, "us-ascii");
	CHECK_STR(m.parts[0].name, "text.txt");
	CHECK(m.parts[0].len == strlen(t) && !memcmp(m.parts[0].data, t, strlen(t)));
	CHECK_STR(m.parts[1].content_type, "image/png");
	CHECK_STR(m.parts[1].name, "pic.png");
	CHECK_STR(m.parts[1].content_id, "<p1>");
	CHECK(m.parts[1].len == 200 && m.parts[1].data[199] == 0x89);
	for (size_t cut = 0; cut < n; cut++)
		mms_decode(p, cut, &m);
	FILE *f = fopen("/tmp/ztmms-test-retrieve.pdu", "wb");
	if (f) {
		fwrite(p, 1, n, f);
		fclose(f);
	}
}

static void test_small(void)
{
	uint8_t b[64];
	size_t n = mms_encode_notifyresp(b, sizeof(b), "T0001", MMS_STATUS_RETRIEVED);
	const uint8_t want[] = { 0x8c, 0x83, 0x98, 'T', '0', '0', '0', '1', 0, 0x8d, 0x92, 0x95, 0x81, 0x91, 0x81 };
	CHECK(n == sizeof(want) && !memcmp(b, want, n));
	n = mms_encode_acknowledge(b, sizeof(b), "T0001");
	CHECK(n == 13 && b[1] == 0x85);
	CHECK(mms_encode_notifyresp(b, 5, "T0001", MMS_STATUS_RETRIEVED) == 0);
	/* m-send-conf, ok, with a message id */
	const uint8_t conf[] = { 0x8c, 0x81, 0x98, 'z', 0, 0x8d, 0x92, 0x92, 0x80, 0x8b, 'i', 'd', 0 };
	struct mms_message m;
	CHECK(mms_decode(conf, sizeof(conf), &m) && m.type == MMS_SEND_CONF && m.response_status == 0x80);
	CHECK_STR(m.message_id, "id");
	/* random bytes: no crash, whatever comes back */
	srand(1);
	for (int i = 0; i < 20000; i++) {
		uint8_t r[96];
		size_t len = (size_t)(rand() % (int)sizeof(r));
		for (size_t k = 0; k < len; k++)
			r[k] = (uint8_t)rand();
		if (len && i % 2)
			r[0] = 0x8c;
		mms_decode(r, len, &m);
		const uint8_t *body;
		size_t bl;
		wsp_push_body(r, len, &body, &bl);
	}
}

int main(int argc, char **argv)
{
	fixture = argc > 1 ? argv[1] : "tests/apns-stock-fixture.xml";
	extra = argc > 2 ? argv[2] : "apns-extra.xml";
	test_apn();
	test_uintvar();
	test_push();
	test_send_req();
	test_retrieve();
	test_small();
	printf("mms-test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
