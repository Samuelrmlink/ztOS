// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * wlpm: what Android's Broadcom driver (bcmdhd) does to the Wi-Fi chip when the
 * screen goes off, done here from userspace, and a log of what the link does.
 *
 * Mainline brcmfmac has no suspend knob, but it passes any firmware command
 * through its nl80211 vendor command (BROADCOM_OUI, BRCMF_VNDR_CMDS_DCMD;
 * root only): the same "wl" ioctls and iovars bcmdhd sets in dhd_set_suspend().
 * With the screen off:
 *
 *   bcn_li_dtim N	listen to every Nth DTIM beacon only (bcmdhd:
 *			dhd_get_suspend_bcn_li_dtim). N is the largest that keeps
 *			the wait for a frame under wifi-listen ms (700: at a DTIM
 *			of 3 x 100 TU, every 2nd, 614 ms) and inside the listen
 *			interval the chip announced at association (assoc_listen,
 *			beacons), past which the AP may drop what it buffered
 *   pm2_sleep_ret 200	back to sleep 200 ms after traffic. brcmfmac sets 2000
 *			whenever power save is switched on (cfg80211's "no
 *			timeout" clamped to its maximum); the firmware's own and
 *			Android's are 200
 *   pkt_filter		only frames to this phone's own address wake the host,
 *			and router advertisements, neighbour solicitations and
 *			DHCP replies (bcmdhd's filter 100, dhd_enable_packet_filter,
 *			forward-on-match mode). ARP requests for our address are
 *			answered by the chip itself (arp_ol/arpoe, which brcmfmac
 *			already turns on at boot, as it does IPv6 ND offload, ndoe)
 *
 * The screen coming on puts everything back. The power save mode itself
 * (PM_FAST) stays with NetworkManager's wifi.powersave (zrnperfd's
 * wifi-powersave); Samsung's build of bcmdhd (SUPPORT_PM2_ONLY) never uses
 * PM_MAX either.
 *
 * The log (journal, zrnperfd's stderr): every connect, roam and disconnect
 * with BSSID, band, channel, signal and the disconnect's reason; each change
 * of the screen-off settings; and every 5 minutes with the screen off one line
 * of BSSID, signal, power save mode and traffic.
 *
 * The firmware is little-endian, as is the phone: values go as they are.
 */
#define _GNU_SOURCE
#include "wlpm.h"
#include <dirent.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BRCM_OUI	0x001018
#define BRCM_DCMD	1	/* BRCMF_VNDR_CMDS_DCMD */
#define BRCM_ATTR_DATA	2	/* BRCMF_NLATTR_DATA */
#define WLC_GET_CHANNEL	29
#define WLC_GET_BCNPRD	75
#define WLC_GET_DTIMPRD	77
#define WLC_GET_PM	85
#define WLC_GET_RSSI	127
#define WLC_GET_VAR	262
#define WLC_SET_VAR	263

#define OFF_SLEEP_RET	200		/* ms awake after traffic, screen off */
#define ON_SLEEP_RET	2000		/* brcmfmac's, if none was read */
#define LINE_MS		(5 * 60 * 1000)	/* the periodic line, screen off */
#define REAPPLY_MS	8000		/* after a connect: NetworkManager sets power save */
enum { F_UNICAST = 100, F_RA, F_NS, F_DHCP, F_END };

bool wlpm_suspend_enabled = true, wlpm_filter_enabled = true;
const char *wlpm_name = "zrnperfd";	/* the log lines' prefix */
int wlpm_listen_ms = 700;

static int nlfd = -1, evfd = -1, family = -1;
static uint32_t seq;
static char ifname[IF_NAMESIZE];
static unsigned ifindex;
static bool screen_on = true, applied, filtering, warned;
static int saved_sleep_ret = -1, skip;	/* skip: bcn_li_dtim as set */
static long off_since, next_line, reapply_at;
static unsigned char bssid[6];
static bool have_bssid;
static unsigned long long last_rx, last_tx, last_drop;

static long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* ---- generic netlink, by hand ------------------------------------------- */

#define ATTRS(a, start, len) \
	for (struct nlattr *a = (struct nlattr *)(start); \
	     (char *)a + NLA_HDRLEN <= (char *)(start) + (len) && a->nla_len >= NLA_HDRLEN && \
	     (char *)a + a->nla_len <= (char *)(start) + (len); \
	     a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)))
