/* ztos-bt-adwatch's one decision, on any machine: which management events
 * go out, and as what (tests/test_adwatch.py builds and runs this). */
#define ADWATCH_NO_MAIN
#include "ztos-bt-adwatch.c"
#include <assert.h>

static size_t event(uint8_t *buf, uint16_t op, uint16_t index, const uint8_t *params, uint16_t len) {
	buf[0] = (uint8_t)op; buf[1] = (uint8_t)(op >> 8);
	buf[2] = (uint8_t)index; buf[3] = (uint8_t)(index >> 8);
	buf[4] = (uint8_t)len; buf[5] = (uint8_t)(len >> 8);
	memcpy(buf + 6, params, len);
	return (size_t)len + 6;
}

int main(void) {
	uint8_t found[14 + 5] = { 1, 2, 3, 4, 5, 6, 2, (uint8_t)-60, 0, 0, 0, 0, 5, 0, 4, 9, 'R', 'M', '_' };
	uint8_t buf[64], out[64];
	size_t n = event(buf, 0x0012, 1, found, sizeof(found));
	size_t m = adwatch_packet(buf, n, out, sizeof(out));
	assert(m == sizeof(found) + 2 && out[0] == 1 && out[1] == 0 && !memcmp(out + 2, found, sizeof(found)));
	/* anything else is not passed on */
	assert(!adwatch_packet(buf, n - 1, out, sizeof(out)));			/* short */
	n = event(buf, 0x0013, 0, found, sizeof(found));			/* Device Connected */
	assert(!adwatch_packet(buf, n, out, sizeof(out)));
	found[12] = 6;								/* data length wrong */
	n = event(buf, 0x0012, 0, found, sizeof(found));
	assert(!adwatch_packet(buf, n, out, sizeof(out)));
	found[12] = 5;
	n = event(buf, 0x0012, 0, found, 10);					/* under the fixed part */
	assert(!adwatch_packet(buf, n, out, sizeof(out)));
	n = event(buf, 0x0012, 0, found, sizeof(found));
	assert(!adwatch_packet(buf, n, out, 8));				/* no room */
	assert(!adwatch_packet(buf, 3, out, sizeof(out)));
	puts("PASS: adwatch passes on Device Found, whole, and nothing else");
	return 0;
}
