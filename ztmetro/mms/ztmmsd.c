// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztmmsd: picture messages (MMS) for ztOS.
 *
 * How an MMS comes in: the network sends an SMS to the WAP push port (2948)
 * holding an m-notification-ind, with the address of the message on the
 * carrier's MMSC. ztoscorenetworkingd puts each such SMS (its PDU, hex) into
 * PUSH_DIR; this daemon joins the parts, reads the notification, has the
 * modem daemon bring up the access point MMS goes through ("mms on" on its
 * control pipe; for T-Mobile and Mint that is the data access point itself),
 * fetches the message over HTTP from the MMSC, through that interface only,
 * answers m-notifyresp-ind, and stores the message's parts in STORE/<id>/.
 * A line in the SMS log (comm.h's SMS_LOG) tells Messaging:
 *
 *	in <time> <from> <text, escaped> mms:<id>
 *
 * Sending ("send" on CTL): an m-send-req is built and POSTed to the MMSC;
 * the answer (m-send-conf) becomes "out <time> <number> ok|failed" in the
 * same log, as for an SMS.
 *
 * CTL, /run/ztmmsd.ctl, one line each:
 *	send NUMBERS FILES TEXT	NUMBERS: one or more, comma separated; FILES:
 *				paths, '|' separated, or "-" for none; TEXT
 *				escaped as in the SMS log (\n, \t, \\).
 *				Anyone may write CTL, and this daemon is
 *				root: a file is sent only from OUTBOX
 *				(sticky, like /tmp), a regular file owned
 *				by a user, not root, and not a link. So
 *				nobody can send what they could not read.
 *	retry			fetch what is waiting again now
 *
 * The settings: CONF (Settings > cellular + SIM > MMS), "key = value":
 *	enabled = 1	nothing goes to the network without it: notifications
 *			wait in WAIT_DIR, sends fail at once
 *	mmsc, proxy, port, apn, user_agent, uaprof
 * Without an mmsc, the SIM's carrier's from apns-conf.xml (apnconf.c).
 *
 * Nothing about a message (numbers, text, addresses of messages) goes to
 * the journal: sizes, types and results only.
 *
 * Tests and tools, no network:
 *	ztmmsd --decode FILE		an MMS PDU's headers and parts
 *	ztmmsd --push HEX...		SMS PDUs as the modem gives them: the
 *					notification they make up
 *	ztmmsd --encode-send OUT NUMBER TEXT [FILE TYPE]...
 *	ztmmsd --apn MCCMNC [GID1 [SPN]] [FILE...]
 *	ztmmsd --store-test DIR FILE	store a retrieve-conf as received into DIR
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "apnconf.h"
#include "mmspdu.h"

#define CTL		"/run/ztmmsd.ctl"
#define MODEM_CTL	"/run/ztoscorenetworking.ctl"
#define MODEM_MMS	"/run/ztoscorenetworking/mms"	/* iface=, dns=: where MMS goes, while it may */
#define SIM_FILE	"/run/ztoscorenetworking/sim"
#define CONF		"/etc/ztoscorenetworking/mms"
#define MMS_DIR		"/var/lib/ztoscorenetworking/mms"
#define PUSH_DIR	MMS_DIR "/push"		/* from ztoscorenetworkingd */
#define WAIT_DIR	MMS_DIR "/waiting"	/* notifications not fetched yet */
#define STORE		MMS_DIR "/store"
#define OUTBOX		MMS_DIR "/outbox"	/* pictures to send: the apps write them here */
#define SMS_LOG		"/var/lib/ztoscorenetworking/sms/log"
#define MAX_MMS		(1024 * 1024)	/* T-Mobile's and Android's usual limit */
#define HTTP_TIMEOUT_MS	45000
#define BEARER_WAIT_MS	40000

static bool verbose;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "ztmmsd: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static long ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static bool read_file(const char *path, uint8_t **data, size_t *len, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t *buf = malloc(max + 1);
	size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
	fclose(f);
	if (!buf || n > max) {
		free(buf);
		return false;
	}
	*data = buf;
	*len = n;
	return true;
}

static bool write_file(const char *path, const void *data, size_t len, mode_t mode)
{
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, len, f) == len;
	ok = !fflush(f) && ok;
	if (ok)
		fsync(fileno(f));
	ok = !fclose(f) && ok;
	if (ok) {
		chmod(tmp, mode);
		ok = !rename(tmp, path);
	}
	if (!ok)
		unlink(tmp);
	return ok;
}

/* ---- settings ---------------------------------------------------------------- */

struct mms_conf {
	bool enabled;		/* "enabled = 1": fetch and send; else notifications only wait */
	char mmsc[256], proxy[128], apn[101];
	int port;
	char user_agent[128], uaprof[256];
	char carrier[64];
};

static void conf_trim(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
	size_t lead = strspn(s, " \t");
	memmove(s, s + lead, strlen(s + lead) + 1);
}

/* The SIM as the modem daemon read it: mccmnc=, gid1=, spn= */
static bool sim_read(struct sim_id *sim)
{
	char line[128], mccmnc[8] = "";
	memset(sim, 0, sizeof(*sim));
	FILE *f = fopen(SIM_FILE, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "mccmnc=", 7))
			snprintf(mccmnc, sizeof(mccmnc), "%.6s", line + 7);
		else if (!strncmp(line, "gid1=", 5))
			snprintf(sim->gid1, sizeof(sim->gid1), "%.32s", line + 5);
		else if (!strncmp(line, "spn=", 4))
			snprintf(sim->spn, sizeof(sim->spn), "%.32s", line + 4);
	}
	if (f)
		fclose(f);
	if (strlen(mccmnc) < 5)
		return false;
	sim_id_from_mccmnc(sim, mccmnc, 0);
	return true;
}