#define ADATA(a)	((void *)((char *)(a) + NLA_HDRLEN))
#define ALEN(a)		((int)(a)->nla_len - NLA_HDRLEN)

static void put(struct nlmsghdr *n, int type, const void *d, int len) {
	struct nlattr *a = (struct nlattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy(ADATA(a), d, len);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(a->nla_len);
}

static struct nlmsghdr *start(void *buf, int type, int cmd) {
	struct nlmsghdr *n = buf;
	memset(n, 0, NLMSG_HDRLEN + GENL_HDRLEN);
	n->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
	n->nlmsg_type = type;
	n->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	n->nlmsg_seq = ++seq;
	((struct genlmsghdr *)NLMSG_DATA(n))->cmd = cmd;
	return n;
}

static int sock(void) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	struct sockaddr_nl a = { .nl_family = AF_NETLINK };
	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* send, then hand each reply's attributes to got() until the ack; 0 or -errno */
static int request(struct nlmsghdr *n, void (*got)(struct nlattr *, int, void *), void *arg) {
	if (send(nlfd, n, n->nlmsg_len, 0) < 0) return -errno;
	uint32_t want = n->nlmsg_seq;
	static char buf[16384] __attribute__((aligned(4)));
	for (;;) {
		ssize_t len = recv(nlfd, buf, sizeof(buf), 0);
		if (len < 0) return errno == EINTR ? -EINTR : -errno;
		for (struct nlmsghdr *m = (struct nlmsghdr *)buf; NLMSG_OK(m, (size_t)len); m = NLMSG_NEXT(m, len)) {
			if (m->nlmsg_seq != want) continue;
			if (m->nlmsg_type == NLMSG_ERROR) return ((struct nlmsgerr *)NLMSG_DATA(m))->error;
			if (m->nlmsg_type == NLMSG_DONE) return 0;
			if (got) got((struct nlattr *)((char *)NLMSG_DATA(m) + GENL_HDRLEN),
				     m->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, arg);
		}
	}
}

struct family_reply { int id, mlme; };
static void got_family(struct nlattr *attrs, int len, void *arg) {
	struct family_reply *r = arg;
	ATTRS(a, attrs, len) {
		if (a->nla_type == CTRL_ATTR_FAMILY_ID) r->id = *(uint16_t *)ADATA(a);
		if (a->nla_type != CTRL_ATTR_MCAST_GROUPS) continue;
		ATTRS(g, ADATA(a), ALEN(a)) {
			const char *name = NULL;
			int id = -1;
			ATTRS(f, ADATA(g), ALEN(g)) {
				if (f->nla_type == CTRL_ATTR_MCAST_GRP_NAME) name = ADATA(f);
				if (f->nla_type == CTRL_ATTR_MCAST_GRP_ID) id = *(uint32_t *)ADATA(f);
			}
			if (name && !strcmp(name, "mlme")) r->mlme = id;
		}
	}
}

/* the wireless interface: the first with a wireless/ directory */
static bool find_if(void) {
	if (ifindex && if_nametoindex(ifname) == ifindex) return true;
	ifindex = 0;
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[300];
		if (e->d_name[0] == '.' || strlen(e->d_name) >= IF_NAMESIZE) continue;
		snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", e->d_name);
		if (access(path, F_OK) != 0) continue;
		snprintf(ifname, sizeof(ifname), "%s", e->d_name);
		ifindex = if_nametoindex(ifname);
		break;
	}
	if (d) closedir(d);
	return ifindex != 0;
}

/* ---- the firmware's commands -------------------------------------------- */

