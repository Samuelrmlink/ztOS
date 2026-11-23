// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * usb-energy.h: the record of what each USB accessory drew from the phone
 * (ROADMAP 8.76). Header only: ztusbenergyd writes it, Settings' battery page
 * and USBView read it. No other dependency, so the host tests build it too.
 *
 * $XDG_STATE_HOME/ztos/usb-energy.json (~/.local/state/ztos/usb-energy.json),
 * replaced whole (a temporary file, fsync, rename, fsync of the directory):
 *
 *	{"version": 1, "units": "Wh", "updated": EPOCH,
 *	 "accessories": [{
 *	   "key": "05ac:110b:SERIAL",	vid:pid:serial, or vid:pid@PORT-PATH
 *	   "vid": "05ac", "pid": "110b", "serial": "", "port": "1-1",
 *	   "name": "Apple, Inc. EarPods", "max_power_ma": 100,
 *	   "first_seen": EPOCH, "last_seen": EPOCH,
 *	   "connected_s": S, "powered_s": S, "asleep_s": S,
 *	   "wh": WH, "method": "...",	the best figure, and how it was got
 *	   "upper_wh": WH,		bMaxPower x 5 V while the phone gave VBUS
 *	   "est_wh": WH, "est_err_wh": WH, "est_s": S,
 *	   "n_sessions": N,		all of them; "sessions" keeps the last 100
 *	   "sessions": [{"start": EPOCH, "end": EPOCH (0: still connected),
 *	     "connected_s", "powered_s", "asleep_s", "wh", "method",
 *	     "upper_wh", "est_wh", "est_err_wh", "est_s"}]}]}
 *
 * method, per session (README.md says how each is got and how good it is):
 *	"estimated from battery current"   the gauge's coulomb counter less the
 *		phone's own draw just before the plug-in; err_wh is a 90 % band
 *	"upper bound"	the device's declared bMaxPower at 5 V (2.5 mA while it
 *		or the phone is suspended): it draws no more than this
 *	"not powered by the phone"	VBUS came from elsewhere (a powered dock)
 *	"measured VBUS"	reserved: this phone has no meter for VBUS out
 * An accessory's own "method" is its sessions' if they agree, else "mixed".
 */
#ifndef ZT_USB_ENERGY_H
#define ZT_USB_ENERGY_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UE_METHOD_EST	"estimated from battery current"
#define UE_METHOD_BOUND	"upper bound"
#define UE_METHOD_NONE	"not powered by the phone"
#define UE_METHOD_VBUS	"measured VBUS"
#define UE_METHOD_MIXED	"mixed"

#define UE_MAX_ACC	64
#define UE_MAX_SESS	100	/* kept per accessory; the totals keep them all */

struct ue_sess {
	long long start, end;	/* wall clock; end 0: still connected */
	double connected_s, powered_s, asleep_s;
	double wh, upper_wh, est_wh, est_err_wh, est_s;
	char method[40];
};

struct ue_acc {
	char key[160];
	char vid[8], pid[8], serial[64], port[32];
	char name[128];
	int max_power_ma;
	long long first_seen, last_seen;
	double connected_s, powered_s, asleep_s;
	double wh, upper_wh, est_wh, est_err_wh, est_s;
	char method[40];
	int n_sessions;
	int n_sess;		/* kept, oldest first */
	struct ue_sess sess[UE_MAX_SESS];
};

struct ue_store {
	long long updated;
	int n;
	struct ue_acc acc[UE_MAX_ACC];
};

/* ---- where ---- */

static inline void ue_store_path(char *out, size_t size)
{
	const char *st = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (st && st[0] == '/')
		snprintf(out, size, "%s/ztos/usb-energy.json", st);
	else
		snprintf(out, size, "%s/.local/state/ztos/usb-energy.json", home && home[0] ? home : "/tmp");
}

/* The same units and rounding as the battery + data app (acct/ztusage.c) */
static inline const char *ue_fmt_wh(double w, char *s, size_t size)
{
	if (w < 0.001)
		snprintf(s, size, "%.2f mWh", w * 1000);
	else if (w < 1)
		snprintf(s, size, "%.0f mWh", w * 1000);
	else
		snprintf(s, size, "%.2f Wh", w);
	return s;
}

/* ---- the key ---- */

