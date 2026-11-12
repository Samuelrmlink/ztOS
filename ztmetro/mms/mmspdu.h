// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MMS PDUs (OMA-TS-MMS-ENC 1.3) and the WSP pieces they are built of
 * (WAP-230-WSP): what ztmmsd needs and no more.
 *
 * Read:  the WAP push an MMS notification comes in (SMS to port 2948), the
 *        m-notification-ind inside it, m-retrieve-conf with its parts,
 *        m-send-conf.
 * Write: m-notifyresp-ind, m-acknowledge-ind, m-send-req (multipart/related
 *        with a SMIL part, as Android's MMS app sends it).
 *
 * Nothing here touches the network or the disk: host-testable
 * (tests/mms-test.c).
 */
#ifndef ZT_MMSPDU_H
#define ZT_MMSPDU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* X-Mms-Message-Type */
enum mms_type {
	MMS_SEND_REQ = 0x80, MMS_SEND_CONF = 0x81, MMS_NOTIFICATION_IND = 0x82,
	MMS_NOTIFYRESP_IND = 0x83, MMS_RETRIEVE_CONF = 0x84, MMS_ACKNOWLEDGE_IND = 0x85,
	MMS_DELIVERY_IND = 0x86,
};

/* X-Mms-Status in m-notifyresp-ind */
enum mms_status { MMS_STATUS_RETRIEVED = 0x81, MMS_STATUS_REJECTED = 0x82,
	MMS_STATUS_DEFERRED = 0x83, MMS_STATUS_UNRECOGNISED = 0x84 };

#define MMS_MAX_TO	20
#define MMS_MAX_PARTS	16

struct mms_part {
	char content_type[96];
	char charset[24];		/* "utf-8", "us-ascii", ... or "" */
	char name[96];			/* Content-Location, or the name parameter */
	char content_id[96];
	const uint8_t *data;		/* into the PDU decoded, not copied */
	size_t len;
};

struct mms_message {
	int type;			/* enum mms_type, 0 unknown */
	int version;			/* 0x90 1.0 ... 0x93 1.3 */
	char transaction_id[128];
	char message_id[128];
	char from[64];
	char to[MMS_MAX_TO][64];
	int n_to;
	char subject[160];
	char content_location[512];	/* notification: where to fetch it */
	long long expiry;		/* seconds: absolute (expiry_relative false) or from now */
	bool expiry_relative;
	long long size;			/* X-Mms-Message-Size */
	long long date;
	int message_class;		/* 0x80 personal, 0x81 ad, 0x82 info, 0x83 auto */
	int response_status;		/* m-send-conf: 0x80 ok */
	char response_text[160];
	char content_type[96];
	struct mms_part parts[MMS_MAX_PARTS];
	int n_parts;
};

/* A WSP push PDU (the SMS's user data after its header): the MMS PDU inside,
 * if its content type is application/vnd.wap.mms-message. */
bool wsp_push_body(const uint8_t *p, size_t n, const uint8_t **body, size_t *body_len);

/* Any MMS PDU's headers (and, for m-retrieve-conf, its parts). */
bool mms_decode(const uint8_t *p, size_t n, struct mms_message *m);

/* Writers: the bytes into out (size), their length back; 0 if it did not fit. */
size_t mms_encode_notifyresp(uint8_t *out, size_t size, const char *transaction_id, int status);
size_t mms_encode_acknowledge(uint8_t *out, size_t size, const char *transaction_id);

struct mms_attachment {
	const char *content_type;	/* "image/jpeg" */
	const char *name;		/* "image_0.jpg": Content-Location */
	const uint8_t *data;
	size_t len;
};

/* m-send-req to every number in to (n_to of them), with text (may be NULL or
 * "") and the attachments. A number gets "/TYPE=PLMN" here. */
size_t mms_encode_send_req(uint8_t *out, size_t size, const char *transaction_id,
	const char *const *to, int n_to, const char *subject, const char *text,
	const struct mms_attachment *att, int n_att);

/* uintvar (WSP 8.1.2), exposed for the tests */
size_t wsp_put_uintvar(uint8_t *out, uint32_t v);
bool wsp_get_uintvar(const uint8_t *p, size_t n, size_t *at, uint32_t *v);

/* The SMS side: a GSM 03.40 SMS-DELIVER (hex, the SMS centre first, as the
 * modem gives it). True if it is 8-bit data addressed to the WAP push port
 * 2948; then its user data after the header, and its concatenation
 * (reference, total, part; total 0 when it is whole). */
struct sms_wap {
	char from[40];
	unsigned ref, total, part;
	uint8_t data[160];
	size_t len;
};
bool sms_wap_push(const char *hex, struct sms_wap *w);

#endif