struct dcmd_reply { void *buf; int len; };
static void got_dcmd(struct nlattr *attrs, int len, void *arg) {
	struct dcmd_reply *r = arg;
	ATTRS(a, attrs, len) {
		if (a->nla_type != NL80211_ATTR_VENDOR_DATA) continue;
		ATTRS(b, ADATA(a), ALEN(a)) {
			if (b->nla_type == BRCM_ATTR_DATA && r->len > 0) {
				int n = ALEN(b) < r->len ? ALEN(b) : r->len;
				memcpy(r->buf, ADATA(b), n);
				r->buf = (char *)r->buf + n;
				r->len -= n;
			}
		}
	}
}

/* one firmware ioctl: buf holds len bytes in, and what comes back */
static int dcmd(uint32_t cmd, void *buf, int len, bool set) {
	if (nlfd < 0 || family < 0 || !find_if() || len > 1024) return -ENODEV;
	struct { uint32_t cmd; int32_t len; uint32_t offset, set, magic; } h = { cmd, len, 20, set, 0 };
	static char req[2048] __attribute__((aligned(4)));
	char data[20 + 1024];
	struct nlmsghdr *n = start(req, family, NL80211_CMD_VENDOR);
	uint32_t oui = BRCM_OUI, sub = BRCM_DCMD;
	memcpy(data, &h, 20);
	memcpy(data + 20, buf, len);
	put(n, NL80211_ATTR_IFINDEX, &ifindex, 4);
	put(n, NL80211_ATTR_VENDOR_ID, &oui, 4);
	put(n, NL80211_ATTR_VENDOR_SUBCMD, &sub, 4);
	put(n, NL80211_ATTR_VENDOR_DATA, data, 20 + len);
	struct dcmd_reply r = { buf, set ? 0 : len };
	return request(n, got_dcmd, &r);
}

static int get_int(uint32_t cmd, int *v) {
	return dcmd(cmd, v, sizeof(*v), false);
}

static int var_get(const char *name, int *v) {
	char b[64] = "";
	snprintf(b, sizeof(b), "%s", name);
	int r = dcmd(WLC_GET_VAR, b, sizeof(b), false);
	if (r == 0) memcpy(v, b, sizeof(*v));
	return r;
}

static int var_set(const char *name, const void *d, int len) {
	char b[256];
	int n = strlen(name) + 1;
	if (n + len > (int)sizeof(b)) return -EINVAL;
	memcpy(b, name, n);
	memcpy(b + n, d, len);
	return dcmd(WLC_SET_VAR, b, n + len, true);
}

static int var_set_int(const char *name, int v) {
	return var_set(name, &v, sizeof(v));
}

/* a pattern filter: frames where (frame[offset + i] & mask[i]) == pattern[i] */
static int filter_add(uint32_t id, uint32_t offset, const uint8_t *mask, const uint8_t *pattern, uint32_t size) {
	uint8_t b[20 + 2 * 64];
	uint32_t h[5] = { id, 0 /* pattern */, 0 /* no negation */, offset, size };
	if (size > 64) return -EINVAL;
	memcpy(b, h, 20);
	memcpy(b + 20, mask, size);
	memcpy(b + 20 + size, pattern, size);
	return var_set("pkt_filter_add", b, 20 + 2 * size);
}

static void filter_enable(uint32_t id, bool on) {
	uint32_t e[2] = { id, on };
	var_set("pkt_filter_enable", e, sizeof(e));
}

/* bytes from the frame's ethertype on (offset 12), set in mask and pattern */
static int filter_from_type(uint32_t id, const int (*bytes)[2], int n, int size) {
	uint8_t mask[64] = { 0 }, pattern[64] = { 0 };
	for (int i = 0; i < n; i++) {
		mask[bytes[i][0]] = 0xff;
		pattern[bytes[i][0]] = bytes[i][1];
	}
	return filter_add(id, 12, mask, pattern, size);
}