static inline void ue_make_key(char *out, size_t size, const char *vid, const char *pid, const char *serial,
	const char *port)
{
	if (serial && serial[0])
		snprintf(out, size, "%s:%s:%s", vid, pid, serial);
	else
		snprintf(out, size, "%s:%s@%s", vid, pid, port);
}

static inline struct ue_acc *ue_find(struct ue_store *s, const char *key)
{
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->acc[i].key, key))
			return &s->acc[i];
	return NULL;
}

/* The accessory for KEY, made if new. When the store is full, the one seen
 * least recently makes room. */
static inline struct ue_acc *ue_get(struct ue_store *s, const char *key)
{
	struct ue_acc *a = ue_find(s, key);
	if (a)
		return a;
	if (s->n < UE_MAX_ACC) {
		a = &s->acc[s->n++];
	} else {
		a = &s->acc[0];
		for (int i = 1; i < s->n; i++)
			if (s->acc[i].last_seen < a->last_seen)
				a = &s->acc[i];
	}
	memset(a, 0, sizeof(*a));
	snprintf(a->key, sizeof(a->key), "%s", key);
	return a;
}

/* A new session, at the end of the kept ones */
static inline struct ue_sess *ue_new_session(struct ue_acc *a, long long start)
{
	if (a->n_sess == UE_MAX_SESS) {
		memmove(&a->sess[0], &a->sess[1], sizeof(a->sess[0]) * (UE_MAX_SESS - 1));
		a->n_sess--;
	}
	struct ue_sess *s = &a->sess[a->n_sess++];
	memset(s, 0, sizeof(*s));
	s->start = start;
	a->n_sessions++;
	if (!a->first_seen)
		a->first_seen = start;
	a->last_seen = start;
	return s;
}

/* An accessory's totals and method from its sessions. Sessions dropped from
 * the kept 100 still count: what was in the totals before them is kept as
 * the difference, so this adds only what changed since. Callers give the
 * session as it was before (or zeros) and as it is now. */
static inline void ue_acc_add(struct ue_acc *a, const struct ue_sess *before, const struct ue_sess *now)
{
	a->connected_s += now->connected_s - before->connected_s;
	a->powered_s += now->powered_s - before->powered_s;
	a->asleep_s += now->asleep_s - before->asleep_s;
	a->wh += now->wh - before->wh;
	a->upper_wh += now->upper_wh - before->upper_wh;
	a->est_wh += now->est_wh - before->est_wh;
	a->est_err_wh += now->est_err_wh - before->est_err_wh;
	a->est_s += now->est_s - before->est_s;
}

/* An accessory's method: its kept sessions' if they agree (sessions it did
 * not power do not count against it), else "mixed" */
static inline void ue_acc_method(struct ue_acc *a)
{
	const char *m = NULL;
	for (int i = 0; i < a->n_sess; i++) {
		const char *sm = a->sess[i].method;
		if (!sm[0] || (!strcmp(sm, UE_METHOD_NONE) && m))
			continue;
		if (!m || !strcmp(m, UE_METHOD_NONE))
			m = sm;
		else if (strcmp(m, sm))
			m = UE_METHOD_MIXED;
	}
	snprintf(a->method, sizeof(a->method), "%s", m ? m : "");
}

/* ---- JSON out ---- */

static inline void ue_json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c == '\n')
			fputs("\\n", f);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static inline void ue_json_sess_fields(FILE *f, double connected, double powered, double asleep, double wh,
	const char *method, double upper, double est, double err, double est_s)
{
	fprintf(f, "\"connected_s\": %.1f, \"powered_s\": %.1f, \"asleep_s\": %.1f, \"wh\": %.6f, \"method\": ",
		connected, powered, asleep, wh);
	ue_json_str(f, method);
	fprintf(f, ", \"upper_wh\": %.6f, \"est_wh\": %.6f, \"est_err_wh\": %.6f, \"est_s\": %.1f", upper, est, err,
		est_s);
}