static void conf_load(struct mms_conf *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->user_agent, sizeof(c->user_agent), "SAMSUNG-SM-G960F");
	snprintf(c->uaprof, sizeof(c->uaprof), "http://wap.samsungmobile.com/uaprof/SM-G960F.xml");
	char line[512];
	FILE *f = fopen(CONF, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *key = line, *value = eq + 1;
		conf_trim(key);
		conf_trim(value);
		if (!strcmp(key, "enabled")) c->enabled = atoi(value) == 1;
		else if (!strcmp(key, "mmsc")) snprintf(c->mmsc, sizeof(c->mmsc), "%s", value);
		else if (!strcmp(key, "proxy")) snprintf(c->proxy, sizeof(c->proxy), "%s", value);
		else if (!strcmp(key, "port")) c->port = atoi(value);
		else if (!strcmp(key, "apn")) snprintf(c->apn, sizeof(c->apn), "%s", value);
		else if (!strcmp(key, "user_agent") && value[0]) snprintf(c->user_agent, sizeof(c->user_agent), "%s", value);
		else if (!strcmp(key, "uaprof") && value[0]) snprintf(c->uaprof, sizeof(c->uaprof), "%s", value);
	}
	if (f)
		fclose(f);
	if (c->mmsc[0])
		return;
	/* nothing set: the SIM's carrier's */
	struct sim_id sim;
	struct apn_choice a;
	const char *files[] = { APNS_STOCK, APNS_EXTRA, NULL };
	if (sim_read(&sim) && apn_lookup(files, &sim, &a)) {
		snprintf(c->mmsc, sizeof(c->mmsc), "%s", a.mmsc);
		snprintf(c->proxy, sizeof(c->proxy), "%s", a.mms_proxy);
		c->port = a.mms_port;
		snprintf(c->apn, sizeof(c->apn), "%s", a.mms_apn);
		snprintf(c->carrier, sizeof(c->carrier), "%s", a.carrier);
	}
}

/* ---- the bearer: which interface, which name servers --------------------------- */

struct bearer {
	char iface[16];
	char dns[2][48];
};

static bool status_bearer(struct bearer *b)
{
	char line[256];
	memset(b, 0, sizeof(*b));
	FILE *f = fopen(MODEM_MMS, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "iface=", 6))
			snprintf(b->iface, sizeof(b->iface), "%.15s", line + 6);
		else if (!strncmp(line, "dns=", 4)) {
			char *comma = strchr(line + 4, ',');
			if (comma)
				*comma = 0;
			snprintf(b->dns[0], sizeof(b->dns[0]), "%.47s", line + 4);
			if (comma)
				snprintf(b->dns[1], sizeof(b->dns[1]), "%.47s", comma + 1);
		}
	}
	if (f)
		fclose(f);
	return b->iface[0] != 0;
}

static void modem_tell(const char *line)
{
	int fd = open(MODEM_CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, line, strlen(line)) < 0) {
		/* it will not be up, and that is said when the wait ends */
	}
	close(fd);
}

static int bearer_users;

static bool bearer_get(struct bearer *b)
{
	if (bearer_users++ == 0)
		modem_tell("mms on\n");
	long until = ms() + BEARER_WAIT_MS;
	while (ms() < until) {
		if (status_bearer(b))
			return true;
		poll(NULL, 0, 500);
	}
	say("no connection for MMS came up");
	return false;
}

static void bearer_put(void)
{
	if (bearer_users > 0 && --bearer_users == 0)
		modem_tell("mms off\n");
}

/* ---- DNS through the bearer's own name servers ---------------------------------- */

static int bound_socket(int family, int type, const char *iface)
{
	int fd = socket(family, type | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (iface && iface[0] && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, (socklen_t)strlen(iface))) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool parse_addr(const char *s, struct sockaddr_storage *ss, socklen_t *len, int port)
{
	memset(ss, 0, sizeof(*ss));
	struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
	if (inet_pton(AF_INET, s, &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port = htons((uint16_t)port);
		*len = sizeof(*v4);
		return true;
	}
	if (inet_pton(AF_INET6, s, &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port = htons((uint16_t)port);
		*len = sizeof(*v6);
		return true;
	}
	return false;
}

/* One A (or AAAA) query to server over iface; the first address answered. */
static bool dns_query(const char *server, const char *iface, const char *host, int qtype,
	struct sockaddr_storage *out, socklen_t *out_len, int port)
{
	struct sockaddr_storage ss;
	socklen_t sl;
	if (!parse_addr(server, &ss, &sl, 53))
		return false;
	uint8_t q[300], r[1500];
	size_t n = 0;
	uint16_t id = (uint16_t)(ms() ^ getpid());
	q[n++] = id >> 8; q[n++] = id & 0xff;
	q[n++] = 0x01; q[n++] = 0x00;		/* recursion desired */
	q[n++] = 0; q[n++] = 1; memset(q + n, 0, 6); n += 6;
	for (const char *p = host; *p;) {
		size_t l = strcspn(p, ".");
		if (!l || l > 63 || n + l + 6 > sizeof(q))
			return false;
		q[n++] = (uint8_t)l;
		memcpy(q + n, p, l);
		n += l;
		p += l + (p[l] == '.');
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = (uint8_t)qtype; q[n++] = 0; q[n++] = 1;
	int fd = bound_socket(ss.ss_family, SOCK_DGRAM, iface);
	if (fd < 0)
		return false;
	bool found = false;
	for (int attempt = 0; attempt < 3 && !found; attempt++) {
		if (sendto(fd, q, n, 0, (struct sockaddr *)&ss, sl) != (ssize_t)n)
			break;
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 3000) <= 0)
			continue;
		ssize_t got = recv(fd, r, sizeof(r), 0);
		if (got < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 0x0f))
			continue;
		size_t at = 12, an = (size_t)(r[6] << 8 | r[7]);
		/* past the question */
		while (at < (size_t)got && r[at] && !(r[at] & 0xc0))
			at += 1 + r[at];
		at += (at < (size_t)got && (r[at] & 0xc0)) ? 2 : 1;
		at += 4;
		for (size_t i = 0; i < an && at + 10 < (size_t)got && !found; i++) {
			while (at < (size_t)got && r[at] && !(r[at] & 0xc0))
				at += 1 + r[at];
			at += (at < (size_t)got && (r[at] & 0xc0)) ? 2 : 1;
			if (at + 10 > (size_t)got)
				break;
			int type = r[at] << 8 | r[at + 1];
			size_t rdlen = (size_t)(r[at + 8] << 8 | r[at + 9]);
			at += 10;
			if (at + rdlen > (size_t)got)
				break;
			memset(out, 0, sizeof(*out));
			if (type == 1 && rdlen == 4) {
				struct sockaddr_in *v4 = (struct sockaddr_in *)out;
				v4->sin_family = AF_INET;
				v4->sin_port = htons((uint16_t)port);
				memcpy(&v4->sin_addr, r + at, 4);
				*out_len = sizeof(*v4);
				found = true;
			} else if (type == 28 && rdlen == 16) {
				struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)out;
				v6->sin6_family = AF_INET6;
				v6->sin6_port = htons((uint16_t)port);
				memcpy(&v6->sin6_addr, r + at, 16);
				*out_len = sizeof(*v6);
				found = true;
			}
			at += rdlen;
		}
	}
	close(fd);
	return found;
}