static int filters_on(void) {
	/* everything that matches one goes up; the rest stops in the chip */
	static const uint8_t m1[1] = { 0x01 }, p1[1] = { 0x00 };
	/* IPv6 (86dd), next header ICMPv6 (58) at +8, its type at +42 */
	static const int ra[][2] = { { 0, 0x86 }, { 1, 0xdd }, { 8, 58 }, { 42, 134 } };
	static const int ns[][2] = { { 0, 0x86 }, { 1, 0xdd }, { 8, 58 }, { 42, 135 } };
	/* IPv4 (0800), UDP (17) at +11, destination port 68 at +24 (no IP options) */
	static const int dhcp[][2] = { { 0, 0x08 }, { 1, 0x00 }, { 11, 17 }, { 24, 0 }, { 25, 68 } };
	for (int id = F_UNICAST; id < F_END; id++) var_set_int("pkt_filter_delete", id);
	int r = filter_add(F_UNICAST, 0, m1, p1, 1);
	if (!r) r = filter_from_type(F_RA, ra, 4, 43);
	if (!r) r = filter_from_type(F_NS, ns, 4, 43);
	if (!r) r = filter_from_type(F_DHCP, dhcp, 5, 26);
	if (!r) r = var_set_int("pkt_filter_mode", 1);	/* forward on match */
	if (r) {
		for (int id = F_UNICAST; id < F_END; id++) var_set_int("pkt_filter_delete", id);
		return r;
	}
	for (int id = F_UNICAST; id < F_END; id++) filter_enable(id, true);
	return 0;
}

static void filters_off(void) {
	for (int id = F_UNICAST; id < F_END; id++) {
		filter_enable(id, false);
		var_set_int("pkt_filter_delete", id);
	}
}

/* frames the unicast filter kept from the host */
static unsigned long long filtered(void) {
	char b[64] = "pkt_filter_stats";
	uint32_t id = F_UNICAST;
	memcpy(b + 17, &id, 4);
	if (dcmd(WLC_GET_VAR, b, sizeof(b), false)) return 0;
	uint32_t s[3];		/* matched, forwarded, discarded */
	memcpy(s, b, sizeof(s));
	return s[2];
}

/* ---- what the link is --------------------------------------------------- */

struct link { int channel, rssi, pm, dtim, beacon; };
static void link_read(struct link *l) {
	int ch[3] = { 0 };
	memset(l, 0, sizeof(*l));
	if (!dcmd(WLC_GET_CHANNEL, ch, sizeof(ch), false)) l->channel = ch[1] ? ch[1] : ch[0];
	get_int(WLC_GET_RSSI, &l->rssi);
	l->pm = -1;
	get_int(WLC_GET_PM, &l->pm);
	get_int(WLC_GET_DTIMPRD, &l->dtim);
	get_int(WLC_GET_BCNPRD, &l->beacon);
}

static const char *band(int ch) {
	return ch <= 0 ? "?" : ch <= 14 ? "2.4 GHz" : "5 GHz";
}

static const char *pm_name(int pm) {
	return pm == 0 ? "off" : pm == 1 ? "max" : pm == 2 ? "fast" : "?";
}

static const char *reason(int r) {
	switch (r) {
	case 1: return "unspecified";
	case 2: return "previous authentication no longer valid";
	case 3: return "deauthenticated: station leaving";
	case 4: return "inactivity";
	case 5: return "AP full";
	case 6: return "class 2 frame from an unauthenticated station";
	case 7: return "class 3 frame from an unassociated station";
	case 8: return "disassociated: station leaving";
	case 14: return "MIC failure";
	case 15: return "4-way handshake timeout";
	case 16: return "group key handshake timeout";
	case 23: return "802.1X authentication failed";
	case 34: return "too many lost frames";
	default: return "see IEEE 802.11 reason codes";
	}
}

static const char *mac(const unsigned char *m) {
	static char s[18];
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return s;
}

static unsigned long long ifstat(const char *what) {
	char path[128];
	unsigned long long v = 0;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, what);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%llu", &v) != 1) v = 0;
		fclose(f);
	}
	return v;
}

/* ---- screen off, screen on ---------------------------------------------- */

/* how many DTIMs to sleep through: the most whose wait stays under
 * wifi-listen ms and inside the announced listen interval */
static int skip_for(const struct link *l, int listen) {
	if (l->dtim <= 0 || l->beacon <= 0 || wlpm_listen_ms <= 0) return 0;
	long dtim_us = (long)l->dtim * l->beacon * 1024;
	int n = (int)((long)wlpm_listen_ms * 1000 / dtim_us);
	if (listen > 0 && n * l->dtim > listen) n = listen / l->dtim;
	return n > 1 ? n : 0;	/* 0: every DTIM, the firmware's default */
}

