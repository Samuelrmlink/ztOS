/* ztos-bt-connparam's decisions, on any machine (tests/test_adwatch.py builds
 * and runs this): what it takes, what it sends, which events settle it. */
#define CONNPARAM_NO_MAIN
#include "ztos-bt-connparam.c"
#include <assert.h>

static size_t req(uint8_t *b, uint16_t min, uint16_t max, uint16_t lat, uint16_t to) {
	static const uint8_t addr[6] = { 0xfd, 0xbb, 0x92, 0xe8, 0xe8, 0xb0 };
	memcpy(b, addr, 6);
	put16(b + 6, min); put16(b + 8, max); put16(b + 10, lat); put16(b + 12, to); put16(b + 14, 0);
	return 16;
}

int main(void) {
	uint8_t b[32];
	struct request r;
	/* Android's three: HIGH 9-12/0, BALANCED 24-40/0, LOW_POWER 80-100/2, 5 s */
	assert(connparam_request(b, req(b, 9, 12, 0, 500), &r) && connparam_valid(&r));
	assert(r.addr[0] == 0xfd && r.min == 9 && r.max == 12 && r.timeout == 500);
	assert(connparam_request(b, req(b, 24, 40, 0, 500), &r) && connparam_valid(&r));
	assert(connparam_request(b, req(b, 80, 100, 2, 500), &r) && connparam_valid(&r));
	/* out of the spec's range */
	assert(connparam_request(b, req(b, 5, 12, 0, 500), &r) && !connparam_valid(&r));	/* under 7.5 ms */
	assert(connparam_request(b, req(b, 12, 9, 0, 500), &r) && !connparam_valid(&r));	/* min > max */
	assert(connparam_request(b, req(b, 9, 3300, 0, 3200), &r) && !connparam_valid(&r));
	assert(connparam_request(b, req(b, 9, 12, 500, 500), &r) && !connparam_valid(&r));	/* latency */
	assert(connparam_request(b, req(b, 9, 12, 0, 9), &r) && !connparam_valid(&r));	/* timeout */
	assert(connparam_request(b, req(b, 80, 100, 2, 70), &r) && !connparam_valid(&r));	/* 700 ms <= 3*125*2 */
	assert(connparam_request(b, req(b, 80, 100, 2, 76), &r) && connparam_valid(&r));
	/* not a request */
	req(b, 9, 12, 0, 500);
	assert(!connparam_request(b, 15, &r));
	b[15] = 1;
	assert(!connparam_request(b, 16, &r));

	/* the one command */
	uint8_t cmd[18];
	connparam_request(b, req(b, 9, 12, 0, 500), &r);
	assert(connparam_command(cmd, 0x40, &r) == 18);
	const uint8_t want[18] = { 0x01, 0x13, 0x20, 14, 0x40, 0, 9, 0, 12, 0, 0, 0, 0xf4, 0x01, 0, 0, 0, 0 };
	assert(!memcmp(cmd, want, 18));

	/* events: accepted (wait on), refused, completed, someone else's */
	uint8_t st = 0xee;
	uint16_t got[3] = { 0, 0, 0 };
	const uint8_t accepted[] = { 0x04, 0x0f, 4, 0x00, 1, 0x13, 0x20 };
	assert(!connparam_event(accepted, sizeof(accepted), 0x40, &st, got));
	const uint8_t refused[] = { 0x04, 0x0f, 4, 0x0c, 1, 0x13, 0x20 };
	assert(connparam_event(refused, sizeof(refused), 0x40, &st, got) && st == 0x0c);
	const uint8_t other_cmd[] = { 0x04, 0x0f, 4, 0x0c, 1, 0x0d, 0x20 };
	assert(!connparam_event(other_cmd, sizeof(other_cmd), 0x40, &st, got));
	const uint8_t done[] = { 0x04, 0x3e, 10, 0x03, 0x00, 0x40, 0x00, 12, 0, 0, 0, 0xf4, 0x01 };
	assert(connparam_event(done, sizeof(done), 0x40, &st, got) && st == 0 && got[0] == 12 && got[2] == 500);
	assert(!connparam_event(done, sizeof(done), 0x41, &st, got));			/* another link */
	assert(!connparam_event(done, sizeof(done) - 1, 0x40, &st, got));		/* short */
	puts("PASS: connparam takes Android's ranges, sends one LE Connection Update, settles on its events");
	return 0;
}