static bool resolve(const char *host, int port, const struct bearer *b, struct sockaddr_storage *out, socklen_t *len)
{
	if (parse_addr(host, out, len, port))
		return true;
	for (int i = 0; i < 2; i++) {
		if (!b->dns[i][0] || !strcmp(b->dns[i], "0.0.0.0"))
			continue;
		if (dns_query(b->dns[i], b->iface, host, 1, out, len, port) ||
		    dns_query(b->dns[i], b->iface, host, 28, out, len, port))
			return true;
	}
	/* no name servers of its own: the system's (right when the bearer is
	 * the default route) */
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
	char p[8];
	snprintf(p, sizeof(p), "%d", port);
	if (getaddrinfo(host, p, &hints, &res) || !res)
		return false;
	memcpy(out, res->ai_addr, res->ai_addrlen);
	*len = res->ai_addrlen;
	freeaddrinfo(res);
	return true;
}

/* ---- HTTP/1.1, plain, through the bearer ------------------------------------------ */

struct url {
	char host[200], path[512];
	int port;
};

static bool url_parse(const char *s, struct url *u)
{
	memset(u, 0, sizeof(*u));
	if (strncasecmp(s, "http://", 7))
		return false;	/* MMSCs are plain HTTP; https is not done here */
	s += 7;
	size_t hl = strcspn(s, ":/?");
	if (!hl || hl >= sizeof(u->host))
		return false;
	memcpy(u->host, s, hl);
	s += hl;
	u->port = 80;
	if (*s == ':') {
		u->port = atoi(s + 1);
		s += 1 + strspn(s + 1, "0123456789");
	}
	snprintf(u->path, sizeof(u->path), "%s", *s ? s : "/");
	return u->port > 0 && u->port < 65536;
}

