// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MMS PDUs: see mmspdu.h. Field numbers and encodings from
 * OMA-TS-MMS-ENC-V1_3 (7.3, 7.4) and WAP-230-WSP (8.4, tables 38-40).
 */
#define _GNU_SOURCE
#include "mmspdu.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- reading --------------------------------------------------------------- */

bool wsp_get_uintvar(const uint8_t *p, size_t n, size_t *at, uint32_t *v)
{
	uint32_t r = 0;
	for (int i = 0; i < 5; i++) {
		if (*at >= n)
			return false;
		uint8_t b = p[(*at)++];
		r = r << 7 | (b & 0x7f);
		if (!(b & 0x80)) {
			*v = r;
			return true;
		}
	}
	return false;
}

/* Value-length: 0..30 in one octet, or 31 and a uintvar. */
static bool get_value_length(const uint8_t *p, size_t n, size_t *at, size_t *len)
{
	if (*at >= n || p[*at] > 31)
		return false;
	if (p[*at] < 31) {
		*len = p[(*at)++];
	} else {
		uint32_t v;
		(*at)++;
		if (!wsp_get_uintvar(p, n, at, &v))
			return false;
		*len = v;
	}
	return *len <= n - *at;
}

/* Text-string, quoted-string or a token: up to its NUL. */
static bool get_text(const uint8_t *p, size_t n, size_t *at, char *out, size_t size)
{
	if (*at < n && (p[*at] == 0x7f || p[*at] == '"'))
		(*at)++;
	const uint8_t *end = *at < n ? memchr(p + *at, 0, n - *at) : NULL;
	if (!end)
		return false;
	size_t len = (size_t)(end - (p + *at));
	if (out && size) {
		size_t c = len < size - 1 ? len : size - 1;
		memcpy(out, p + *at, c);
		out[c] = 0;
	}
	*at += len + 1;
	return true;
}

/* Long-integer (a length 1..30 and big-endian octets) or Short-integer. */
static bool get_integer(const uint8_t *p, size_t n, size_t *at, long long *v)
{
	if (*at >= n)
		return false;
	uint8_t b = p[*at];
	if (b & 0x80) {
		*v = b & 0x7f;
		(*at)++;
		return true;
	}
	if (b == 0 || b > 8 || b > n - *at - 1)
		return false;
	(*at)++;
	long long r = 0;
	for (unsigned i = 0; i < b; i++)
		r = r << 8 | p[(*at)++];
	*v = r;
	return true;
}

/* Any value, to step over it (WSP 8.4.1.2: its first octet says its kind). */
static bool skip_value(const uint8_t *p, size_t n, size_t *at)
{
	if (*at >= n)
		return false;
	uint8_t b = p[*at];
	if (b >= 0x80) {
		(*at)++;
		return true;
	}
	if (b <= 31) {
		size_t len;
		if (!get_value_length(p, n, at, &len))
			return false;
		*at += len;
		return true;
	}
	return get_text(p, n, at, NULL, 0);
}

static const char *charset_name(long long mib)
{
	switch (mib) {
	case 3: return "us-ascii";
	case 4: return "iso-8859-1";
	case 106: return "utf-8";
	case 1000: return "iso-10646-ucs-2";
	case 1015: return "utf-16";
	case 0x7f: case 0: return "";	/* Any-charset */
	default: return "other";
	}
}

/* Encoded-string-value: text, or a length, a charset and text. UTF-8 and
 * ASCII come through as they are; others too, marked by nothing (rare in
 * headers). */
static bool get_encoded_string(const uint8_t *p, size_t n, size_t *at, char *out, size_t size)
{
	if (*at >= n)
		return false;
	if (p[*at] > 31)
		return get_text(p, n, at, out, size);
	size_t len;
	if (!get_value_length(p, n, at, &len))
		return false;
	size_t end = *at + len;
	long long cs;
	if (!get_integer(p, end, at, &cs) || !get_text(p, end, at, out, size))
		return false;
	*at = end;
	return true;
}