static inline void ue_write(FILE *f, const struct ue_store *s)
{
	fprintf(f, "{\"version\": 1, \"units\": \"Wh\", \"updated\": %lld,\n \"accessories\": [", s->updated);
	for (int i = 0; i < s->n; i++) {
		const struct ue_acc *a = &s->acc[i];
		fprintf(f, "%s\n  {\"key\": ", i ? "," : "");
		ue_json_str(f, a->key);
		fputs(", \"vid\": ", f);
		ue_json_str(f, a->vid);
		fputs(", \"pid\": ", f);
		ue_json_str(f, a->pid);
		fputs(", \"serial\": ", f);
		ue_json_str(f, a->serial);
		fputs(", \"port\": ", f);
		ue_json_str(f, a->port);
		fputs(", \"name\": ", f);
		ue_json_str(f, a->name);
		fprintf(f, ", \"max_power_ma\": %d,\n   \"first_seen\": %lld, \"last_seen\": %lld, ", a->max_power_ma,
			a->first_seen, a->last_seen);
		ue_json_sess_fields(f, a->connected_s, a->powered_s, a->asleep_s, a->wh, a->method, a->upper_wh,
			a->est_wh, a->est_err_wh, a->est_s);
		fprintf(f, ",\n   \"n_sessions\": %d, \"sessions\": [", a->n_sessions);
		for (int k = 0; k < a->n_sess; k++) {
			const struct ue_sess *e = &a->sess[k];
			fprintf(f, "%s\n    {\"start\": %lld, \"end\": %lld, ", k ? "," : "", e->start, e->end);
			ue_json_sess_fields(f, e->connected_s, e->powered_s, e->asleep_s, e->wh, e->method,
				e->upper_wh, e->est_wh, e->est_err_wh, e->est_s);
			fputc('}', f);
		}
		fputs("]}", f);
	}
	fputs("\n]}\n", f);
}

/* Written whole and durably: a hard reset (the deadman's sysrq-b skips the
 * sync) must leave the old file or the new one, never an empty one. As
 * atomic_write in os/overlay/usr/local/bin/ztos-android. */
static inline bool ue_save(const char *path, const struct ue_store *s)
{
	char dir[PATH_MAX], tmp[PATH_MAX + 16];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (!slash)
		return false;
	*slash = 0;
	/* make the directories */
	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(dir, 0755);
			*p = '/';
		}
	}
	mkdir(dir, 0755);
	snprintf(tmp, sizeof(tmp), "%s/.usb-energy.XXXXXX", dir);
	int fd = mkstemp(tmp);
	if (fd < 0)
		return false;
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	fchmod(fd, 0644);
	ue_write(f, s);
	bool ok = fflush(f) == 0 && fsync(fd) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return false;
	}
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return true;
}

/* ---- JSON in: enough for the file above (objects, arrays, strings,
 * numbers, true/false/null), tolerant of fields it does not know ---- */

struct ue_json {
	const char *p, *end;
	bool bad;
};

static inline void ue_j_ws(struct ue_json *j)
{
	while (j->p < j->end && isspace((unsigned char)*j->p))
		j->p++;
}

static inline bool ue_j_eat(struct ue_json *j, char c)
{
	ue_j_ws(j);
	if (j->p < j->end && *j->p == c) {
		j->p++;
		return true;
	}
	return false;
}

static inline void ue_j_string(struct ue_json *j, char *out, size_t size)
{
	size_t n = 0;
	ue_j_ws(j);
	if (j->p >= j->end || *j->p != '"') {
		j->bad = true;
		return;
	}
	j->p++;
	while (j->p < j->end && *j->p != '"') {
		char c = *j->p++;
		if (c == '\\' && j->p < j->end) {
			c = *j->p++;
			if (c == 'n')
				c = '\n';
			else if (c == 't')
				c = '\t';
			else if (c == 'u' && j->end - j->p >= 4) {
				char hex[5] = { j->p[0], j->p[1], j->p[2], j->p[3], 0 };
				long v = strtol(hex, NULL, 16);
				j->p += 4;
				c = v < 0x80 ? (char)v : '?';
			}
		}
		if (out && n + 1 < size)
			out[n++] = c;
	}
	if (j->p >= j->end) {
		j->bad = true;
		return;
	}
	j->p++;
	if (out && size)
		out[n] = 0;
}

static inline double ue_j_number(struct ue_json *j)
{
	ue_j_ws(j);
	char *e;
	double v = strtod(j->p, &e);
	if (e == j->p)
		j->bad = true;
	j->p = e;
	return v;
}