static bool send_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len) {
		struct pollfd pfd = { fd, POLLOUT, 0 };
		if (poll(&pfd, 1, HTTP_TIMEOUT_MS) <= 0)
			return false;
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

/* Undo chunked transfer coding in place. */
static bool unchunk(uint8_t *body, size_t *len)
{
	size_t in = 0, out = 0, n = *len;
	for (;;) {
		char hex[20];
		size_t l = 0;
		while (in < n && body[in] != '\r' && l + 1 < sizeof(hex))
			hex[l++] = (char)body[in++];
		hex[l] = 0;
		if (in + 2 > n)
			return false;
		in += 2;
		size_t chunk = strtoul(hex, NULL, 16);
		if (!chunk)
			break;
		if (chunk > n - in)
			return false;
		memmove(body + out, body + in, chunk);
		out += chunk;
		in += chunk + 2;
	}
	*len = out;
	return true;
}

/* GET (body NULL) or POST url; the response body into *resp. */
static int http(const struct mms_conf *c, const struct bearer *b, const char *url, const uint8_t *body, size_t len,
	uint8_t **resp, size_t *resp_len)
{
	struct url u;
	*resp = NULL;
	*resp_len = 0;
	if (!url_parse(url, &u)) {
		say("not an http:// address");
		return -1;
	}
	const char *host = c->proxy[0] ? c->proxy : u.host;
	int port = c->proxy[0] ? (c->port ? c->port : 80) : u.port;
	struct sockaddr_storage ss;
	socklen_t sl;
	if (!resolve(host, port, b, &ss, &sl)) {
		say("could not find the MMSC's address");
		return -1;
	}
	int fd = bound_socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK, b->iface);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&ss, sl) && errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	struct pollfd pfd = { fd, POLLOUT, 0 };
	int err = 0;
	socklen_t el = sizeof(err);
	if (poll(&pfd, 1, HTTP_TIMEOUT_MS) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) {
		say("could not connect to the MMSC%s", c->proxy[0] ? "'s proxy" : "");
		close(fd);
		return -1;
	}
	char head[1400];
	char target[800];
	if (c->proxy[0])
		snprintf(target, sizeof(target), "%s", url);
	else
		snprintf(target, sizeof(target), "%s", u.path);
	int hn = snprintf(head, sizeof(head),
		"%s %s HTTP/1.1\r\nHost: %s%s%.0d\r\nUser-Agent: %s\r\nx-wap-profile: %s\r\n"
		"Accept: */*, application/vnd.wap.mms-message, application/vnd.wap.sic\r\n"
		"Accept-Language: en-US\r\nConnection: close\r\n",
		body ? "POST" : "GET", target, u.host, u.port != 80 ? ":" : "", u.port != 80 ? u.port : 0,
		c->user_agent, c->uaprof);
	if (body)
		hn += snprintf(head + hn, sizeof(head) - (size_t)hn,
			"Content-Type: application/vnd.wap.mms-message\r\nContent-Length: %zu\r\n", len);
	hn += snprintf(head + hn, sizeof(head) - (size_t)hn, "\r\n");
	if ((size_t)hn >= sizeof(head) || !send_all(fd, head, (size_t)hn) || (body && !send_all(fd, body, len))) {
		close(fd);
		return -1;
	}
	size_t cap = 2 * MAX_MMS + 8192, got = 0;
	uint8_t *buf = malloc(cap + 1);
	if (!buf) {
		close(fd);
		return -1;
	}
	for (;;) {
		pfd.events = POLLIN;
		if (poll(&pfd, 1, HTTP_TIMEOUT_MS) <= 0)
			break;
		ssize_t n = recv(fd, buf + got, cap - got, 0);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (got == cap)
			break;
	}
	close(fd);
	buf[got] = 0;
	int status = -1;
	uint8_t *end = memmem(buf, got, "\r\n\r\n", 4);
	if (got < 12 || !end || sscanf((char *)buf, "HTTP/%*d.%*d %d", &status) != 1) {
		free(buf);
		return -1;
	}
	*end = 0;
	bool chunked = strcasestr((char *)buf, "\r\ntransfer-encoding: chunked") != NULL;
	char *cl = strcasestr((char *)buf, "\r\ncontent-length:");
	size_t blen = got - (size_t)(end + 4 - buf);
	memmove(buf, end + 4, blen);
	if (chunked && !unchunk(buf, &blen))
		blen = 0;
	else if (cl && !chunked) {
		size_t want = strtoul(cl + 17, NULL, 10);
		if (want < blen)
			blen = want;
	}
	*resp = buf;
	*resp_len = blen;
	return status;
}

/* ---- the SMS log and the shell ------------------------------------------------------ */

