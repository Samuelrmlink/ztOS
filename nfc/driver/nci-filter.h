/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The owner-approved NCI allowlist for stage 2 (card reading), 2026-11-12.
 * Pure C with no kernel dependencies, so the same code is host-tested
 * (test-nci-filter.c) and compiled into zrn-nfc.
 *
 * Control commands allowed (anything else is refused):
 *   CORE_RESET 20 00 (keep config)        CORE_INIT 20 01
 *   CORE_SET_CONFIG 20 02: only TOTAL_DURATION (0x00) = 500 ms and
 *                          PA_BAIL_OUT (0x08) = 1
 *   RF_DISCOVER_MAP 21 00: only ISO-DEP (0x04), poll (0x01) -> ISO-DEP if (0x02)
 *   RF_DISCOVER 21 03: only poll modes NFC-A/B/F/V (0x00 0x01 0x02 0x06)
 *   RF_DISCOVER_SELECT 21 04: protocols T2T/T3T/ISO-DEP only (never 0x81 MFC)
 *   RF_DEACTIVATE 21 06: idle (0x00) only
 * Data on the static RF connection (conn 0), by the activated protocol:
 *   T2T: READ (30 nn), GET_VERSION (60)
 *   T3T: Polling (cmd 0x00), Read Without Encryption (cmd 0x06)
 *   ISO-DEP: SELECT by name of the NDEF app or the PPSE; DESFire GetVersion
 *            (90 60 00 00 00) and its additional frames (90 AF 00 00 00)
 * Never: NFCEE (GID 2), proprietary (GID 0xF), listen modes, writes.
 */
#ifndef ZRN_NCI_FILTER_H
#define ZRN_NCI_FILTER_H

#define NCI_PROTO_T2T		0x02
#define NCI_PROTO_T3T		0x03
#define NCI_PROTO_ISO_DEP	0x04
#define NCI_PROTO_NONE		0x00

static const unsigned char zrn_aid_ndef[] = { 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01 };
static const unsigned char zrn_aid_ppse[] = "2PAY.SYS.DDF01";	/* 14 bytes */

static inline int zrn_memeq(const unsigned char *a, const unsigned char *b, unsigned int n)
{
	while (n--)
		if (*a++ != *b++)
			return 0;
	return 1;
}