/* The well-known content types (WSP table 40) that MMS uses or may meet. */
static const char *const WELL_KNOWN_TYPES[] = {
	[0x00] = "*/*", [0x01] = "text/*", [0x02] = "text/html", [0x03] = "text/plain",
	[0x06] = "text/x-vCalendar", [0x07] = "text/x-vCard", [0x08] = "text/vnd.wap.wml",
	[0x0b] = "multipart/*", [0x0c] = "multipart/mixed", [0x0f] = "multipart/alternative",
	[0x10] = "application/*", [0x1c] = "image/*", [0x1d] = "image/gif", [0x1e] = "image/jpeg",
	[0x1f] = "image/tiff", [0x20] = "image/png", [0x21] = "image/vnd.wap.wbmp",
	[0x22] = "application/vnd.wap.multipart.*", [0x23] = "application/vnd.wap.multipart.mixed",
	[0x26] = "application/vnd.wap.multipart.alternative", [0x27] = "application/xml",
	[0x28] = "text/xml", [0x33] = "application/vnd.wap.multipart.related",
	[0x3b] = "application/xhtml+xml", [0x3d] = "text/css",
	[0x3e] = "application/vnd.wap.mms-message",
};
#define N_WELL_KNOWN (sizeof(WELL_KNOWN_TYPES) / sizeof(WELL_KNOWN_TYPES[0]))

static int well_known_type(const char *type)
{
	for (size_t i = 0; i < N_WELL_KNOWN; i++)
		if (WELL_KNOWN_TYPES[i] && !strcmp(WELL_KNOWN_TYPES[i], type))
			return (int)i;
	return -1;
}

static void type_name(int code, char *out, size_t size)
{
	if (code >= 0 && (size_t)code < N_WELL_KNOWN && WELL_KNOWN_TYPES[code])
		snprintf(out, size, "%s", WELL_KNOWN_TYPES[code]);
	else
		snprintf(out, size, "application/octet-stream");
}

/* Content-type-value with its parameters; into part when given (charset,
 * name). */
static bool get_content_type(const uint8_t *p, size_t n, size_t *at, char *type, size_t size,
	struct mms_part *part)
{
	if (*at >= n)
		return false;
	uint8_t b = p[*at];
	if (b >= 0x80) {
		type_name(b & 0x7f, type, size);
		(*at)++;
		return true;
	}
	if (b > 31)
		return get_text(p, n, at, type, size);
	size_t len;
	if (!get_value_length(p, n, at, &len))
		return false;
	size_t end = *at + len;
	if (*at >= end)
		return false;
	if (p[*at] >= 0x80) {
		type_name(p[*at] & 0x7f, type, size);
		(*at)++;
	} else if (p[*at] > 31) {
		if (!get_text(p, end, at, type, size))
			return false;
	} else {
		long long code;
		if (!get_integer(p, end, at, &code))
			return false;
		type_name((int)code, type, size);
	}
	while (*at < end) {
		uint8_t k = p[*at];
		char scratch[128];
		if (k >= 0x80) {
			(*at)++;
			switch (k & 0x7f) {
			case 0x01: {	/* charset */
				long long cs;
				if (p[*at] > 31 && p[*at] < 0x80) {
					if (!get_text(p, end, at, scratch, sizeof(scratch)))
						return false;
					if (part)
						snprintf(part->charset, sizeof(part->charset), "%.23s", scratch);
				} else {
					if (!get_integer(p, end, at, &cs))
						return false;
					if (part)
						snprintf(part->charset, sizeof(part->charset), "%s", charset_name(cs));
				}
				break;
			}
			case 0x05: case 0x17: case 0x06: case 0x18:	/* name, filename */
				if (!get_text(p, end, at, scratch, sizeof(scratch)))
					return false;
				if (part && !part->name[0])
					snprintf(part->name, sizeof(part->name), "%.95s", scratch);
				break;
			default:
				if (!skip_value(p, end, at))
					return false;
			}
		} else {
			/* Untyped: a token, then an integer or text */
			if (!get_text(p, end, at, NULL, 0) || !skip_value(p, end, at))
				return false;
		}
	}
	*at = end;
	return true;
}

/* A part's headers after its content type: Content-Location and Content-ID. */
static void part_headers(const uint8_t *p, size_t end, size_t at, struct mms_part *part)
{
	while (at < end) {
		uint8_t f = p[at];
		if (f >= 0x80) {
			at++;
			if (f == 0x8e) {
				char loc[96];
				if (!get_text(p, end, &at, loc, sizeof(loc)))
					return;
				snprintf(part->name, sizeof(part->name), "%s", loc);
			} else if (f == 0xc0) {
				if (!get_text(p, end, &at, part->content_id, sizeof(part->content_id)))
					return;
			} else if (!skip_value(p, end, &at)) {
				return;
			}
		} else {
			char name[64], value[128];
			if (!get_text(p, end, &at, name, sizeof(name)) ||
			    !get_text(p, end, &at, value, sizeof(value)))
				return;
			if (!strcasecmp(name, "Content-Location"))
				snprintf(part->name, sizeof(part->name), "%.95s", value);
			else if (!strcasecmp(name, "Content-ID"))
				snprintf(part->content_id, sizeof(part->content_id), "%.95s", value);
		}
	}
}