static void log_escape(char *out, size_t size, const char *in)
{
	size_t o = 0;
	for (; *in && o + 3 < size; in++) {
		if (*in == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
		else if (*in == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
		else if (*in == '\t' || *in == '\r') out[o++] = ' ';
		else out[o++] = *in;
	}
	out[o] = 0;
}

static const char *log_path = SMS_LOG;

static void sms_log(const char *fmt, ...)
{
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", log_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		mkdir(dir, 0755);
	}
	FILE *f = fopen(log_path, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	if (!fflush(f))
		fsync(fileno(f));
	fclose(f);
	chmod(log_path, 0644);
}

static void tell_shell(const char *line)
{
	char path[64];
	for (int uid = 1000; uid < 1010; uid++) {
		snprintf(path, sizeof(path), "/run/user/%d/ztmetro.ctl", uid);
		int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			if (write(fd, line, strlen(line)) < 0) {
				/* the shell reads the log at its next look anyway */
			}
			close(fd);
		}
	}
}

/* ---- storing a message ------------------------------------------------------------ */

static void safe_name(char *out, size_t size, const char *name, int index, const char *type)
{
	size_t o = 0;
	for (const char *c = name; *c && o + 1 < size && o < 60; c++)
		out[o++] = isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_' ? *c : '_';
	out[o] = 0;
	if (!o || out[0] == '.') {
		const char *ext = strstr(type, "jpeg") ? "jpg" : strstr(type, "png") ? "png" : strstr(type, "gif") ? "gif" :
			strstr(type, "text/plain") ? "txt" : strstr(type, "smil") ? "smil" : strstr(type, "video") ? "mp4" :
			strstr(type, "audio") ? "amr" : strstr(type, "vcard") || strstr(type, "vCard") ? "vcf" : "bin";
		snprintf(out, size, "part%d.%s", index, ext);
	}
}

/*
 * dir/<parts>, and dir/meta, one line per part: "part\t<type>\t<file>", and
 * "from\t", "to\t", "subject\t", "date\t". The text of the text parts back
 * in text (joined), for the log line.
 */
static bool store_message(const char *dir, const struct mms_message *m, char *text, size_t text_size,
	int *pictures)
{
	if (mkdir(dir, 0755) && errno != EEXIST)
		return false;
	char meta[8192];
	size_t mo = 0;
	text[0] = 0;
	*pictures = 0;
	size_t to = 0;
	mo += (size_t)snprintf(meta + mo, sizeof(meta) - mo, "from\t%s\n", m->from);
	for (int i = 0; i < m->n_to && mo < sizeof(meta); i++)
		mo += (size_t)snprintf(meta + mo, sizeof(meta) - mo, "to\t%s\n", m->to[i]);
	if (m->subject[0] && mo < sizeof(meta))
		mo += (size_t)snprintf(meta + mo, sizeof(meta) - mo, "subject\t%s\n", m->subject);
	if (mo < sizeof(meta))
		mo += (size_t)snprintf(meta + mo, sizeof(meta) - mo, "date\t%lld\n", m->date ? m->date : (long long)time(NULL));
	for (int i = 0; i < m->n_parts && mo < sizeof(meta); i++) {
		const struct mms_part *p = &m->parts[i];
		char name[80], path[700];
		safe_name(name, sizeof(name), p->name, i, p->content_type);
		/* two parts of one name: the second numbered */
		for (int j = 0; j < i; j++) {
			char other[80];
			safe_name(other, sizeof(other), m->parts[j].name, j, m->parts[j].content_type);
			if (!strcmp(other, name)) {
				char again[80];
				snprintf(again, sizeof(again), "%d-%.70s", i, name);
				memcpy(name, again, sizeof(name));
				break;
			}
		}
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (!write_file(path, p->data, p->len, 0644))
			return false;
		mo += (size_t)snprintf(meta + mo, sizeof(meta) - mo, "part\t%s\t%s\n", p->content_type, name);
		if (!strncasecmp(p->content_type, "text/plain", 10) && to + p->len + 2 < text_size) {
			if (to)
				text[to++] = '\n';
			memcpy(text + to, p->data, p->len);
			to += p->len;
			text[to] = 0;
		} else if (!strncasecmp(p->content_type, "image/", 6) || !strncasecmp(p->content_type, "video/", 6)) {
			(*pictures)++;
		}
	}
	if (mo >= sizeof(meta))
		return false;
	char path[700];
	snprintf(path, sizeof(path), "%s/meta", dir);
	return write_file(path, meta, mo, 0644);
}

/* The SMS log's line for a message received, and the banner. */
static void announce(const char *id, const struct mms_message *m, const char *text, int pictures)
{
	char body[4096], escaped[8192];
	if (text[0])
		snprintf(body, sizeof(body), "%s", text);
	else if (m->subject[0])
		snprintf(body, sizeof(body), "%s", m->subject);
	else
		snprintf(body, sizeof(body), "%s", pictures ? "(picture)" : "(multimedia message)");
	log_escape(escaped, sizeof(escaped), body);
	/* a group message: the others too, so Messaging can show the group */
	sms_log("in\t%ld\t%s\t%s\tmms:%s\n", (long)time(NULL), m->from[0] ? m->from : "unknown", escaped, id);
	char banner[300], line[160];
	snprintf(line, sizeof(line), "%s", pictures ? "Picture message" : "Multimedia message");
	snprintf(banner, sizeof(banner), "notify Messaging|%s|%s\n", m->from[0] ? m->from : "MMS", line);
	tell_shell(banner);
}

/* ---- receiving -------------------------------------------------------------------- */

static unsigned fnv(const void *data, size_t len)
{
	unsigned h = 2166136261u;
	const uint8_t *p = data;
	for (size_t i = 0; i < len; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* A notification kept until it is fetched: WAIT_DIR/<hash>.ind */
static void notification_keep(const uint8_t *pdu, size_t len)
{
	char path[300];
	mkdir(MMS_DIR, 0755);
	mkdir(WAIT_DIR, 0700);
	snprintf(path, sizeof(path), "%s/%08x.ind", WAIT_DIR, fnv(pdu, len));
	if (access(path, F_OK) == 0) {
		say("a notification already had (ignored)");
		return;
	}
	write_file(path, pdu, len, 0600);
	say("a notification came: %zu bytes", len);
}

static bool fetch_one(const char *path, const struct mms_conf *c)
{
	uint8_t *ind;
	size_t ind_len;
	struct mms_message n;
	if (!read_file(path, &ind, &ind_len, 4096))
		return false;
	bool decoded = mms_decode(ind, ind_len, &n);
	free(ind);
	if (!decoded || n.type != MMS_NOTIFICATION_IND || !n.content_location[0]) {
		say("not a notification we can use: dropped");
		unlink(path);
		return true;
	}
	struct stat st;
	if (!stat(path, &st) && n.expiry && !n.expiry_relative && time(NULL) > (time_t)n.expiry) {
		say("expired before it was fetched: dropped");
		unlink(path);
		return true;
	}
	struct bearer b;
	if (!bearer_get(&b)) {
		bearer_put();
		return false;
	}
	uint8_t *resp;
	size_t rlen;
	int status = http(c, &b, n.content_location, NULL, 0, &resp, &rlen);
	struct mms_message m;
	bool ok = status == 200 && mms_decode(resp, rlen, &m) && m.type == MMS_RETRIEVE_CONF;
	say("fetch: HTTP %d, %zu bytes, %s", status, rlen, ok ? "a message" : "not a message");
	if (ok) {
		char id[40], dir[300], text[4096];
		int pictures;
		snprintf(id, sizeof(id), "%ld-%08x", (long)time(NULL), fnv(resp, rlen));
		mkdir(STORE, 0755);
		snprintf(dir, sizeof(dir), "%s/%s", STORE, id);
		if (store_message(dir, &m, text, sizeof(text), &pictures)) {
			announce(id, &m, text, pictures);
			say("stored: %d parts, %d pictures", m.n_parts, pictures);
		} else {
			say("could not store it");
			ok = false;
		}
	}
	free(resp);
	if (ok) {
		/* told it was taken: else the MMSC keeps it and notifies again */
		uint8_t pdu[300];
		size_t plen = mms_encode_notifyresp(pdu, sizeof(pdu), n.transaction_id, MMS_STATUS_RETRIEVED);
		uint8_t *ack;
		size_t alen;
		int s2 = plen ? http(c, &b, c->mmsc, pdu, plen, &ack, &alen) : -1;
		say("notifyresp: HTTP %d", s2);
		free(ack);
		unlink(path);
	}
	bearer_put();
	return ok;
}

static void fetch_waiting(void)
{
	struct mms_conf c;
	conf_load(&c);
	if (!c.enabled) {
		say("MMS is switched off (Settings > cellular + SIM > MMS): notifications wait");
		return;
	}
	if (!c.mmsc[0]) {
		say("no MMSC set (Settings > cellular + SIM > MMS), and none known for this SIM");
		return;
	}
	DIR *d = opendir(WAIT_DIR);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!strstr(e->d_name, ".ind") || strstr(e->d_name, ".new"))
			continue;
		char path[600];
		snprintf(path, sizeof(path), "%s/%s", WAIT_DIR, e->d_name);
		fetch_one(path, &c);
	}
	if (d)
		closedir(d);
}

/* Parts of one push, kept until all have come: PUSH_DIR/part-<from hash>-<ref>-<total>.<n> */
static bool push_join(const struct sms_wap *w, const char *dir, uint8_t *out, size_t *out_len, size_t size)
{
	if (!w->total || w->total == 1) {
		memcpy(out, w->data, w->len);
		*out_len = w->len;
		return true;
	}
	char base[400], path[450];
	snprintf(base, sizeof(base), "%s/part-%08x-%u-%u", dir, fnv(w->from, strlen(w->from)), w->ref, w->total);
	snprintf(path, sizeof(path), "%s.%u", base, w->part);
	write_file(path, w->data, w->len, 0600);
	size_t used = 0;
	for (unsigned i = 1; i <= w->total; i++) {
		uint8_t *d;
		size_t l;
		snprintf(path, sizeof(path), "%s.%u", base, i);
		if (!read_file(path, &d, &l, 200))
			return false;
		if (used + l > size) {
			free(d);
			return false;
		}
		memcpy(out + used, d, l);
		used += l;
		free(d);
	}
	for (unsigned i = 1; i <= w->total; i++) {
		snprintf(path, sizeof(path), "%s.%u", base, i);
		unlink(path);
	}
	*out_len = used;
	return true;
}

/* One SMS PDU (hex) from the modem daemon: a notification, once whole. */
static bool push_take(const char *hex, const char *dir, bool keep)
{
	struct sms_wap w;
	if (!sms_wap_push(hex, &w)) {
		say("an SMS that is not a WAP push: left");
		return false;
	}
	uint8_t whole[4096];
	size_t len;
	if (!push_join(&w, dir, whole, &len, sizeof(whole)))
		return true;	/* more parts to come */
	const uint8_t *body;
	size_t blen;
	struct mms_message m;
	if (!wsp_push_body(whole, len, &body, &blen) || !mms_decode(body, blen, &m)) {
		say("a WAP push that is not an MMS (%zu bytes): ignored", len);
		return true;
	}
	if (m.type == MMS_DELIVERY_IND) {
		say("a delivery report");
		return true;
	}
	if (m.type != MMS_NOTIFICATION_IND) {
		say("an MMS PDU of type 0x%02x: ignored", m.type);
		return true;
	}
	if (keep)
		notification_keep(body, blen);
	else
		printf("notification: size %lld, class 0x%02x, location %s, expiry %lld%s, tid %s, from %s\n",
			m.size, m.message_class, m.content_location[0] ? "set" : "missing", m.expiry,
			m.expiry_relative ? " (relative)" : "", m.transaction_id[0] ? "set" : "missing",
			m.from[0] ? "set" : "missing");
	return true;
}

static void push_scan(void)
{
	DIR *d = opendir(PUSH_DIR);
	struct dirent *e;
	bool any = false;
	while (d && (e = readdir(d))) {
		if (!strstr(e->d_name, ".pdu") || strstr(e->d_name, ".new"))
			continue;
		char path[600];
		snprintf(path, sizeof(path), "%s/%s", PUSH_DIR, e->d_name);
		uint8_t *hex;
		size_t len;
		if (read_file(path, &hex, &len, 1024)) {
			hex[len] = 0;
			hex[strcspn((char *)hex, "\r\n")] = 0;
			push_take((char *)hex, PUSH_DIR, true);
			free(hex);
			any = true;
		}
		unlink(path);
	}
	if (d)
		closedir(d);
	if (any)
		fetch_waiting();
}

/* ---- sending ---------------------------------------------------------------------- */

static const char *type_for(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot)
		return "application/octet-stream";
	dot++;
	if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
	if (!strcasecmp(dot, "png")) return "image/png";
	if (!strcasecmp(dot, "gif")) return "image/gif";
	if (!strcasecmp(dot, "mp4")) return "video/mp4";
	if (!strcasecmp(dot, "3gp")) return "video/3gpp";
	if (!strcasecmp(dot, "amr")) return "audio/amr";
	if (!strcasecmp(dot, "vcf")) return "text/x-vCard";
	if (!strcasecmp(dot, "txt")) return "text/plain";
	return "application/octet-stream";
}

