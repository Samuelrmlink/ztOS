// SPDX-License-Identifier: GPL-2.0-or-later
/* The access points for a SIM, from apns-conf.xml: see apnconf.h. */
#define _GNU_SOURCE
#include "apnconf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct entry {
	char carrier[64], mcc[4], mnc[4], apn[101], user[33], password[33], authtype[4];
	char type[96], protocol[12], mmsc[256], mmsproxy[128], mmsport[8];
	char mvno_type[8], mvno_match_data[40], bearer_bitmask[24];
};

void sim_id_from_mccmnc(struct sim_id *sim, const char *mccmnc, int mnc_digits)
{
	snprintf(sim->mcc, sizeof(sim->mcc), "%.3s", mccmnc);
	size_t rest = strlen(mccmnc) > 3 ? strlen(mccmnc) - 3 : 0;
	int d = mnc_digits ? mnc_digits : rest >= 3 ? 3 : 2;
	snprintf(sim->mnc, sizeof(sim->mnc), "%.*s", d, rest ? mccmnc + 3 : "");
}

/* name="value" inside one element; XML's five entities undone */
static void attr(const char *el, const char *name, char *out, size_t size)
{
	out[0] = 0;
	size_t nl = strlen(name);
	for (const char *p = el; (p = strstr(p, name)); p += nl) {
		if (p > el && !isspace((unsigned char)p[-1]))
			continue;
		const char *q = p + nl;
		while (isspace((unsigned char)*q))
			q++;
		if (*q++ != '=')
			continue;
		while (isspace((unsigned char)*q))
			q++;
		char quote = *q++;
		if (quote != '"' && quote != '\'')
			continue;
		size_t o = 0;
		for (; *q && *q != quote && o + 1 < size; q++) {
			static const struct { const char *e; char c; } ents[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
			bool done = false;
			for (size_t i = 0; *q == '&' && i < 5; i++) {
				size_t el2 = strlen(ents[i].e);
				if (!strncmp(q, ents[i].e, el2)) {
					out[o++] = ents[i].c;
					q += el2 - 1;
					done = true;
					break;
				}
			}
			if (!done)
				out[o++] = *q;
		}
		out[o] = 0;
		return;
	}
}

static void entry_read(const char *el, struct entry *e)
{
#define A(field, name) attr(el, name, e->field, sizeof(e->field))
	A(carrier, "carrier"); A(mcc, "mcc"); A(mnc, "mnc"); A(apn, "apn"); A(user, "user");
	A(password, "password"); A(authtype, "authtype"); A(type, "type"); A(protocol, "protocol");
	A(mmsc, "mmsc"); A(mmsproxy, "mmsproxy"); A(mmsport, "mmsport"); A(mvno_type, "mvno_type");
	A(mvno_match_data, "mvno_match_data"); A(bearer_bitmask, "bearer_bitmask");
#undef A
}

/* Does the SIM meet the entry's MVNO rule? */
static bool mvno_match(const struct entry *e, const struct sim_id *sim)
{
	const char *t = e->mvno_type, *d = e->mvno_match_data;
	if (!strcasecmp(t, "gid"))
		return sim->gid1[0] && d[0] && !strncasecmp(sim->gid1, d, strlen(d));
	if (!strcasecmp(t, "spn"))
		return sim->spn[0] && !strcasecmp(sim->spn, d);
	if (!strcasecmp(t, "imsi")) {
		size_t n = strlen(d);
		if (!n || strlen(sim->imsi) < n)
			return false;
		for (size_t i = 0; i < n; i++)
			if (tolower((unsigned char)d[i]) != 'x' && d[i] != sim->imsi[i])
				return false;
		return true;
	}
	return false;
}

static bool has_type(const struct entry *e, const char *want)
{
	if (!e->type[0])
		return !strcmp(want, "default") || !strcmp(want, "mms");	/* no type: all */
	char types[96];
	snprintf(types, sizeof(types), "%s", e->type);
	for (char *save = NULL, *t = strtok_r(types, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		while (isspace((unsigned char)*t))
			t++;
		if (!strcasecmp(t, want) || !strcmp(t, "*"))
			return true;
	}
	return false;
}

/* Only over Wi-Fi (IWLAN, radio technology 18): not for the modem's PDNs. */
static bool iwlan_only(const struct entry *e)
{
	return !strcmp(e->bearer_bitmask, "18");
}

#define MAX_ENTRIES 64

/* The entries for this MCC and MNC in one file. */
static int read_file(const char *path, const struct sim_id *sim, const char *mnc, struct entry *out, int max)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	if (size <= 0 || size > 16 << 20) {
		fclose(f);
		return 0;
	}
	char *buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return 0;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = 0;
	int n = 0;
	char *p = buf;
	while (n < max && (p = strstr(p, "<"))) {
		if (!strncmp(p, "<!--", 4)) {
			char *end = strstr(p, "-->");
			if (!end)
				break;
			p = end + 3;
			continue;
		}
		if (strncmp(p, "<apn", 4) || !isspace((unsigned char)p[4])) {
			p++;
			continue;
		}
		char *end = strchr(p, '>');
		if (!end)
			break;
		*end = 0;
		struct entry e;
		memset(&e, 0, sizeof(e));
		entry_read(p, &e);
		if (!strcmp(e.mcc, sim->mcc) && !strcmp(e.mnc, mnc))
			out[n++] = e;
		p = end + 1;
	}
	free(buf);
	return n;
}

static void choose(const struct entry *list, int n, const struct sim_id *sim, struct apn_choice *c)
{
	/* the MVNO's own, if the SIM is one's; else the network's plain ones */
	bool mvno = false;
	for (int i = 0; i < n; i++)
		if (list[i].mvno_type[0] && mvno_match(&list[i], sim))
			mvno = true;
	const struct entry *data = NULL, *mms = NULL, *ims = NULL;
	for (int i = 0; i < n; i++) {
		const struct entry *e = &list[i];
		if (mvno ? !(e->mvno_type[0] && mvno_match(e, sim)) : e->mvno_type[0] != 0)
			continue;
		if (iwlan_only(e))
			continue;
		if (!data && has_type(e, "default"))
			data = e;
		if (has_type(e, "mms") && e->mmsc[0] && (!mms || e == data))
			mms = e;
		if (!ims && has_type(e, "ims") && strcmp(e->type, "*"))
			ims = e;
	}
	/* the data access point's own MMS, when it carries MMS too */
	if (data && has_type(data, "mms") && data->mmsc[0])
		mms = data;
	memset(c, 0, sizeof(*c));
	if (data) {
		snprintf(c->carrier, sizeof(c->carrier), "%s", data->carrier);
		snprintf(c->apn, sizeof(c->apn), "%s", data->apn);
		snprintf(c->user, sizeof(c->user), "%s", data->user);
		snprintf(c->password, sizeof(c->password), "%s", data->password);
		snprintf(c->protocol, sizeof(c->protocol), "%s", data->protocol[0] ? data->protocol : "IP");
		int auth = data->authtype[0] ? atoi(data->authtype) : (data->user[0] ? 3 : 0);
		snprintf(c->auth, sizeof(c->auth), "%s", auth == 1 ? "pap" : auth >= 2 ? "chap" : "none");
	}
	if (mms) {
		if (!c->carrier[0])
			snprintf(c->carrier, sizeof(c->carrier), "%s", mms->carrier);
		snprintf(c->mms_apn, sizeof(c->mms_apn), "%s", mms->apn);
		snprintf(c->mmsc, sizeof(c->mmsc), "%s", mms->mmsc);
		snprintf(c->mms_proxy, sizeof(c->mms_proxy), "%s", mms->mmsproxy);
		c->mms_port = atoi(mms->mmsport);
	}
	if (ims)
		snprintf(c->ims_apn, sizeof(c->ims_apn), "%s", ims->apn);
}

bool apn_lookup(const char *const *files, const struct sim_id *sim, struct apn_choice *choice)
{
	struct entry list[MAX_ENTRIES];
	memset(choice, 0, sizeof(*choice));
	/* the MNC as given, then its first two digits (an MNC of two) */
	char mncs[2][4];
	snprintf(mncs[0], sizeof(mncs[0]), "%s", sim->mnc);
	snprintf(mncs[1], sizeof(mncs[1]), "%.2s", sim->mnc);
	for (int m = 0; m < 2; m++) {
		if (m == 1 && !strcmp(mncs[0], mncs[1]))
			break;
		/* each file in turn: the first with anything for this SIM decides,
		 * but an MVNO match in a later file beats a plain match earlier */
		struct apn_choice plain;
		bool have_plain = false;
		for (int f = 0; files[f]; f++) {
			int n = read_file(files[f], sim, mncs[m], list, MAX_ENTRIES);
			if (!n)
				continue;
			bool mvno = false;
			for (int i = 0; i < n; i++)
				if (list[i].mvno_type[0] && mvno_match(&list[i], sim))
					mvno = true;
			struct apn_choice c;
			choose(list, n, sim, &c);
			if (!c.apn[0] && !c.mmsc[0])
				continue;
			if (mvno) {
				*choice = c;
				return true;
			}
			if (!have_plain) {
				plain = c;
				have_plain = true;
			}
		}
		if (have_plain) {
			*choice = plain;
			return true;
		}
	}
	return false;
}