static bool get_multipart(const uint8_t *p, size_t n, size_t at, struct mms_message *m)
{
	uint32_t count;
	if (!wsp_get_uintvar(p, n, &at, &count))
		return false;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t hlen, dlen;
		if (!wsp_get_uintvar(p, n, &at, &hlen) || !wsp_get_uintvar(p, n, &at, &dlen))
			return false;
		if (hlen > n - at || dlen > n - at - hlen)
			return false;
		size_t hend = at + hlen;
		if (m->n_parts < MMS_MAX_PARTS) {
			struct mms_part *part = &m->parts[m->n_parts];
			memset(part, 0, sizeof(*part));
			size_t h = at;
			if (!get_content_type(p, hend, &h, part->content_type, sizeof(part->content_type), part))
				return false;
			part_headers(p, hend, h, part);
			part->data = p + hend;
			part->len = dlen;
			m->n_parts++;
		}
		at = hend + dlen;
	}
	return true;
}

/* From-value: a length, then 0x80 and an address or 0x81 (insert it). */
static bool get_from(const uint8_t *p, size_t n, size_t *at, char *out, size_t size)
{
	size_t len;
	if (!get_value_length(p, n, at, &len) || !len)
		return false;
	size_t end = *at + len;
	out[0] = 0;
	if (p[*at] == 0x80) {
		(*at)++;
		if (!get_encoded_string(p, end, at, out, size))
			return false;
	}
	*at = end;
	return true;
}

/* "+15551234567/TYPE=PLMN" is a number */
static void strip_type(char *addr)
{
	char *slash = strstr(addr, "/TYPE=");
	if (slash)
		*slash = 0;
}

bool mms_decode(const uint8_t *p, size_t n, struct mms_message *m)
{
	memset(m, 0, sizeof(*m));
	size_t at = 0;
	while (at < n) {
		uint8_t f = p[at++];
		long long v;
		size_t len;
		if (f < 0x80) {
			/* an application header: a token and a text */
			at--;
			if (!get_text(p, n, &at, NULL, 0) || !get_text(p, n, &at, NULL, 0))
				return false;
			continue;
		}
		switch (f) {
		case 0x8c:
			if (at >= n)
				return false;
			m->type = p[at++];
			break;
		case 0x8d:
			if (!get_integer(p, n, &at, &v))
				return false;
			m->version = (int)v | 0x80;
			break;
		case 0x98:
			if (!get_text(p, n, &at, m->transaction_id, sizeof(m->transaction_id)))
				return false;
			break;
		case 0x8b:
			if (!get_text(p, n, &at, m->message_id, sizeof(m->message_id)))
				return false;
			break;
		case 0x83:
			if (!get_text(p, n, &at, m->content_location, sizeof(m->content_location)))
				return false;
			break;
		case 0x89:
			if (!get_from(p, n, &at, m->from, sizeof(m->from)))
				return false;
			strip_type(m->from);
			break;
		case 0x97:
			if (m->n_to < MMS_MAX_TO) {
				if (!get_encoded_string(p, n, &at, m->to[m->n_to], sizeof(m->to[0])))
					return false;
				strip_type(m->to[m->n_to++]);
			} else if (!skip_value(p, n, &at)) {
				return false;
			}
			break;
		case 0x96:
			if (!get_encoded_string(p, n, &at, m->subject, sizeof(m->subject)))
				return false;
			break;
		case 0x93:
			if (!get_encoded_string(p, n, &at, m->response_text, sizeof(m->response_text)))
				return false;
			break;
		case 0x92:
			if (at >= n)
				return false;
			m->response_status = p[at++];
			break;
		case 0x8e:
			if (!get_integer(p, n, &at, &v))
				return false;
			m->size = v;
			break;
		case 0x85:
			if (!get_integer(p, n, &at, &v))
				return false;
			m->date = v;
			break;
		case 0x8a:
			if (at < n && p[at] >= 0x80)
				m->message_class = p[at++];
			else if (!get_text(p, n, &at, NULL, 0))
				return false;
			break;
		case 0x88: {
			if (!get_value_length(p, n, &at, &len) || len < 2)
				return false;
			size_t end = at + len;
			m->expiry_relative = p[at++] == 0x81;
			if (!get_integer(p, end, &at, &v))
				return false;
			m->expiry = v;
			at = end;
			break;
		}
		case 0x84:
			if (!get_content_type(p, n, &at, m->content_type, sizeof(m->content_type), NULL))
				return false;
			/* the body follows the content type: the last header */
			if (strstr(m->content_type, "multipart"))
				return get_multipart(p, n, at, m);
			if (m->n_parts < MMS_MAX_PARTS && at < n) {
				struct mms_part *part = &m->parts[m->n_parts++];
				memset(part, 0, sizeof(*part));
				snprintf(part->content_type, sizeof(part->content_type), "%s", m->content_type);
				part->data = p + at;
				part->len = n - at;
			}
			return true;
		default:
			if (!skip_value(p, n, &at))
				return false;
		}
	}
	return m->type != 0;
}