/* An m-send-req for numbers (comma separated), files ('|' separated or "-")
 * and text into *pdu. */
static bool outbox_only = true;	/* off for the offline tools */

/* A file someone may have us send: see CTL above. */
static bool sendable(const char *path)
{
	if (!outbox_only)
		return true;
	size_t n = strlen(OUTBOX);
	if (strncmp(path, OUTBOX "/", n + 1) || strchr(path + n + 1, '/') || strstr(path, ".."))
		return false;
	struct stat st;
	return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid != 0 && st.st_nlink == 1;
}

static bool build_send(const char *numbers, const char *files, const char *text, uint8_t **pdu, size_t *len)
{
	char nums[512], fl[2048];
	const char *to[MMS_MAX_TO];
	int n_to = 0;
	snprintf(nums, sizeof(nums), "%s", numbers);
	for (char *save = NULL, *t = strtok_r(nums, ",", &save); t && n_to < MMS_MAX_TO; t = strtok_r(NULL, ",", &save))
		if (t[0] && strspn(t, "+0123456789") == strlen(t))
			to[n_to++] = t;
	if (!n_to)
		return false;
	struct mms_attachment att[8];
	uint8_t *datas[8];
	char names[8][40];
	int n_att = 0;
	size_t total = text ? strlen(text) : 0;
	snprintf(fl, sizeof(fl), "%s", files);
	bool ok = true;
	if (strcmp(fl, "-")) {
		for (char *save = NULL, *f = strtok_r(fl, "|", &save); f && n_att < 8; f = strtok_r(NULL, "|", &save)) {
			size_t l;
			if (!sendable(f)) {
				say("an attachment not in the outbox, or not a user's own file: refused");
				ok = false;
				break;
			}
			if (!read_file(f, &datas[n_att], &l, MAX_MMS)) {
				say("an attachment could not be read, or is over %d KB", MAX_MMS / 1024);
				ok = false;
				break;
			}
			const char *type = type_for(f);
			const char *ext = strrchr(f, '.');
			snprintf(names[n_att], sizeof(names[0]), "%s_%d%s", !strncmp(type, "image/", 6) ? "image" :
				!strncmp(type, "video/", 6) ? "video" : !strncmp(type, "audio/", 6) ? "audio" : "file",
				n_att, ext && strlen(ext) < 6 ? ext : "");
			att[n_att] = (struct mms_attachment){ type, names[n_att], datas[n_att], l };
			total += l;
			n_att++;
		}
	}
	if (ok && total > MAX_MMS - 4096) {
		say("too big to send: %zu KB", total / 1024);
		ok = false;
	}
	*pdu = NULL;
	if (ok) {
		size_t cap = total + 8192;
		*pdu = malloc(cap);
		char tid[40];
		snprintf(tid, sizeof(tid), "zt%lx%04x", (long)time(NULL), (unsigned)(ms() & 0xffff));
		*len = *pdu ? mms_encode_send_req(*pdu, cap, tid, to, n_to, NULL, text, att, n_att) : 0;
		ok = *len > 0;
	}
	for (int i = 0; i < n_att; i++)
		free(datas[i]);
	if (!ok) {
		free(*pdu);
		*pdu = NULL;
	}
	return ok;
}