/* returns NULL if allowed, else a short reason */
static inline const char *zrn_nci_check(const unsigned char *p, unsigned int len,
					unsigned char proto)
{
	unsigned int plen, i, n;
	const unsigned char *q;

	if (len < 3)
		return "short";
	plen = p[2];
	if (len != 3 + plen)
		return "length mismatch";
	if (p[0] & 0x10)
		return "segmented (PBF) not allowed";
	q = p + 3;

	switch (p[0] >> 5) {
	case 1:	/* control command */
		break;
	case 0:	/* data */
		if ((p[0] & 0x0f) != 0)
			return "data on a connection other than 0";
		switch (proto) {
		case NCI_PROTO_T2T:
			if (plen == 2 && q[0] == 0x30)
				return 0;
			if (plen == 1 && q[0] == 0x60)
				return 0;
			return "T2T: only READ / GET_VERSION";
		case NCI_PROTO_T3T:
			if (plen < 2 || q[0] != plen)
				return "T3T: bad length byte";
			if (q[1] == 0x00 && plen == 6)
				return 0;	/* Polling */
			if (q[1] == 0x06 && plen >= 14)
				return 0;	/* Read Without Encryption */
			return "T3T: only Polling / Read Without Encryption";
		case NCI_PROTO_ISO_DEP:
			if (plen == 5 && q[0] == 0x90 && (q[1] == 0x60 || q[1] == 0xAF) &&
			    q[2] == 0 && q[3] == 0 && q[4] == 0)
				return 0;	/* DESFire GetVersion (+ additional frames) */
			if (plen >= 5 && q[0] == 0x00 && q[1] == 0xA4 && q[2] == 0x04 &&
			    q[3] == 0x00) {
				n = q[4];
				if (plen != 5 + n && plen != 6 + n)
					return "SELECT: bad Lc";
				if (n == sizeof(zrn_aid_ndef) && zrn_memeq(q + 5, zrn_aid_ndef, n))
					return 0;
				if (n == 14 && zrn_memeq(q + 5, zrn_aid_ppse, 14))
					return 0;
				return "SELECT: AID not allowed";
			}
			return "ISO-DEP: APDU not allowed";
		default:
			return "data: no allowed protocol activated";
		}
	default:
		return "only commands and data may be sent";
	}

	/* control commands: GID p[0] & 0x0f, OID p[1] & 0x3f */
	switch (((p[0] & 0x0f) << 8) | (p[1] & 0x3f)) {
	case 0x000:	/* CORE_RESET */
		return (plen == 1 && q[0] == 0x00) ? 0 : "CORE_RESET: keep-config only";
	case 0x001:	/* CORE_INIT (NCI 1.0) */
		return plen == 0 ? 0 : "CORE_INIT: NCI 1.0 form only";
	case 0x002:	/* CORE_SET_CONFIG */
		if (plen < 1)
			return "SET_CONFIG: empty";
		n = q[0];
		for (i = 1; n--; ) {
			if (i + 2 > plen)
				return "SET_CONFIG: truncated";
			if (q[i] == 0x00 && q[i + 1] == 2 && i + 4 <= plen &&
			    q[i + 2] == 0xF4 && q[i + 3] == 0x01)
				i += 4;			/* TOTAL_DURATION 500 ms */
			else if (q[i] == 0x08 && q[i + 1] == 1 && i + 3 <= plen &&
				 q[i + 2] == 0x01)
				i += 3;			/* PA_BAIL_OUT 1 */
			else
				return "SET_CONFIG: parameter not allowed";
		}
		return i == plen ? 0 : "SET_CONFIG: trailing bytes";
	case 0x100:	/* RF_DISCOVER_MAP */
		if (plen == 4 && q[0] == 1 && q[1] == NCI_PROTO_ISO_DEP && q[2] == 0x01 &&
		    q[3] == 0x02)
			return 0;
		return "DISCOVER_MAP: only ISO-DEP poll -> ISO-DEP";
	case 0x103:	/* RF_DISCOVER */
		if (plen < 1 || plen != 1 + 2u * q[0] || q[0] == 0)
			return "DISCOVER: bad length";
		for (i = 0; i < q[0]; i++) {
			unsigned char m = q[1 + 2 * i];

			if (m != 0x00 && m != 0x01 && m != 0x02 && m != 0x06)
				return "DISCOVER: only poll NFC-A/B/F/V";
		}
		return 0;
	case 0x104:	/* RF_DISCOVER_SELECT */
		if (plen != 3)
			return "SELECT: bad length";
		if (q[1] != NCI_PROTO_T2T && q[1] != NCI_PROTO_T3T && q[1] != NCI_PROTO_ISO_DEP)
			return "SELECT: protocol not allowed";
		if (q[2] != 0x01 && q[2] != 0x02)
			return "SELECT: interface not allowed";
		if (q[2] == 0x02 && q[1] != NCI_PROTO_ISO_DEP)
			return "SELECT: ISO-DEP interface only for ISO-DEP";
		return 0;
	case 0x106:	/* RF_DEACTIVATE */
		return (plen == 1 && q[0] == 0x00) ? 0 : "DEACTIVATE: idle only";
	}
	return "command not allowed";
}

/* is this an accepted RF_DISCOVER / RF_DEACTIVATE command? */
static inline int zrn_nci_is_discover(const unsigned char *p)
{
	return p[0] == 0x21 && (p[1] & 0x3f) == 0x03;
}

static inline int zrn_nci_is_deactivate(const unsigned char *p)
{
	return p[0] == 0x21 && (p[1] & 0x3f) == 0x06;
}

#endif