bool wsp_push_body(const uint8_t *p, size_t n, const uint8_t **body, size_t *body_len)
{
	size_t at = 1;		/* the transaction id */
	uint32_t hlen;
	if (n < 4 || (p[at] != 0x06 && p[at] != 0x07))
		return false;
	at++;
	if (!wsp_get_uintvar(p, n, &at, &hlen) || hlen > n - at)
		return false;
	size_t h = at;
	char type[96];
	if (!get_content_type(p, at + hlen, &h, type, sizeof(type), NULL))
		return false;
	if (strcmp(type, "application/vnd.wap.mms-message"))
		return false;
	*body = p + at + hlen;
	*body_len = n - at - hlen;
	return true;
}

/* ---- writing --------------------------------------------------------------- */

struct out {
	uint8_t *p;
	size_t size, len;
	bool full;
};

static void put(struct out *o, const void *data, size_t len)
{
	if (o->full || len > o->size - o->len) {
		o->full = true;
		return;
	}
	memcpy(o->p + o->len, data, len);
	o->len += len;
}

static void put_byte(struct out *o, uint8_t b)
{
	put(o, &b, 1);
}

static void put_text(struct out *o, const char *s)
{
	if ((uint8_t)s[0] >= 0x80)
		put_byte(o, 0x7f);	/* a Quote, so it is not taken for a short integer */
	put(o, s, strlen(s) + 1);
}

size_t wsp_put_uintvar(uint8_t *out, uint32_t v)
{
	uint8_t tmp[5];
	size_t n = 0;
	do {
		tmp[n++] = v & 0x7f;
		v >>= 7;
	} while (v);
	for (size_t i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i] | (i + 1 < n ? 0x80 : 0);
	return n;
}

static void put_uintvar(struct out *o, uint32_t v)
{
	uint8_t tmp[5];
	put(o, tmp, wsp_put_uintvar(tmp, v));
}

static void put_value_length(struct out *o, size_t len)
{
	if (len < 31) {
		put_byte(o, (uint8_t)len);
	} else {
		put_byte(o, 31);
		put_uintvar(o, (uint32_t)len);
	}
}

/* A value built apart, then written after its length. */
static void put_with_length(struct out *o, const struct out *value)
{
	put_value_length(o, value->len);
	put(o, value->p, value->len);
	o->full |= value->full;
}

static bool ascii(const char *s)
{
	for (; *s; s++)
		if ((uint8_t)*s >= 0x80)
			return false;
	return true;
}

/* Encoded-string-value: plain text if ASCII, else marked UTF-8. */
static void put_encoded_string(struct out *o, const char *s)
{
	if (ascii(s)) {
		put_text(o, s);
		return;
	}
	uint8_t tmp[512];
	struct out v = { tmp, sizeof(tmp), 0, false };
	put_byte(&v, 0xea);	/* UTF-8, MIBenum 106 */
	put_text(&v, s);
	put_with_length(o, &v);
}

static void put_head(struct out *o, int type, const char *tid)
{
	put_byte(o, 0x8c);
	put_byte(o, (uint8_t)type);
	put_byte(o, 0x98);
	put_text(o, tid);
	put_byte(o, 0x8d);
	put_byte(o, 0x92);	/* 1.2 */
}

