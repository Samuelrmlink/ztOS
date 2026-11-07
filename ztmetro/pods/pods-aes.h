// SPDX-License-Identifier: GPL-3.0-or-later
/* AES-128, one block at a time (FIPS-197): the two things AirPods' broadcasts need
 * and no more, so that the protocol layer has no library under it and its tests run
 * anywhere. This protects nothing of ours: it reads what the pods encrypt. */
#ifndef PODS_AES_H
#define PODS_AES_H

#include <stdint.h>
#include <string.h>

static inline uint8_t pods_aes_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static inline uint8_t pods_aes_mul(uint8_t a, uint8_t b) {
	uint8_t r = 0;
	for (; b; b >>= 1, a = pods_aes_xt(a)) {
		if (b & 1) r ^= a;
	}
	return r;
}

/* the S-box from its definition: inverse in GF(2^8), then the affine step */
static inline void pods_aes_tables(uint8_t sbox[256], uint8_t inv[256]) {
	uint8_t p = 1, q = 1;
	do {
		p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));	/* times 3 */
		q ^= (uint8_t)(q << 1);						/* divided by 3 */
		q ^= (uint8_t)(q << 2);
		q ^= (uint8_t)(q << 4);
		if (q & 0x80) q ^= 0x09;
		uint8_t x = (uint8_t)(q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^
			(uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4));
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
	for (int i = 0; i < 256; i++) inv[sbox[i]] = (uint8_t)i;
}

static inline void pods_aes_expand(const uint8_t key[16], const uint8_t sbox[256], uint8_t rk[176]) {
	memcpy(rk, key, 16);
	uint8_t rcon = 1;
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4];
		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t first = t[0];
			t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[first];
			rcon = pods_aes_xt(rcon);
		}
		for (int j = 0; j < 4; j++) rk[i + j] = rk[i + j - 16] ^ t[j];
	}
}

static inline void pods_aes_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
	uint8_t sbox[256], inv[256], rk[176], s[16], t[16];
	pods_aes_tables(sbox, inv);
	pods_aes_expand(key, sbox, rk);
	for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
	for (int round = 1; round <= 10; round++) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
		}
		if (round < 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *a = t + c * 4, b[4];
				for (int r = 0; r < 4; r++) {
					b[r] = (uint8_t)(pods_aes_xt(a[r]) ^ pods_aes_xt(a[(r + 1) % 4]) ^ a[(r + 1) % 4] ^
						a[(r + 2) % 4] ^ a[(r + 3) % 4]);
				}
				memcpy(a, b, 4);
			}
		}
		for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
	}
	memcpy(out, s, 16);
}

static inline void pods_aes_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
	uint8_t sbox[256], inv[256], rk[176], s[16], t[16];
	pods_aes_tables(sbox, inv);
	pods_aes_expand(key, sbox, rk);
	for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
	for (int round = 9; round >= 0; round--) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = inv[s[c * 4 + r]];
		}
		for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
		if (round > 0) {
			for (int c = 0; c < 4; c++) {
				uint8_t *a = s + c * 4, b[4];
				for (int r = 0; r < 4; r++) {
					b[r] = (uint8_t)(pods_aes_mul(a[r], 14) ^ pods_aes_mul(a[(r + 1) % 4], 11) ^
						pods_aes_mul(a[(r + 2) % 4], 13) ^ pods_aes_mul(a[(r + 3) % 4], 9));
				}
				memcpy(a, b, 4);
			}
		}
	}
	memcpy(out, s, 16);
}

#endif