static void unescape(char *s)
{
	char *o = s;
	for (char *i = s; *i; i++) {
		if (i[0] == '\\' && i[1]) {
			i++;
			*o++ = *i == 'n' ? '\n' : *i == 't' ? '\t' : *i;
		} else {
			*o++ = *i;
		}
	}
	*o = 0;
}

static void send_message(char *args)
{
	char *numbers = strtok(args, " "), *files = strtok(NULL, " "), *text = strtok(NULL, "");
	if (!numbers || !files)
		return;
	if (text)
		unescape(text);
	uint8_t *pdu;
	size_t len;
	bool ok = false;
	struct mms_conf c;
	conf_load(&c);
	if (!c.enabled)
		say("MMS is switched off: not sent");
	else if (!c.mmsc[0])
		say("no MMSC set: not sent");
	else if (build_send(numbers, files, text, &pdu, &len)) {
		struct bearer b;
		if (bearer_get(&b)) {
			uint8_t *resp;
			size_t rlen;
			int status = http(&c, &b, c.mmsc, pdu, len, &resp, &rlen);
			struct mms_message m;
			ok = status == 200 && mms_decode(resp, rlen, &m) && m.type == MMS_SEND_CONF &&
				m.response_status == 0x80;
			say("send: %zu bytes, HTTP %d, %s", len, status, ok ? "taken" : "refused");
			free(resp);
		}
		bearer_put();
		free(pdu);
	}
	char nums[512];
	snprintf(nums, sizeof(nums), "%s", numbers);
	for (char *save = NULL, *t = strtok_r(nums, ",", &save); t; t = strtok_r(NULL, ",", &save))
		sms_log("out\t%ld\t%s\t%s\n", (long)time(NULL), t, ok ? "ok" : "failed");
}

/* ---- the daemon ------------------------------------------------------------------- */

static void command(char *l)
{
	if (!strncmp(l, "send ", 5))
		send_message(l + 5);
	else if (!strcmp(l, "retry"))
		fetch_waiting();
}

static int run(void)
{
	signal(SIGPIPE, SIG_IGN);
	mkdir(MMS_DIR, 0755);
	mkdir(PUSH_DIR, 0700);
	mkdir(WAIT_DIR, 0700);
	mkdir(STORE, 0755);
	mkdir(OUTBOX, 01777);
	chmod(OUTBOX, 01777);
	unlink(CTL);
	mkfifo(CTL, 0666);
	chmod(CTL, 0666);	/* the apps send through it (umask aside) */
	int ctl = open(CTL, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ctl < 0 || in < 0 || inotify_add_watch(in, PUSH_DIR, IN_MOVED_TO | IN_CLOSE_WRITE) < 0) {
		say("could not start: %s", strerror(errno));
		return 1;
	}
	say("ready");
	push_scan();
	long next_retry = ms() + 600000;
	char buf[8192];
	size_t have = 0;
	for (;;) {
		struct pollfd p[2] = { { ctl, POLLIN, 0 }, { in, POLLIN, 0 } };
		long wait = next_retry - ms();
		poll(p, 2, wait > 0 ? (int)(wait < 600000 ? wait : 600000) : 0);
		if (p[1].revents & POLLIN) {
			char ev[4096];
			while (read(in, ev, sizeof(ev)) > 0) {
			}
			push_scan();
		}
		if (p[0].revents & POLLIN) {
			ssize_t n = read(ctl, buf + have, sizeof(buf) - 1 - have);
			if (n > 0) {
				have += (size_t)n;
				buf[have] = 0;
				char *nl;
				while ((nl = memchr(buf, '\n', have))) {
					*nl = 0;
					command(buf);
					size_t used = (size_t)(nl + 1 - buf);
					memmove(buf, nl + 1, have - used);
					have -= used;
				}
				if (have == sizeof(buf) - 1)
					have = 0;
			}
		}
		if (ms() >= next_retry) {
			/* anything not fetched (no connection then), every ten minutes */
			fetch_waiting();
			next_retry = ms() + 600000;
		}
	}
}