size_t mms_encode_notifyresp(uint8_t *buf, size_t size, const char *tid, int status)
{
	struct out o = { buf, size, 0, false };
	put_head(&o, MMS_NOTIFYRESP_IND, tid);
	put_byte(&o, 0x95);
	put_byte(&o, (uint8_t)status);
	put_byte(&o, 0x91);	/* Report-Allowed: no */
	put_byte(&o, 0x81);
	return o.full ? 0 : o.len;
}

size_t mms_encode_acknowledge(uint8_t *buf, size_t size, const char *tid)
{
	struct out o = { buf, size, 0, false };
	put_head(&o, MMS_ACKNOWLEDGE_IND, tid);
	put_byte(&o, 0x91);
	put_byte(&o, 0x81);
	return o.full ? 0 : o.len;
}

/* One part: its headers (content type, Content-ID, Content-Location) and data. */
static void put_part(struct out *o, const char *type, bool utf8, const char *name, const char *cid,
	const uint8_t *data, size_t len)
{
	uint8_t hb[512], tb[256];
	struct out h = { hb, sizeof(hb), 0, false }, t = { tb, sizeof(tb), 0, false };
	int code = well_known_type(type);
	if (code >= 0)
		put_byte(&t, (uint8_t)(0x80 | code));
	else
		put_text(&t, type);
	if (utf8) {
		put_byte(&t, 0x81);
		put_byte(&t, 0xea);
	}
	put_byte(&t, 0x85);	/* name */
	put_text(&t, name);
	put_with_length(&h, &t);
	char quoted[128];
	snprintf(quoted, sizeof(quoted), "<%s>", cid);
	put_byte(&h, 0xc0);	/* Content-ID: a quoted string */
	put_byte(&h, '"');
	put(&h, quoted, strlen(quoted) + 1);
	put_byte(&h, 0x8e);	/* Content-Location */
	put_text(&h, name);
	put_uintvar(o, (uint32_t)h.len);
	put_uintvar(o, (uint32_t)len);
	put(o, h.p, h.len);
	put(o, data, len);
	o->full |= h.full;
}

static const char *smil_tag(const char *type)
{
	if (!strncmp(type, "image/", 6))
		return "img";
	if (!strncmp(type, "video/", 6))
		return "video";
	if (!strncmp(type, "audio/", 6))
		return "audio";
	return "ref";
}

size_t mms_encode_send_req(uint8_t *buf, size_t size, const char *tid, const char *const *to, int n_to,
	const char *subject, const char *text, const struct mms_attachment *att, int n_att)
{
	struct out o = { buf, size, 0, false };
	bool has_text = text && text[0];
	put_head(&o, MMS_SEND_REQ, tid);
	put_byte(&o, 0x89);	/* From: insert-address, the MMSC fills it in */
	put_byte(&o, 0x01);
	put_byte(&o, 0x81);
	for (int i = 0; i < n_to; i++) {
		char addr[80];
		snprintf(addr, sizeof(addr), "%s/TYPE=PLMN", to[i]);
		put_byte(&o, 0x97);
		put_encoded_string(&o, addr);
	}
	if (subject && subject[0]) {
		put_byte(&o, 0x96);
		put_encoded_string(&o, subject);
	}
	put_byte(&o, 0x8a);	/* Message-Class: personal */
	put_byte(&o, 0x80);
	put_byte(&o, 0x86);	/* Delivery-Report: no */
	put_byte(&o, 0x81);
	put_byte(&o, 0x90);	/* Read-Report: no */
	put_byte(&o, 0x81);

	/* Content-Type: multipart/related; type="application/smil"; start="<smil>" */
	uint8_t cb[64];
	struct out c = { cb, sizeof(cb), 0, false };
	put_byte(&c, 0xb3);
	put_byte(&c, 0x89);
	put_text(&c, "application/smil");
	put_byte(&c, 0x8a);
	put_text(&c, "<smil>");
	put_byte(&o, 0x84);
	put_with_length(&o, &c);

	/* The SMIL: each attachment in turn, the text under the last */
	char smil[2048];
	size_t s = (size_t)snprintf(smil, sizeof(smil),
		"<smil><head><layout><root-layout/>"
		"<region id=\"Image\" top=\"0\" left=\"0\" height=\"80%%\" width=\"100%%\" fit=\"meet\"/>"
		"<region id=\"Text\" top=\"80%%\" left=\"0\" height=\"20%%\" width=\"100%%\" fit=\"scroll\"/>"
		"</layout></head><body>");
	int pars = n_att ? n_att : 1;
	for (int i = 0; i < pars && s < sizeof(smil); i++) {
		s += (size_t)snprintf(smil + s, sizeof(smil) - s, "<par dur=\"5000ms\">");
		if (i < n_att)
			s += (size_t)snprintf(smil + s, sizeof(smil) - s, "<%s src=\"%s\" region=\"Image\"/>",
				smil_tag(att[i].content_type), att[i].name);
		if (has_text && i == pars - 1)
			s += (size_t)snprintf(smil + s, sizeof(smil) - s, "<text src=\"text_0.txt\" region=\"Text\"/>");
		s += (size_t)snprintf(smil + s, sizeof(smil) - s, "</par>");
	}
	if (s < sizeof(smil))
		s += (size_t)snprintf(smil + s, sizeof(smil) - s, "</body></smil>");
	if (s >= sizeof(smil))
		return 0;

	put_uintvar(&o, (uint32_t)(1 + n_att + (has_text ? 1 : 0)));
	put_part(&o, "application/smil", false, "smil.xml", "smil", (const uint8_t *)smil, s);
	for (int i = 0; i < n_att; i++) {
		char cid[100];
		snprintf(cid, sizeof(cid), "%s", att[i].name);
		char *dot = strrchr(cid, '.');
		if (dot)
			*dot = 0;
		put_part(&o, att[i].content_type, false, att[i].name, cid, att[i].data, att[i].len);
	}
	if (has_text)
		put_part(&o, "text/plain", true, "text_0.txt", "text_0", (const uint8_t *)text, strlen(text));
	return o.full ? 0 : o.len;
}