static void put_back(bool say) {
	if (!applied) return;
	unsigned long long dropped = filtering ? filtered() : 0;
	int ret = saved_sleep_ret > 0 ? saved_sleep_ret : ON_SLEEP_RET;
	var_set_int("bcn_li_dtim", 0);
	var_set_int("pm2_sleep_ret", ret);
	if (filtering) filters_off();
	if (say)
		fprintf(stderr, "%s: Wi-Fi screen-on: every DTIM, awake %d ms after traffic%s (%llu frames filtered while off)\n", wlpm_name,
			ret, filtering ? ", filter off" : "", dropped);
	applied = filtering = false;
	skip = 0;
	saved_sleep_ret = -1;
}

static void apply_off(const char *why) {
	if (!find_if()) return;
	struct link l;
	int listen = 0, ret = -1;
	link_read(&l);
	var_get("assoc_listen", &listen);
	if (!applied && !var_get("pm2_sleep_ret", &ret) && ret != OFF_SLEEP_RET) saved_sleep_ret = ret;
	skip = skip_for(&l, listen);
	int r = var_set_int("bcn_li_dtim", skip);
	if (!r) r = var_set_int("pm2_sleep_ret", OFF_SLEEP_RET);
	if (r) {
		/* down, or the radio off: the next connect brings them */
		if (!warned)
			fprintf(stderr, "%s: Wi-Fi screen-off settings not set (%s): %s\n", wlpm_name, ifname, strerror(-r));
		warned = true;
		return;
	}
	warned = false;
	bool was = filtering;
	if (wlpm_filter_enabled && !filtering) {
		int f = filters_on();
		filtering = f == 0;
		if (f) fprintf(stderr, "%s: Wi-Fi packet filter not set: %s\n", wlpm_name, strerror(-f));
	} else if (!wlpm_filter_enabled && filtering) {
		filters_off();
		filtering = false;
	}
	applied = true;
	char wait[64] = "every DTIM";
	if (skip)
		snprintf(wait, sizeof(wait), "every %d DTIMs (DTIM %d x %d TU: %ld ms)", skip, l.dtim, l.beacon,
			 (long)skip * l.dtim * l.beacon * 1024 / 1000);
	fprintf(stderr, "%s: Wi-Fi screen-off (%s): listen %s, awake %d ms after traffic, PM %s, %s\n", wlpm_name,
		why, wait, OFF_SLEEP_RET, pm_name(l.pm),
		filtering ? (was ? "filter kept" : "broadcast/multicast filtered in the chip (unicast, RA, NS, DHCP pass)")
			  : "no filter");
}

void wlpm_screen(bool on) {
	static bool started;
	if (!started && on) {
		/* a daemon before this one may have stopped with the screen
		 * off and left its settings in the chip */
		int dtim = 0, ret = 0;
		if (!var_get("bcn_li_dtim", &dtim) && !var_get("pm2_sleep_ret", &ret) &&
		    (dtim != 0 || ret == OFF_SLEEP_RET)) {
			applied = filtering = true;
			put_back(true);
		}
	}
	started = true;
	if (on == screen_on && (on || applied || !wlpm_suspend_enabled)) return;
	screen_on = on;
	if (on) {
		put_back(true);
		next_line = 0;
		return;
	}
	off_since = now_ms();
	next_line = off_since + LINE_MS;
	last_rx = ifstat("rx_packets");
	last_tx = ifstat("tx_packets");
	last_drop = 0;
	if (wlpm_suspend_enabled) apply_off("screen off");
}

void wlpm_reconfigure(void) {
	if (screen_on) return;
	if (!wlpm_suspend_enabled) {
		put_back(true);
		return;
	}
	apply_off("settings changed");
}

/* ---- events ------------------------------------------------------------- */