/* ---- tools ------------------------------------------------------------------------ */

static int tool_decode(const char *path)
{
	uint8_t *d;
	size_t n;
	struct mms_message m;
	if (!read_file(path, &d, &n, 4 * MAX_MMS) || !mms_decode(d, n, &m)) {
		fprintf(stderr, "not an MMS PDU\n");
		return 1;
	}
	printf("type 0x%02x version 0x%02x tid %s\n", m.type, m.version, m.transaction_id);
	printf("from %s\n", m.from);
	for (int i = 0; i < m.n_to; i++)
		printf("to %s\n", m.to[i]);
	printf("subject %s\ncontent-location %s\nsize %lld\nexpiry %lld%s\nstatus 0x%02x %s\nmessage-id %s\n",
		m.subject, m.content_location, m.size, m.expiry, m.expiry_relative ? " relative" : "",
		m.response_status, m.response_text, m.message_id);
	printf("content-type %s\n", m.content_type);
	for (int i = 0; i < m.n_parts; i++)
		printf("part %d: %s charset=%s name=%s cid=%s, %zu bytes\n", i, m.parts[i].content_type,
			m.parts[i].charset, m.parts[i].name, m.parts[i].content_id, m.parts[i].len);
	free(d);
	return 0;
}

static int tool_apn(int argc, char **argv)
{
	struct sim_id sim;
	memset(&sim, 0, sizeof(sim));
	sim_id_from_mccmnc(&sim, argv[0], 0);
	int i = 1;
	if (i < argc && !strchr(argv[i], '/')) snprintf(sim.gid1, sizeof(sim.gid1), "%s", argv[i++]);
	if (i < argc && !strchr(argv[i], '/')) snprintf(sim.spn, sizeof(sim.spn), "%s", argv[i++]);
	const char *files[8];
	int nf = 0;
	for (; i < argc && nf < 7; i++)
		files[nf++] = argv[i];
	if (!nf) {
		files[nf++] = APNS_STOCK;
		files[nf++] = APNS_EXTRA;
	}
	files[nf] = NULL;
	struct apn_choice a;
	if (!apn_lookup(files, &sim, &a)) {
		printf("none\n");
		return 1;
	}
	printf("carrier=%s\napn=%s\nuser=%s\npassword=%s\nauth=%s\nprotocol=%s\nmms_apn=%s\nmmsc=%s\n"
		"mms_proxy=%s\nmms_port=%d\nims_apn=%s\n", a.carrier, a.apn, a.user, a.password, a.auth,
		a.protocol, a.mms_apn, a.mmsc, a.mms_proxy, a.mms_port, a.ims_apn);
	return 0;
}

static int tool_encode_send(int argc, char **argv)
{
	/* OUT NUMBER TEXT [FILE]... */
	char files[2048] = "";
	size_t o = 0;
	for (int i = 3; i < argc; i++)
		o += (size_t)snprintf(files + o, sizeof(files) - o, "%s%s", o ? "|" : "", argv[i]);
	uint8_t *pdu;
	size_t len;
	if (!build_send(argv[1], o ? files : "-", argv[2], &pdu, &len))
		return 1;
	bool ok = write_file(argv[0], pdu, len, 0644);
	free(pdu);
	return ok ? 0 : 1;
}

static int tool_store(const char *dir, const char *path, const char *log)
{
	uint8_t *d;
	size_t n;
	struct mms_message m;
	if (!read_file(path, &d, &n, 4 * MAX_MMS) || !mms_decode(d, n, &m) || m.type != MMS_RETRIEVE_CONF)
		return 1;
	char text[4096], sub[600];
	int pictures;
	snprintf(sub, sizeof(sub), "%s/test", dir);
	if (!store_message(sub, &m, text, sizeof(text), &pictures))
		return 1;
	log_path = log;
	announce("test", &m, text, pictures);
	printf("stored, %d parts, %d pictures, text %zu bytes\n", m.n_parts, pictures, strlen(text));
	free(d);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--decode"))
		return tool_decode(argv[2]);
	if (argc >= 3 && !strcmp(argv[1], "--apn"))
		return tool_apn(argc - 2, argv + 2);
	if (argc >= 5 && !strcmp(argv[1], "--encode-send")) {
		outbox_only = false;
		return tool_encode_send(argc - 2, argv + 2);
	}
	if (argc >= 5 && !strcmp(argv[1], "--store-test"))
		return tool_store(argv[2], argv[3], argv[4]);
	if (argc >= 4 && !strcmp(argv[1], "--push")) {
		/* --push DIR HEX...: parts kept in DIR */
		for (int i = 3; i < argc; i++)
			push_take(argv[i], argv[2], false);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "-v"))
		verbose = true;
	return run();
}