static inline void ue_j_skip(struct ue_json *j)
{
	ue_j_ws(j);
	if (j->bad || j->p >= j->end) {
		j->bad = true;
		return;
	}
	char c = *j->p;
	if (c == '"') {
		ue_j_string(j, NULL, 0);
	} else if (c == '{' || c == '[') {
		char close = c == '{' ? '}' : ']';
		j->p++;
		if (ue_j_eat(j, close))
			return;
		do {
			if (c == '{') {
				ue_j_string(j, NULL, 0);
				if (!ue_j_eat(j, ':'))
					j->bad = true;
			}
			ue_j_skip(j);
		} while (!j->bad && ue_j_eat(j, ','));
		if (!ue_j_eat(j, close))
			j->bad = true;
	} else if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "null", 4)) {
		j->p += 4;
	} else if (!strncmp(j->p, "false", 5)) {
		j->p += 5;
	} else {
		ue_j_number(j);
	}
}

/* Calls FIELD(j, key, ctx) for each member of an object; FIELD reads the
 * value or returns false to have it skipped. */
static inline void ue_j_object(struct ue_json *j, bool (*field)(struct ue_json *, const char *, void *), void *ctx)
{
	if (!ue_j_eat(j, '{')) {
		j->bad = true;
		return;
	}
	if (ue_j_eat(j, '}'))
		return;
	do {
		char key[48];
		ue_j_string(j, key, sizeof(key));
		if (j->bad || !ue_j_eat(j, ':')) {
			j->bad = true;
			return;
		}
		if (!field(j, key, ctx))
			ue_j_skip(j);
	} while (!j->bad && ue_j_eat(j, ','));
	if (!ue_j_eat(j, '}'))
		j->bad = true;
}

static inline void ue_j_array(struct ue_json *j, void (*item)(struct ue_json *, void *), void *ctx)
{
	if (!ue_j_eat(j, '[')) {
		j->bad = true;
		return;
	}
	if (ue_j_eat(j, ']'))
		return;
	do
		item(j, ctx);
	while (!j->bad && ue_j_eat(j, ','));
	if (!ue_j_eat(j, ']'))
		j->bad = true;
}

/* the fields a session and an accessory share */
static inline bool ue_j_common(struct ue_json *j, const char *k, double *connected, double *powered, double *asleep,
	double *wh, char *method, double *upper, double *est, double *err, double *est_s)
{
	struct { const char *name; double *v; } nums[] = {
		{ "connected_s", connected }, { "powered_s", powered }, { "asleep_s", asleep }, { "wh", wh },
		{ "upper_wh", upper }, { "est_wh", est }, { "est_err_wh", err }, { "est_s", est_s },
	};
	for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
		if (!strcmp(k, nums[i].name)) {
			*nums[i].v = ue_j_number(j);
			return true;
		}
	}
	if (!strcmp(k, "method")) {
		ue_j_string(j, method, 40);
		return true;
	}
	return false;
}

static inline bool ue_j_sess_field(struct ue_json *j, const char *k, void *ctx)
{
	struct ue_sess *s = ctx;
	if (!strcmp(k, "start"))
		s->start = (long long)ue_j_number(j);
	else if (!strcmp(k, "end"))
		s->end = (long long)ue_j_number(j);
	else
		return ue_j_common(j, k, &s->connected_s, &s->powered_s, &s->asleep_s, &s->wh, s->method,
			&s->upper_wh, &s->est_wh, &s->est_err_wh, &s->est_s);
	return true;
}

static inline void ue_j_sess(struct ue_json *j, void *ctx)
{
	struct ue_acc *a = ctx;
	struct ue_sess s = { 0 };
	ue_j_object(j, ue_j_sess_field, &s);
	if (a->n_sess == UE_MAX_SESS) {
		memmove(&a->sess[0], &a->sess[1], sizeof(a->sess[0]) * (UE_MAX_SESS - 1));
		a->n_sess--;
	}
	a->sess[a->n_sess++] = s;
}

