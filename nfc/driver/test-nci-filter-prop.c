// SPDX-License-Identifier: GPL-2.0
/*
 * Sweep check that exactly one proprietary (GID 0x0F) packet passes the
 * allowlist: 2F 28 03 00 12 00 (FW_CFG). cc -O2 test-nci-filter-prop.c
 *   A. 2F 28, length 3: every one of the 2^24 payloads (exhaustive).
 *   B. every GID-0xF first byte (any MT/PBF) x every OID x lengths 0..2,
 *      each with every payload (exhaustive for those lengths).
 *   C. every GID-0xF first byte x every OID, length 3, payload 00 12 00.
 *   D. 2F 28 lengths 4..8 with the approved prefix plus padding.
 */
#include <stdio.h>
#include "nci-filter.h"

static unsigned long tried, passed;

static void check(const unsigned char *p, unsigned int n)
{
	unsigned int i;

	tried++;
	if (!zrn_nci_check(p, n, 0)) {
		passed++;
		printf("ALLOWED:");
		for (i = 0; i < n; i++)
			printf(" %02x", p[i]);
		printf("\n");
	}
}

int main(void)
{
	unsigned char p[16];
	unsigned int b0, oid, len, v;

	/* A */
	p[0] = 0x2F; p[1] = 0x28; p[2] = 3;
	for (v = 0; v < (1u << 24); v++) {
		p[3] = v & 0xff; p[4] = (v >> 8) & 0xff; p[5] = (v >> 16) & 0xff;
		check(p, 6);
	}
	/* B */
	for (b0 = 0x0F; b0 <= 0xFF; b0 += 0x10)
		for (oid = 0; oid < 256; oid++)
			for (len = 0; len <= 2; len++)
				for (v = 0; v < (len ? 1u << (8 * len) : 1u); v++) {
					p[0] = b0; p[1] = oid; p[2] = len;
					p[3] = v & 0xff; p[4] = (v >> 8) & 0xff;
					check(p, 3 + len);
				}
	/* C */
	for (b0 = 0x0F; b0 <= 0xFF; b0 += 0x10)
		for (oid = 0; oid < 256; oid++) {
			if (b0 == 0x2F && oid == 0x28)
				continue;	/* covered by A */
			p[0] = b0; p[1] = oid; p[2] = 3;
			p[3] = 0x00; p[4] = 0x12; p[5] = 0x00;
			check(p, 6);
		}
	/* D */
	for (len = 4; len <= 8; len++) {
		unsigned int i;

		p[0] = 0x2F; p[1] = 0x28; p[2] = len;
		p[3] = 0x00; p[4] = 0x12; p[5] = 0x00;
		for (i = 3; i < len; i++)
			p[3 + i] = 0;
		check(p, 3 + len);
	}
	printf("%lu GID-0xF packets tried, %lu allowed\n", tried, passed);
	return passed == 1 ? 0 : 1;
}
