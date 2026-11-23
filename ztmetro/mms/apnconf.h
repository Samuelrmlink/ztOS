// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The access points for a SIM, from Android's apns-conf.xml (the phone's
 * stock SYSTEM has one, /run/android/system/etc/apns-conf.xml) and ours
 * beside it (APNS_EXTRA: carriers stock's file lacks, such as Mint).
 *
 * Matched as Android's ApnSettings does: the SIM's MCC and MNC, then, among
 * those, the entries with an MVNO rule the SIM meets (GID1 prefix, SPN, or an
 * IMSI pattern) win over those with none. Entries only for Wi-Fi calling
 * (bearer_bitmask 18, IWLAN) are left out.
 */
#ifndef ZT_APNCONF_H
#define ZT_APNCONF_H

#include <stdbool.h>

#define APNS_STOCK	"/run/android/system/etc/apns-conf.xml"
#define APNS_EXTRA	"/usr/local/share/ztoscorenetworking/apns-extra.xml"

struct sim_id {
	char mcc[4], mnc[4];	/* "310", "260" */
	char gid1[33];		/* hex, as the SIM gives it ("" unknown) */
	char spn[33];
	char imsi[16];		/* only to match an IMSI rule; never stored or shown */
};

struct apn_choice {
	char carrier[64];
	char apn[101], user[33], password[33], auth[8];	/* auth: none, pap, chap */
	char protocol[12];	/* IP, IPV6, IPV4V6 */
	/* for MMS: the access point (the same as apn when the data one carries
	 * MMS), the MMSC and its proxy */
	char mms_apn[101], mmsc[256], mms_proxy[128];
	int mms_port;
	char ims_apn[101];
};

/* Fills choice from the files given (NULL ends the list); false if nothing
 * matched. */
bool apn_lookup(const char *const *files, const struct sim_id *sim, struct apn_choice *choice);

/* "310260" or "310","260" and a SIM's MNC length: helper for the daemon's
 * mccmnc. mnc_digits 0 tries 3 and then 2. */
void sim_id_from_mccmnc(struct sim_id *sim, const char *mccmnc, int mnc_digits);

#endif