/* ---- the SMS that carries the push ----------------------------------------- */

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
		c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool sms_wap_push(const char *hex, struct sms_wap *w)
{
	uint8_t b[200];
	size_t n = 0, hl = strlen(hex);
	if (hl % 2 || hl / 2 > sizeof(b))
		return false;
	for (size_t i = 0; i < hl; i += 2) {
		int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		b[n++] = (uint8_t)(hi << 4 | lo);
	}
	memset(w, 0, sizeof(*w));
	size_t at = 0;
	if (n < 1)
		return false;
	at = 1 + (size_t)b[0];		/* the SMS centre */
	if (at + 3 > n || (b[at] & 3) != 0 || !(b[at] & 0x40))
		return false;		/* a delivery, with a user data header */
	at++;
	size_t digits = b[at++], type = b[at++], bytes = (digits + 1) / 2;
	if (at + bytes + 10 > n)
		return false;
	size_t o = 0;
	if ((type & 0x70) == 0x10)
		w->from[o++] = '+';
	if ((type & 0x70) != 0x50) {
		for (size_t i = 0; i < digits && o + 1 < sizeof(w->from); i++) {
			int d = i % 2 ? b[at + i / 2] >> 4 : b[at + i / 2] & 0xf;
			if (d <= 9)
				w->from[o++] = (char)('0' + d);
		}
	}
	w->from[o] = 0;
	at += bytes;
	uint8_t dcs = b[at + 1];
	at += 2 + 7;			/* protocol, coding, time stamp */
	bool eight_bit = (dcs & 0xc0) == 0 ? (dcs & 0x0c) == 0x04 : (dcs & 0xf0) == 0xf0 && (dcs & 0x04);
	if (!eight_bit)
		return false;
	size_t udl = b[at++];
	if (udl > n - at || udl < 1)
		return false;
	size_t udhl = b[at];
	if (udhl + 1 > udl)
		return false;
	bool port = false;
	for (size_t i = at + 1; i + 2 <= at + 1 + udhl;) {
		unsigned tag = b[i++], len = b[i++];
		if (i + len > at + 1 + udhl)
			return false;
		if (tag == 0x05 && len == 4)
			port = (b[i] << 8 | b[i + 1]) == 2948;
		else if (tag == 0x04 && len == 2)
			port = false;	/* 8-bit ports: not WAP push */
		else if (tag == 0x00 && len == 3) {
			w->ref = b[i]; w->total = b[i + 1]; w->part = b[i + 2];
		} else if (tag == 0x08 && len == 4) {
			w->ref = (unsigned)b[i] << 8 | b[i + 1]; w->total = b[i + 2]; w->part = b[i + 3];
		}
		i += len;
	}
	if (!port)
		return false;
	if (w->total && (!w->part || w->part > w->total))
		return false;
	w->len = udl - udhl - 1;
	memcpy(w->data, b + at + 1 + udhl, w->len);
	return true;
}