int wlpm_open(void) {
	nlfd = sock();
	if (nlfd < 0) return -1;
	struct timeval tv = { .tv_sec = 4 };	/* brcmfmac's ioctl timeout is 2 s */
	setsockopt(nlfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	char buf[256] __attribute__((aligned(4)));
	struct nlmsghdr *n = start(buf, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
	((struct genlmsghdr *)NLMSG_DATA(n))->version = 1;
	put(n, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	struct family_reply r = { -1, -1 };
	if (request(n, got_family, &r) || r.id < 0) {
		fprintf(stderr, "%s: no nl80211: Wi-Fi screen-off settings off\n", wlpm_name);
		return -1;
	}
	family = r.id;
	evfd = sock();
	if (evfd >= 0 && (r.mlme < 0 ||
	    setsockopt(evfd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &r.mlme, sizeof(r.mlme)) < 0)) {
		close(evfd);
		evfd = -1;
	}
	if (evfd >= 0) {
		int fl = 1;
		setsockopt(evfd, SOL_NETLINK, NETLINK_NO_ENOBUFS, &fl, sizeof(fl));
	}
	find_if();
	return evfd;
}

static void link_event(int cmd, struct nlattr *attrs, int len) {
	unsigned idx = 0;
	const unsigned char *m = NULL;
	int code = -1;
	bool by_ap = false;
	ATTRS(a, attrs, len) {
		if (a->nla_type == NL80211_ATTR_IFINDEX) idx = *(uint32_t *)ADATA(a);
		if (a->nla_type == NL80211_ATTR_MAC && ALEN(a) >= 6) m = ADATA(a);
		if (a->nla_type == NL80211_ATTR_STATUS_CODE || a->nla_type == NL80211_ATTR_REASON_CODE)
			code = *(uint16_t *)ADATA(a);
		if (a->nla_type == NL80211_ATTR_DISCONNECTED_BY_AP) by_ap = true;
	}
	if (!find_if() || idx != ifindex) return;
	char off[48] = "";
	if (!screen_on) snprintf(off, sizeof(off), ", screen off %ld min", (now_ms() - off_since) / 60000);
	if (cmd == NL80211_CMD_DISCONNECT) {
		fprintf(stderr, "%s: Wi-Fi: disconnected from %s, reason %d (%s), %s%s\n", wlpm_name,
			have_bssid ? mac(bssid) : "?", code, reason(code),
			by_ap ? "by the AP" : "by this phone", off);
		have_bssid = false;	/* the settings stay in the chip for the next link */
		return;
	}
	if (cmd == NL80211_CMD_CONNECT && code != 0) {
		fprintf(stderr, "%s: Wi-Fi: connecting to %s failed, status %d%s\n", wlpm_name, m ? mac(m) : "?", code, off);
		return;
	}
	struct link l;
	link_read(&l);
	if (m) {
		memcpy(bssid, m, 6);
		have_bssid = true;
	}
	fprintf(stderr, "%s: Wi-Fi: %s %s (%s ch %d, %d dBm, DTIM %d)%s\n", wlpm_name,
		cmd == NL80211_CMD_ROAM ? "roamed to" : "connected to", have_bssid ? mac(bssid) : "?",
		band(l.channel), l.channel, l.rssi, l.dtim, off);
	if (!screen_on && wlpm_suspend_enabled) {
		/* a new BSS has its own DTIM; NetworkManager sets power save again */
		apply_off(cmd == NL80211_CMD_ROAM ? "roam" : "connect");
		reapply_at = now_ms() + REAPPLY_MS;
	}
}

void wlpm_event(void) {
	static char buf[8192] __attribute__((aligned(4)));
	ssize_t len;
	while (evfd >= 0 && (len = recv(evfd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		for (struct nlmsghdr *m = (struct nlmsghdr *)buf; NLMSG_OK(m, (size_t)len); m = NLMSG_NEXT(m, len)) {
			if (m->nlmsg_type != family) continue;
			int cmd = ((struct genlmsghdr *)NLMSG_DATA(m))->cmd;
			if (cmd == NL80211_CMD_CONNECT || cmd == NL80211_CMD_ROAM || cmd == NL80211_CMD_DISCONNECT)
				link_event(cmd, (struct nlattr *)((char *)NLMSG_DATA(m) + GENL_HDRLEN),
					   m->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN);
		}
	}
}

int wlpm_timeout(void) {
	long due = 0, now = now_ms();
	if (reapply_at) due = reapply_at;
	if (next_line && (!due || next_line < due)) due = next_line;
	if (!due) return -1;
	return due <= now ? 0 : (int)(due - now);
}

void wlpm_tick(void) {
	long now = now_ms();
	if (reapply_at && now >= reapply_at) {
		reapply_at = 0;
		int ret = 0;
		/* NetworkManager switches power save on after a connect: brcmfmac
		 * puts pm2_sleep_ret back to 2000 then */
		if (!screen_on && wlpm_suspend_enabled && !var_get("pm2_sleep_ret", &ret) && ret != OFF_SLEEP_RET) {
			var_set_int("pm2_sleep_ret", OFF_SLEEP_RET);
			fprintf(stderr, "%s: Wi-Fi: awake %d ms after traffic again, not %d (power save set after the connect)\n", wlpm_name,
				OFF_SLEEP_RET, ret);
		}
	}
	if (!next_line || now < next_line) return;
	next_line = now + LINE_MS;
	if (screen_on || !find_if()) return;
	struct link l;
	link_read(&l);
	int dtim = -1, ret = -1;
	var_get("bcn_li_dtim", &dtim);
	var_get("pm2_sleep_ret", &ret);
	unsigned long long rx = ifstat("rx_packets"), tx = ifstat("tx_packets"), drop = filtering ? filtered() : 0;
	fprintf(stderr, "%s: Wi-Fi, screen off %ld min: %s %s ch %d %d dBm, PM %s, DTIM %d listen %d, "
		"awake %d ms; 5 min: rx %llu tx %llu frames, %llu filtered\n", wlpm_name,
		(now - off_since) / 60000, have_bssid ? mac(bssid) : "not connected", band(l.channel), l.channel,
		l.rssi, pm_name(l.pm), l.dtim, dtim, ret, rx - last_rx, tx - last_tx,
		drop >= last_drop ? drop - last_drop : drop);
	last_rx = rx;
	last_tx = tx;
	last_drop = drop;
	/* the firmware may have been restarted, or something set them back */
	if (wlpm_suspend_enabled && applied && (dtim != skip || ret != OFF_SLEEP_RET)) {
		fprintf(stderr, "%s: Wi-Fi screen-off settings were reset (DTIM listen %d, awake %d ms)\n", wlpm_name, dtim, ret);
		applied = filtering = false;
		apply_off("reset");
	}
}

void wlpm_status(FILE *f) {
	fprintf(f, "wifi_suspend=%s\nwifi_filter=%s\nwifi_listen_ms=%d\n", wlpm_suspend_enabled ? "on" : "off",
		wlpm_filter_enabled ? "on" : "off", wlpm_listen_ms);
	fprintf(f, "wifi_screen_off_settings=%s\nwifi_skip_dtim=%d\nwifi_filtering=%s\n", applied ? "on" : "off",
		skip, filtering ? "on" : "off");
}

/* for ztprofile: the link and the chip's filter counters as they are now */
int wlpm_read(struct wlpm_info *i) {
	memset(i, 0, sizeof(*i));
	if (family < 0 || !find_if()) return -ENODEV;
	struct link l;
	link_read(&l);
	i->channel = l.channel;
	i->rssi = l.rssi;
	i->pm = l.pm;
	unsigned char b[6] = { 0 };
	if (!dcmd(23 /* WLC_GET_BSSID */, b, 6, false)) memcpy(i->bssid, b, 6);
	char s[64] = "pkt_filter_stats";
	uint32_t id = F_UNICAST, v[3];
	memcpy(s + 17, &id, 4);
	if (!dcmd(WLC_GET_VAR, s, sizeof(s), false)) {
		memcpy(v, s, sizeof(v));	/* matched, forwarded, discarded */
		i->filter_on = true;
		i->filter_matched = v[0];
		i->filter_discarded = v[2];
	}
	return 0;
}