static inline bool ue_j_acc_field(struct ue_json *j, const char *k, void *ctx)
{
	struct ue_acc *a = ctx;
	struct { const char *name; char *s; size_t size; } strs[] = {
		{ "key", a->key, sizeof(a->key) }, { "vid", a->vid, sizeof(a->vid) }, { "pid", a->pid, sizeof(a->pid) },
		{ "serial", a->serial, sizeof(a->serial) }, { "port", a->port, sizeof(a->port) },
		{ "name", a->name, sizeof(a->name) },
	};
	for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
		if (!strcmp(k, strs[i].name)) {
			ue_j_string(j, strs[i].s, strs[i].size);
			return true;
		}
	}
	if (!strcmp(k, "max_power_ma"))
		a->max_power_ma = (int)ue_j_number(j);
	else if (!strcmp(k, "first_seen"))
		a->first_seen = (long long)ue_j_number(j);
	else if (!strcmp(k, "last_seen"))
		a->last_seen = (long long)ue_j_number(j);
	else if (!strcmp(k, "n_sessions"))
		a->n_sessions = (int)ue_j_number(j);
	else if (!strcmp(k, "sessions"))
		ue_j_array(j, ue_j_sess, a);
	else
		return ue_j_common(j, k, &a->connected_s, &a->powered_s, &a->asleep_s, &a->wh, a->method,
			&a->upper_wh, &a->est_wh, &a->est_err_wh, &a->est_s);
	return true;
}

static inline void ue_j_acc(struct ue_json *j, void *ctx)
{
	struct ue_store *s = ctx;
	struct ue_acc tmp;
	memset(&tmp, 0, sizeof(tmp));
	ue_j_object(j, ue_j_acc_field, &tmp);
	if (j->bad || !tmp.key[0])
		return;
	if (tmp.n_sessions < tmp.n_sess)
		tmp.n_sessions = tmp.n_sess;
	struct ue_acc *a = ue_get(s, tmp.key);
	*a = tmp;
}

static inline bool ue_j_top_field(struct ue_json *j, const char *k, void *ctx)
{
	struct ue_store *s = ctx;
	if (!strcmp(k, "accessories"))
		ue_j_array(j, ue_j_acc, s);
	else if (!strcmp(k, "updated"))
		s->updated = (long long)ue_j_number(j);
	else
		return false;
	return true;
}

static inline bool ue_parse(struct ue_store *s, const char *text, size_t len)
{
	memset(s, 0, sizeof(*s));
	struct ue_json j = { text, text + len, false };
	ue_j_object(&j, ue_j_top_field, s);
	return !j.bad;
}

/* The record from PATH. A missing file is an empty record (true); one that
 * does not parse is false, with what could be read kept. */
static inline bool ue_load(const char *path, struct ue_store *s)
{
	memset(s, 0, sizeof(*s));
	FILE *f = fopen(path, "re");
	if (!f)
		return errno == ENOENT;
	size_t cap = 1 << 16, len = 0;
	char *buf = malloc(cap);
	size_t got;
	while (buf && (got = fread(buf + len, 1, cap - len, f)) > 0) {
		len += got;
		if (len == cap) {
			char *more = realloc(buf, cap * 2);
			if (!more)
				break;
			buf = more;
			cap *= 2;
		}
	}
	fclose(f);
	if (!buf)
		return false;
	bool ok = ue_parse(s, buf, len);
	free(buf);
	return ok;
}

/* Most recently seen first, for showing */
static inline int ue_by_last_seen(const void *a, const void *b)
{
	const struct ue_acc *x = *(const struct ue_acc *const *)a, *y = *(const struct ue_acc *const *)b;
	return (x->last_seen < y->last_seen) - (x->last_seen > y->last_seen);
}

/* "≤ 83 mWh (upper bound)", "41 ± 12 mWh (estimated from battery current)" */
static inline void ue_describe(char *out, size_t size, double wh, const char *method, double err)
{
	char a[24], b[24];
	ue_fmt_wh(wh, a, sizeof(a));
	if (!strcmp(method, UE_METHOD_EST))
		snprintf(out, size, "%s ± %s, estimated from battery current", a, ue_fmt_wh(err, b, sizeof(b)));
	else if (!strcmp(method, UE_METHOD_BOUND))
		snprintf(out, size, "at most %s (upper bound, from bMaxPower)", a);
	else if (!strcmp(method, UE_METHOD_NONE))
		snprintf(out, size, "none: powered by something else");
	else if (!strcmp(method, UE_METHOD_MIXED))
		snprintf(out, size, "%s (estimates and upper bounds)", a);
	else
		snprintf(out, size, "%s, %s", a, method);
}

#endif
