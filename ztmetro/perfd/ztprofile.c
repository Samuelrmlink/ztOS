// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztprofile: an opt-in Wi-Fi and network profiler, for what the screen-off draw
 * and dropped connections come from. Off unless started: the service then sits
 * in read() on its control pipe, with no timer and no capture.
 *
 *	ztprofile start [MINUTES]	start a session (60 min by default, at most
 *					24 h); it stops itself at the end
 *	ztprofile stop			end it now (everything is written out)
 *	ztprofile status		/run/ztprofile.status, as it is
 *	ztprofile report [--json] [SESSION]
 *					a summary of the running session, or the
 *					newest one, or the one named
 *	ztprofile list [--json]		the sessions kept, newest first
 *	ztprofile delete SESSION	remove one
 *	ztprofile service		the root part (ztprofile.service)
 *
 * Who may drive it: start, stop and delete are lines written to the control
 * pipe /run/ztprofile.ctl ("start MINUTES", "stop", "delete SESSION"), which
 * anyone may write, as zrnperfd's: Settings, running as the user, needs no
 * polkit rule, no sudo, and the root part stays one long-lived, idle process.
 * status, report and list only read files anyone may read.
 *
 * A session samples, every 15 s: the Wi-Fi interface's counters, the Wi-Fi
 * chip's interrupt count, the processors' idle entries and residency,
 * interrupts and context switches, the battery's current and the charger's
 * ISYS/IIN, the screen, the BSSID and signal, and the chip's own packet filter
 * counters when zrnperfd's screen-off filter is in. It counts the frames that
 * reach the phone, by kind (unicast, broadcast, multicast; ARP, mDNS, SSDP,
 * LLMNR, NetBIOS, DHCP, IPv6 RA/NS/NA/MLD, TCP/UDP by local port) and by
 * sender, a minute at a time; and every 3 min it lists the open TCP/UDP
 * sockets with the process that owns them. Frames the chip's filter drops
 * never reach the host: the filter's own counter says how many.
 *
 * Privacy: only header fields are read (MAC and IP addresses, ports, protocol);
 * the socket keeps the first 128 bytes of a frame in the kernel's ring and the
 * payload is never looked at, copied or stored. Counts are kept in /run
 * (RAM) and written to /var/log/ztprofile/SESSION every 15 min, when the screen
 * comes on, and at the end; the sessions there are kept under 32 MB, oldest
 * removed first. Nothing leaves the phone.
 *
 * Its own cost: one wake per 15 s for the sample and at most one every 2 s for
 * frames (a TPACKET_V3 ring that hands over a block of frames at a time). The
 * report shows the profiler's own CPU time and wakes.
 *
 * Files (all text, for Settings):
 *   /run/ztprofile.status	key=value: running=yes|no, session, dir, log_dir,
 *				started, ends, minutes_left (epoch seconds), and the
 *				latest: sample_t, screen, bssid, rssi, bat_ma,
 *				interrupts_per_min, idle_entries_per_min,
 *				wifi_irq_per_min, frames_per_min and fpm_KIND for
 *				each kind (the last whole minute); last_session
 *   /var/log/ztprofile/SESSION/summary.json
 *				what report --json prints (written with each flush)
 *   .../samples.csv traffic.csv talkers.csv ports.csv sockets.csv session log
 *				the raw counts, CSV with a header line
 */
#define _GNU_SOURCE
#include "wlpm.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RUN_DIR		"/run/ztprofile"
#define LOG_DIR		"/var/log/ztprofile"
#define CTL		"/run/ztprofile.ctl"
#define STATUS		"/run/ztprofile.status"
#define CURRENT		RUN_DIR "/current"
#define SAMPLE_S	15
#define SOCKETS_S	180
#define FLUSH_S		900
#define SNAP		128
#define MAX_MIN		1440
#define LOG_CAP		(32 << 20)

static const char *HELP =
"ztprofile: an opt-in Wi-Fi/network profiler (screen-off power, dropped links)\n"
"\n"
"  ztprofile start [MINUTES]          start (60 min by default, at most 1440); stops itself\n"
"  ztprofile stop                     end it now\n"
"  ztprofile status                   " STATUS "\n"
"  ztprofile report [--json] [SESSION] summary of the running, newest or named session\n"
"  ztprofile list [--json]            the sessions kept (at most 32 MB, oldest go first)\n"
"  ztprofile delete SESSION           remove one\n"
"\n"
"Every 15 s: Wi-Fi counters, its interrupts, CPU idle entries, battery current,\n"
"screen, BSSID and signal. Each minute: the frames that reached the phone, by\n"
"kind and by sender. Every 3 min: open TCP/UDP sockets and their processes.\n"
"\n"
"Privacy: header fields only (MAC/IP addresses, ports, protocol). Payloads are\n"
"never read or kept. Everything stays on the phone: " RUN_DIR " (RAM),\n"
LOG_DIR " (every 15 min, screen on, and at the end).\n"
"While it runs it costs a little: one wake per 15 s and at most one per 2 s for\n"
"frames; the report shows its own CPU time. Off, it costs nothing.\n";

/* the latest values, for the status file */
static struct {
	long t, ma;
	int screen, rssi;
	char bssid[18];
	double intr_min, idle_min, irq_min;
	unsigned long frames, d[3], k[18];
} live;
static int wlpm_fd = -1;

/* ---- frame kinds --------------------------------------------------------- */

enum { D_UCAST, D_BCAST, D_MCAST, D_N };
static const char *dnames[D_N] = { "unicast", "broadcast", "multicast" };
enum { K_ARP, K_MDNS, K_SSDP, K_LLMNR, K_NETBIOS, K_DHCP, K_DHCP6, K_RA, K_RS, K_NS, K_NA, K_MLD,
       K_ICMP6, K_ICMP, K_TCP, K_UDP, K_EAPOL, K_OTHER, K_N };
_Static_assert(K_N == 18 && D_N == 3, "the status file's live counts");
static const char *knames[K_N] = { "arp", "mdns", "ssdp", "llmnr", "netbios", "dhcp", "dhcp6", "ra", "rs",
	"ns", "na", "mld", "icmp6", "icmp", "tcp", "udp", "eapol", "other" };

struct talker { unsigned char mac[6]; char ip[INET6_ADDRSTRLEN]; unsigned long n; };
struct port { int proto, port; unsigned long n; };
#define NTALK 256
#define NPORT 128
static struct {
	unsigned long d[D_N], k[K_N], bytes, frames;
	struct talker t[NTALK];
	int nt;
	struct port p[NPORT];
	int np;
} cur;

static void count_talker(const unsigned char *mac, const char *ip) {
	for (int i = 0; i < cur.nt; i++)
		if (!memcmp(cur.t[i].mac, mac, 6) && !strcmp(cur.t[i].ip, ip)) {
			cur.t[i].n++;
			return;
		}
	if (cur.nt < NTALK) {
		memcpy(cur.t[cur.nt].mac, mac, 6);
		snprintf(cur.t[cur.nt].ip, sizeof(cur.t[0].ip), "%s", ip);
		cur.t[cur.nt++].n = 1;
	}
}

static void count_port(int proto, int port) {
	for (int i = 0; i < cur.np; i++)
		if (cur.p[i].proto == proto && cur.p[i].port == port) {
			cur.p[i].n++;
			return;
		}
	if (cur.np < NPORT) cur.p[cur.np++] = (struct port){ proto, port, 1 };
}

static int udp_kind(int sport, int dport) {
	int a = sport < dport ? sport : dport, b = sport < dport ? dport : sport;
	if (sport == 5353 || dport == 5353) return K_MDNS;
	if (dport == 1900 || sport == 1900) return K_SSDP;
	if (dport == 5355 || sport == 5355) return K_LLMNR;
	if (a == 137 || a == 138 || b == 137 || b == 138) return K_NETBIOS;
	if (dport == 67 || dport == 68) return K_DHCP;
	if (dport == 546 || dport == 547) return K_DHCP6;
	return K_UDP;
}

/* one frame, header only: data holds at most SNAP bytes */
static void classify(const unsigned char *f, unsigned len, unsigned wire, int pkttype) {
	cur.frames++;
	cur.bytes += wire;
	cur.d[pkttype == PACKET_BROADCAST ? D_BCAST : pkttype == PACKET_MULTICAST ? D_MCAST : D_UCAST]++;
	if (len < 14) { cur.k[K_OTHER]++; return; }
	const unsigned char *src = f + 6;
	unsigned off = 12, type = f[12] << 8 | f[13];
	if (type == 0x8100 && len >= 18) { off = 16; type = f[16] << 8 | f[17]; }
	off += 2;
	char ip[INET6_ADDRSTRLEN] = "";
	int kind = K_OTHER, proto = 0, sport = 0, dport = 0;
	if (type == 0x0806) {
		kind = K_ARP;
		if (len >= off + 18) inet_ntop(AF_INET, f + off + 14, ip, sizeof(ip));
	} else if (type == 0x888e) {
		kind = K_EAPOL;
	} else if (type == 0x0800 && len >= off + 20) {
		const unsigned char *h = f + off;
		unsigned ihl = (h[0] & 15) * 4;
		proto = h[9];
		inet_ntop(AF_INET, h + 12, ip, sizeof(ip));
		if (len >= off + ihl + 4 && (proto == 6 || proto == 17)) {
			sport = h[ihl] << 8 | h[ihl + 1];
			dport = h[ihl + 2] << 8 | h[ihl + 3];
		}
		kind = proto == 1 ? K_ICMP : proto == 6 ? K_TCP : proto == 17 ? udp_kind(sport, dport) : K_OTHER;
	} else if (type == 0x86dd && len >= off + 40) {
		const unsigned char *h = f + off;
		unsigned nh = h[6], p = off + 40;
		inet_ntop(AF_INET6, h + 8, ip, sizeof(ip));
		if (nh == 0 && len >= p + 2) { nh = f[p]; p += (f[p + 1] + 1) * 8; }	/* hop-by-hop (MLD) */
		proto = nh;
		if (nh == 58 && len > p) {
			int t = f[p];
			kind = t == 134 ? K_RA : t == 133 ? K_RS : t == 135 ? K_NS : t == 136 ? K_NA :
			       (t == 130 || t == 131 || t == 132 || t == 143) ? K_MLD : K_ICMP6;
		} else if ((nh == 6 || nh == 17) && len >= p + 4) {
			sport = f[p] << 8 | f[p + 1];
			dport = f[p + 2] << 8 | f[p + 3];
			kind = nh == 6 ? K_TCP : udp_kind(sport, dport);
		}
	}
	cur.k[kind]++;
	count_talker(src, ip);
	if (pkttype == PACKET_HOST && (proto == 6 || proto == 17)) count_port(proto, dport);
}

/* ---- the capture: TPACKET_V3, blocks handed over at most every 2 s ------- */

#define BLOCK_SIZE	(1 << 16)
#define BLOCK_NR	8
static int cap = -1;
static unsigned char *ring;
static unsigned block;
static unsigned long ring_drops;

static int capture_open(const char *ifname) {
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (fd < 0) return -1;
	/* only frames coming in, and only their first SNAP bytes */
	struct sock_filter code[] = {
		{ BPF_LD | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF + SKF_AD_PKTTYPE },
		{ BPF_JMP | BPF_JEQ | BPF_K, 1, 0, PACKET_OUTGOING },
		{ BPF_RET | BPF_K, 0, 0, SNAP },
		{ BPF_RET | BPF_K, 0, 0, 0 },
	};
	struct sock_fprog prog = { sizeof(code) / sizeof(code[0]), code };
	int v = TPACKET_V3;
	struct tpacket_req3 req = { .tp_block_size = BLOCK_SIZE, .tp_block_nr = BLOCK_NR, .tp_frame_size = 2048,
		.tp_frame_nr = BLOCK_SIZE / 2048 * BLOCK_NR, .tp_retire_blk_tov = 2000 };
	struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = if_nametoindex(ifname) };
	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0 ||
	    setsockopt(fd, SOL_PACKET, PACKET_VERSION, &v, sizeof(v)) < 0 ||
	    setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0 ||
	    (ring = mmap(NULL, BLOCK_SIZE * BLOCK_NR, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED ||
	    !a.sll_ifindex || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		ring = NULL;
		return -1;
	}
	return fd;
}

static void capture_drain(void) {
	for (;;) {
		struct tpacket_block_desc *b = (void *)(ring + (size_t)block * BLOCK_SIZE);
		if (!(b->hdr.bh1.block_status & TP_STATUS_USER)) break;
		struct tpacket3_hdr *p = (void *)((unsigned char *)b + b->hdr.bh1.offset_to_first_pkt);
		for (unsigned i = 0; i < b->hdr.bh1.num_pkts; i++) {
			struct sockaddr_ll *sll = (void *)((unsigned char *)p + TPACKET_ALIGN(sizeof(*p)));
			classify((unsigned char *)p + p->tp_mac, p->tp_snaplen, p->tp_len, sll->sll_pkttype);
			p = (void *)((unsigned char *)p + p->tp_next_offset);
		}
		b->hdr.bh1.block_status = TP_STATUS_KERNEL;
		block = (block + 1) % BLOCK_NR;
	}
	struct tpacket_stats_v3 st;
	socklen_t sl = sizeof(st);
	if (getsockopt(cap, SOL_PACKET, PACKET_STATISTICS, &st, &sl) == 0) ring_drops += st.tp_drops;
}

/* ---- the samples --------------------------------------------------------- */

static char ifname[IF_NAMESIZE] = "wlp1s0";
static char dir[256], logdir[256];

static unsigned long long read_ull(const char *path) {
	FILE *f = fopen(path, "r");
	unsigned long long v = 0;
	if (f) {
		if (fscanf(f, "%llu", &v) != 1) v = 0;
		fclose(f);
	}
	return v;
}

static long read_long(const char *path, long dflt) {
	FILE *f = fopen(path, "r");
	long v = dflt;
	if (f) {
		if (fscanf(f, "%ld", &v) != 1) v = dflt;
		fclose(f);
	}
	return v;
}

static unsigned long long netstat(const char *what) {
	char p[128];
	snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/%s", ifname, what);
	return read_ull(p);
}

/* the Wi-Fi chip's interrupts: the /proc/interrupts lines naming brcmf or the
 * PCIe MSI it sits on */
static unsigned long long wifi_irqs(void) {
	FILE *f = fopen("/proc/interrupts", "r");
	char line[1024];
	unsigned long long sum = 0;
	while (f && fgets(line, sizeof(line), f)) {
		if (!strstr(line, "brcmf") && !strstr(line, "PCI-MSI") && !strstr(line, "pcie")) continue;
		char *p = strchr(line, ':');
		if (!p) continue;
		for (p++;;) {
			while (*p == ' ') p++;
			if (!isdigit((unsigned char)*p)) break;
			sum += strtoull(p, &p, 10);
		}
	}
	if (f) fclose(f);
	return sum;
}

static void proc_stat(unsigned long long *intr, unsigned long long *ctxt) {
	FILE *f = fopen("/proc/stat", "r");
	char line[256];
	*intr = *ctxt = 0;
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "intr ", 5)) *intr = strtoull(line + 5, NULL, 10);
		if (!strncmp(line, "ctxt ", 5)) *ctxt = strtoull(line + 5, NULL, 10);
	}
	if (f) fclose(f);
}

/* idle entries (wakes, more or less) and idle time over all CPUs and states */
static void cpuidle(unsigned long long *entries, unsigned long long *us, int *cpus) {
	*entries = *us = 0;
	*cpus = 0;
	for (int c = 0; c < 64; c++) {
		char p[128];
		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/online", c);
		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d", c);
		if (access(p, F_OK)) break;
		(*cpus)++;
		for (int s = 0; s < 16; s++) {
			snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/usage", c, s);
			if (access(p, F_OK)) break;
			*entries += read_ull(p);
			snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time", c, s);
			*us += read_ull(p);
		}
	}
}

static char bat[128], isys[128], iin[128];
static void find_power(void) {
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[300], t[32] = "";
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (!fgets(t, sizeof(t), f)) t[0] = 0;
			fclose(f);
		}
		if (!strncmp(t, "Battery", 7) && !bat[0])
			snprintf(bat, sizeof(bat), "/sys/class/power_supply/%s/current_now", e->d_name);
	}
	if (d) closedir(d);
	d = opendir("/sys/class/hwmon");
	while (d && (e = readdir(d))) {
		char p[300], n[64] = "";
		snprintf(p, sizeof(p), "/sys/class/hwmon/%s/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (!fgets(n, sizeof(n), f)) n[0] = 0;
			fclose(f);
		}
		if (strcmp(n, "max77705\n")) continue;
		for (int i = 1; i <= 4; i++) {
			char l[32] = "";
			snprintf(p, sizeof(p), "/sys/class/hwmon/%s/curr%d_label", e->d_name, i);
			f = fopen(p, "r");
			if (!f) continue;
			if (!fgets(l, sizeof(l), f)) l[0] = 0;
			fclose(f);
			snprintf(p, sizeof(p), "/sys/class/hwmon/%s/curr%d_input", e->d_name, i);
			if (!strncmp(l, "ISYS", 4)) snprintf(isys, sizeof(isys), "%s", p);
			if (!strncmp(l, "IIN", 3)) snprintf(iin, sizeof(iin), "%s", p);
		}
	}
	if (d) closedir(d);
}

static int screen_state(void) {
	FILE *f = fopen("/run/zrnperfd.status", "r");
	char line[128];
	int on = -1;
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "screen=", 7)) on = strncmp(line + 7, "off", 3) != 0;
	if (f) fclose(f);
	return on;
}

static unsigned long long self_cpu_ms(void) {
	FILE *f = fopen("/proc/self/stat", "r");
	char b[1024];
	unsigned long long u = 0, s = 0;
	if (f && fgets(b, sizeof(b), f)) {
		char *p = strrchr(b, ')');
		/* fields after the comm: state is 3, utime 14, stime 15 */
		if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &u, &s) != 2) u = s = 0;
	}
	if (f) fclose(f);
	return (u + s) * 1000 / sysconf(_SC_CLK_TCK);
}

static FILE *open_run(const char *name, const char *header) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	bool fresh = access(p, F_OK) != 0;
	FILE *f = fopen(p, "a");
	if (f && fresh) fprintf(f, "%s\n", header);
	return f;
}

static unsigned long wakes;	/* the profiler's own loop turns */
static bool first = true;	/* a session's first sample: only the base */

static void sample(time_t now) {
	static unsigned long long p_rx, p_tx, p_rxb, p_txb, p_mc, p_rd, p_td, p_re, p_te, p_irq, p_intr, p_ctxt,
		p_ie, p_ius, p_fd, p_cpu;
	static unsigned long p_wakes;
	static long p_t;
	unsigned long long rx = netstat("rx_packets"), tx = netstat("tx_packets"), rxb = netstat("rx_bytes"),
		txb = netstat("tx_bytes"), mc = netstat("multicast"), rd = netstat("rx_dropped"),
		td = netstat("tx_dropped"), re = netstat("rx_errors"), te = netstat("tx_errors"), irq = wifi_irqs(),
		intr, ctxt, ie, ius, cpu = self_cpu_ms();
	int cpus;
	proc_stat(&intr, &ctxt);
	cpuidle(&ie, &ius, &cpus);
	struct wlpm_info w;
	int wr = wlpm_read(&w);
	long b = bat[0] ? read_long(bat, 0) : 0, y = isys[0] ? read_long(isys, -1) : -1, n = iin[0] ? read_long(iin, -1) : -1;
	if (!first) {
		FILE *f = open_run("samples.csv",
			"t,dt,screen,bssid,channel,rssi,pm,rx,tx,rx_bytes,tx_bytes,multicast,rx_drop,tx_drop,rx_err,tx_err,"
			"wifi_irq,intr,ctxt,idle_entries,idle_pct,filter_on,filter_discarded,bat_ma,isys_ma,iin_ma,"
			"self_cpu_ms,self_wakes,ring_drops");
		long dt = now - p_t;
		char bss[18] = "-";
		if (!wr && (w.bssid[0] | w.bssid[1] | w.bssid[2] | w.bssid[3] | w.bssid[4] | w.bssid[5]))
			snprintf(bss, sizeof(bss), "%02x:%02x:%02x:%02x:%02x:%02x", w.bssid[0], w.bssid[1], w.bssid[2],
				 w.bssid[3], w.bssid[4], w.bssid[5]);
		double idle = dt > 0 && cpus ? 100.0 * (ius - p_ius) / (dt * 1e6 * cpus) : 0;
		live.t = now;
		live.ma = -b / 1000;
		live.screen = screen_state();
		live.rssi = w.rssi;
		snprintf(live.bssid, sizeof(live.bssid), "%s", bss);
		if (dt > 0) {
			live.intr_min = (intr - p_intr) * 60.0 / dt;
			live.idle_min = (ie - p_ie) * 60.0 / dt;
			live.irq_min = (irq - p_irq) * 60.0 / dt;
		}
		if (f) {
			fprintf(f, "%ld,%ld,%d,%s,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,"
				"%d,%llu,%ld,%ld,%ld,%llu,%lu,%lu\n",
				(long)now, dt, live.screen, bss, w.channel, w.rssi, w.pm, rx - p_rx, tx - p_tx, rxb - p_rxb,
				txb - p_txb, mc - p_mc, rd - p_rd, td - p_td, re - p_re, te - p_te, irq - p_irq,
				intr - p_intr, ctxt - p_ctxt, ie - p_ie, idle, w.filter_on,
				w.filter_on && w.filter_discarded >= p_fd ? w.filter_discarded - p_fd : 0,
				-b / 1000, y, n, cpu - p_cpu, wakes - p_wakes, ring_drops);
			fclose(f);
		}
	}
	first = false;
	p_rx = rx; p_tx = tx; p_rxb = rxb; p_txb = txb; p_mc = mc; p_rd = rd; p_td = td; p_re = re; p_te = te;
	p_irq = irq; p_intr = intr; p_ctxt = ctxt; p_ie = ie; p_ius = ius; p_fd = w.filter_discarded; p_cpu = cpu;
	p_wakes = wakes; p_t = now;
}

static int cmp_talk(const void *a, const void *b) {
	unsigned long x = ((const struct talker *)a)->n, y = ((const struct talker *)b)->n;
	return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_port(const void *a, const void *b) {
	unsigned long x = ((const struct port *)a)->n, y = ((const struct port *)b)->n;
	return x < y ? 1 : x > y ? -1 : 0;
}

/* the minute's frame counts, its top 10 senders and local ports */
static void minute(time_t m) {
	FILE *f = open_run("traffic.csv",
		"minute,frames,bytes,unicast,broadcast,multicast,arp,mdns,ssdp,llmnr,netbios,dhcp,dhcp6,ra,rs,ns,na,mld,"
		"icmp6,icmp,tcp,udp,eapol,other");
	if (f) {
		fprintf(f, "%ld,%lu,%lu", (long)m, cur.frames, cur.bytes);
		for (int i = 0; i < D_N; i++) fprintf(f, ",%lu", cur.d[i]);
		for (int i = 0; i < K_N; i++) fprintf(f, ",%lu", cur.k[i]);
		fputc('\n', f);
		fclose(f);
	}
	qsort(cur.t, cur.nt, sizeof(cur.t[0]), cmp_talk);
	f = cur.nt ? open_run("talkers.csv", "minute,mac,ip,frames") : NULL;
	for (int i = 0; f && i < cur.nt && i < 10; i++)
		fprintf(f, "%ld,%02x:%02x:%02x:%02x:%02x:%02x,%s,%lu\n", (long)m, cur.t[i].mac[0], cur.t[i].mac[1],
			cur.t[i].mac[2], cur.t[i].mac[3], cur.t[i].mac[4], cur.t[i].mac[5],
			cur.t[i].ip[0] ? cur.t[i].ip : "-", cur.t[i].n);
	if (f) fclose(f);
	qsort(cur.p, cur.np, sizeof(cur.p[0]), cmp_port);
	f = cur.np ? open_run("ports.csv", "minute,proto,port,frames") : NULL;
	for (int i = 0; f && i < cur.np && i < 10; i++)
		fprintf(f, "%ld,%s,%d,%lu\n", (long)m, cur.p[i].proto == 6 ? "tcp" : "udp", cur.p[i].port, cur.p[i].n);
	if (f) fclose(f);
	live.frames = cur.frames;
	memcpy(live.d, cur.d, sizeof(live.d));
	memcpy(live.k, cur.k, sizeof(live.k));
	memset(&cur, 0, sizeof(cur));
}

/* ---- sockets ------------------------------------------------------------- */

struct owner { unsigned long inode; int pid; };
static struct owner *owners;
static int nowners, cap_owners;

static void scan_owners(void) {
	nowners = 0;
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char p[300];
		snprintf(p, sizeof(p), "/proc/%s/fd", e->d_name);
		DIR *fd = opendir(p);
		struct dirent *g;
		while (fd && (g = readdir(fd))) {
			char l[300], t[64];
			snprintf(l, sizeof(l), "/proc/%s/fd/%s", e->d_name, g->d_name);
			ssize_t n = readlink(l, t, sizeof(t) - 1);
			if (n < 9) continue;
			t[n] = 0;
			if (strncmp(t, "socket:[", 8)) continue;
			if (nowners == cap_owners) {
				cap_owners = cap_owners ? cap_owners * 2 : 256;
				owners = realloc(owners, cap_owners * sizeof(*owners));
				if (!owners) { nowners = cap_owners = 0; break; }
			}
			owners[nowners++] = (struct owner){ strtoul(t + 8, NULL, 10), atoi(e->d_name) };
		}
		if (fd) closedir(fd);
	}
	if (d) closedir(d);
}

static int owner_of(unsigned long inode) {
	for (int i = 0; i < nowners; i++)
		if (owners[i].inode == inode) return owners[i].pid;
	return 0;
}

static void addr(const char *hex, bool v6, char *out, size_t len) {
	char a[64];
	unsigned port = 0;
	sscanf(hex, "%63[0-9A-Fa-f]:%x", a, &port);
	unsigned char b[16] = { 0 };
	for (int i = 0; i < (v6 ? 16 : 4); i++) {
		unsigned x;
		sscanf(a + i * 2, "%2x", &x);
		b[i] = x;
	}
	/* /proc/net shows each 32-bit word in host (little-endian) order */
	for (int w = 0; w < (v6 ? 4 : 1); w++) {
		unsigned char t;
		t = b[w * 4]; b[w * 4] = b[w * 4 + 3]; b[w * 4 + 3] = t;
		t = b[w * 4 + 1]; b[w * 4 + 1] = b[w * 4 + 2]; b[w * 4 + 2] = t;
	}
	char ip[INET6_ADDRSTRLEN];
	inet_ntop(v6 ? AF_INET6 : AF_INET, b, ip, sizeof(ip));
	snprintf(out, len, v6 ? "[%s]:%u" : "%s:%u", ip, port);
}

static const char *tcp_state(int s) {
	static const char *n[] = { "?", "established", "syn-sent", "syn-recv", "fin-wait1", "fin-wait2", "time-wait",
		"close", "close-wait", "last-ack", "listen", "closing" };
	return s > 0 && s < 12 ? n[s] : "?";
}

static void sockets(time_t now) {
	static const char *files[] = { "/proc/net/tcp", "/proc/net/tcp6", "/proc/net/udp", "/proc/net/udp6" };
	scan_owners();
	FILE *out = open_run("sockets.csv", "t,proto,state,local,remote,pid,process");
	for (int i = 0; out && i < 4; i++) {
		FILE *f = fopen(files[i], "r");
		char line[512];
		bool v6 = i & 1, tcp = i < 2;
		if (f && !fgets(line, sizeof(line), f)) line[0] = 0;	/* the header */
		while (f && fgets(line, sizeof(line), f)) {
			char l[80], r[80], la[96], ra[96];
			int st;
			unsigned long inode;
			if (sscanf(line, "%*d: %79s %79s %x %*s %*s %*s %*d %*d %lu", l, r, &st, &inode) != 4) continue;
			if (tcp && st == 6) continue;	/* time-wait: no owner, no cost */
			addr(l, v6, la, sizeof(la));
			addr(r, v6, ra, sizeof(ra));
			int pid = owner_of(inode);
			char comm[32] = "-", p[64];
			snprintf(p, sizeof(p), "/proc/%d/comm", pid);
			FILE *c = pid ? fopen(p, "r") : NULL;
			if (c) {
				if (fgets(comm, sizeof(comm), c)) comm[strcspn(comm, "\n")] = 0;
				fclose(c);
			}
			fprintf(out, "%ld,%s%s,%s,%s,%s,%d,%s\n", (long)now, tcp ? "tcp" : "udp", v6 ? "6" : "",
				tcp ? tcp_state(st) : (st == 7 ? "open" : st == 1 ? "connected" : "?"), la, ra, pid, comm);
		}
		if (f) fclose(f);
	}
	if (out) fclose(out);
}

/* ---- flushing to flash --------------------------------------------------- */

static void copy(const char *from, const char *to) {
	int a = open(from, O_RDONLY | O_CLOEXEC);
	if (a < 0) return;
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.new", to);
	int b = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	char buf[65536];
	ssize_t n;
	bool ok = b >= 0;
	while (ok && (n = read(a, buf, sizeof(buf))) > 0) ok = write(b, buf, n) == n;
	close(a);
	if (b >= 0) {
		ok = ok && fsync(b) == 0;
		close(b);
		if (ok) rename(tmp, to);
		else unlink(tmp);
	}
}

static void write_summary(const char *d, const char *to);

static void flush(const char *why) {
	static const char *names[] = { "samples.csv", "traffic.csv", "talkers.csv", "ports.csv", "sockets.csv",
		"session", "log" };
	mkdir(LOG_DIR, 0755);
	mkdir(logdir, 0755);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char a[512], b[512];
		snprintf(a, sizeof(a), "%s/%s", dir, names[i]);
		snprintf(b, sizeof(b), "%s/%s", logdir, names[i]);
		copy(a, b);
	}
	char s[512];
	snprintf(s, sizeof(s), "%s/summary.json", logdir);
	write_summary(dir, s);
	fprintf(stderr, "ztprofile: written to %s (%s)\n", logdir, why);
}

/* the sessions on flash, oldest first */
static int sessions(char names[][32], int max) {
	DIR *l = opendir(LOG_DIR);
	struct dirent *e;
	int n = 0;
	while (l && (e = readdir(l)) && n < max)
		if (e->d_name[0] != '.' && strlen(e->d_name) < 32) snprintf(names[n++], 32, "%s", e->d_name);
	if (l) closedir(l);
	qsort(names, n, 32, (int (*)(const void *, const void *))strcmp);
	return n;
}

static long long du(const char *d) {
	DIR *l = opendir(d);
	struct dirent *e;
	long long sum = 0;
	while (l && (e = readdir(l))) {
		char p[600];
		struct stat st;
		snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
		if (e->d_name[0] != '.' && stat(p, &st) == 0 && S_ISREG(st.st_mode)) sum += st.st_size;
	}
	if (l) closedir(l);
	return sum;
}

static bool valid_session(const char *s) {
	if (!*s || strlen(s) >= 32) return false;
	for (const char *p = s; *p; p++)
		if (!isdigit((unsigned char)*p) && *p != '-') return false;
	return true;
}

static void remove_session(const char *s) {
	char d[300];
	snprintf(d, sizeof(d), LOG_DIR "/%s", s);
	DIR *l = opendir(d);
	struct dirent *e;
	while (l && (e = readdir(l))) {
		char p[600];
		snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
		if (e->d_name[0] != '.') unlink(p);
	}
	if (l) closedir(l);
	rmdir(d);
}

/* LOG_CAP for all sessions on flash: the oldest go first (never the running one) */
static void prune(const char *keep) {
	static char names[512][32];
	int n = sessions(names, 512);
	long long total = 0;
	for (int i = 0; i < n; i++) {
		char d[300];
		snprintf(d, sizeof(d), LOG_DIR "/%s", names[i]);
		total += du(d);
	}
	for (int i = 0; i < n && total > LOG_CAP; i++) {
		if (keep && !strcmp(names[i], keep)) continue;
		char d[300];
		snprintf(d, sizeof(d), LOG_DIR "/%s", names[i]);
		total -= du(d);
		remove_session(names[i]);
		fprintf(stderr, "ztprofile: session %s removed (over %d MB of sessions)\n", names[i], LOG_CAP >> 20);
	}
}

/* ---- the status file, for Settings --------------------------------------- */

static char session_name[32], last_session[32];
static time_t session_start, session_end;

static void status_write(bool on) {
	FILE *f = fopen(STATUS ".new", "w");
	if (!f) return;
	fprintf(f, "running=%s\n", on ? "yes" : "no");
	if (on) {
		long left = (session_end - time(NULL) + 59) / 60;
		fprintf(f, "session=%s\ndir=%s\nlog_dir=%s\nstarted=%ld\nends=%ld\nminutes_left=%ld\n", session_name, dir,
			logdir, (long)session_start, (long)session_end, left < 0 ? 0 : left);
		fprintf(f, "sample_t=%ld\nscreen=%s\nbssid=%s\nrssi=%d\nbat_ma=%ld\n", (long)live.t,
			live.screen == 1 ? "on" : live.screen == 0 ? "off" : "?", live.bssid, live.rssi, live.ma);
		fprintf(f, "interrupts_per_min=%.0f\nidle_entries_per_min=%.0f\nwifi_irq_per_min=%.1f\n", live.intr_min,
			live.idle_min, live.irq_min);
		fprintf(f, "frames_per_min=%lu\n", live.frames);
		for (int i = 0; i < D_N; i++) fprintf(f, "fpm_%s=%lu\n", dnames[i], live.d[i]);
		for (int i = 0; i < K_N; i++) fprintf(f, "fpm_%s=%lu\n", knames[i], live.k[i]);
	}
	if (last_session[0]) fprintf(f, "last_session=%s\n", last_session);
	fclose(f);
	chmod(STATUS ".new", 0644);
	rename(STATUS ".new", STATUS);
}

/* ---- a session ----------------------------------------------------------- */

static volatile sig_atomic_t stopping;
static void on_signal(int s) { stopping = 1; }
static int ctl = -1;
static char pending[512];
static size_t pending_len;

/* a line from the control pipe: 0 nothing, 1 stop, or minutes to start */
static int ctl_line(char *line) {
	char what[16] = "", arg[64] = "";
	if (sscanf(line, "%15s %63s", what, arg) < 1) return 0;
	if (!strcmp(what, "start")) {
		int m = atoi(arg);
		return m < 1 ? 60 : m > MAX_MIN ? MAX_MIN : m;
	}
	if (!strcmp(what, "stop")) return -1;
	if (!strcmp(what, "delete") && valid_session(arg) && strcmp(arg, session_name)) {
		remove_session(arg);
		fprintf(stderr, "ztprofile: session %s deleted\n", arg);
	}
	return 0;
}

/* read what is there; the first start/stop is returned, the rest done */
static int ctl_read(void) {
	int r = 0;
	ssize_t got = read(ctl, pending + pending_len, sizeof(pending) - 1 - pending_len);
	if (got <= 0) return 0;
	pending_len += got;
	char *s = pending, *nl;
	while ((nl = memchr(s, '\n', pending + pending_len - s))) {
		*nl = 0;
		int c = ctl_line(s);
		if (c && !r) r = c;
		s = nl + 1;
	}
	pending_len -= s - pending;
	memmove(pending, s, pending_len);
	if (pending_len == sizeof(pending) - 1) pending_len = 0;
	return r;
}

static void run(int minutes) {
	time_t now = time(NULL);
	strftime(session_name, sizeof(session_name), "%Y%m%d-%H%M%S", localtime(&now));
	mkdir(RUN_DIR, 0755);
	snprintf(dir, sizeof(dir), RUN_DIR "/%s", session_name);
	snprintf(logdir, sizeof(logdir), LOG_DIR "/%s", session_name);
	mkdir(dir, 0755);
	prune(NULL);
	char p[600];
	snprintf(p, sizeof(p), "%s/log", dir);
	FILE *log = fopen(p, "a");
	int saved = dup(2);
	if (log) {
		dup2(fileno(log), 2);
		fclose(log);
	}
	unlink(CURRENT);
	if (symlink(dir, CURRENT)) {}
	memset(&cur, 0, sizeof(cur));
	memset(&live, 0, sizeof(live));
	first = true;
	wakes = ring_drops = 0;
	block = 0;
	find_power();
	cap = capture_open(ifname);
	if (cap < 0) fprintf(stderr, "ztprofile: no frame capture on %s: %s\n", ifname, strerror(errno));
	session_start = now;
	session_end = now + minutes * 60L;
	FILE *s = open_run("session", "key,value");
	if (s) {
		fprintf(s, "start,%ld\nminutes,%d\ninterface,%s\nsample_s,%d\nsockets_s,%d\nname,%s\n", (long)now, minutes,
			ifname, SAMPLE_S, SOCKETS_S, session_name);
		fclose(s);
	}
	fprintf(stderr, "ztprofile: session %s, %d min, on %s\n", session_name, minutes, ifname);
	time_t next_sample = now, next_sockets = now, next_flush = now + FLUSH_S, this_min = now / 60 * 60;
	int last_screen = screen_state();
	const char *why = "time up";
	sample(now);
	status_write(true);
	while (!stopping) {
		now = time(NULL);
		if (now >= session_end) break;
		time_t due = next_sample < next_sockets ? next_sample : next_sockets;
		if (this_min + 60 < due) due = this_min + 60;
		int wait = due > now ? (int)(due - now) * 1000 : 0;
		struct pollfd fds[3] = { { cap, POLLIN | POLLERR, 0 }, { wlpm_fd, POLLIN, 0 }, { ctl, POLLIN, 0 } };
		if (poll(fds, 3, wait) < 0 && errno != EINTR) break;
		wakes++;
		if (cap >= 0 && (fds[0].revents & POLLIN)) capture_drain();
		if (wlpm_fd >= 0 && (fds[1].revents & POLLIN)) wlpm_event();
		if (ctl >= 0 && (fds[2].revents & POLLIN) && ctl_read() < 0) { why = "stopped"; break; }
		now = time(NULL);
		if (now >= this_min + 60) {
			if (cap >= 0) capture_drain();
			minute(this_min);
			this_min = now / 60 * 60;
		}
		if (now >= next_sample) {
			sample(now);
			next_sample = now + SAMPLE_S;
			int sc = live.screen;
			if (sc == 1 && last_screen == 0) flush("screen on");
			last_screen = sc;
			status_write(true);
		}
		if (now >= next_sockets) {
			sockets(now);
			next_sockets = now + SOCKETS_S;
		}
		if (now >= next_flush) {
			flush("every 15 min");
			next_flush = now + FLUSH_S;
		}
	}
	if (stopping) why = "service stopped";
	if (cap >= 0) capture_drain();
	minute(this_min);
	sample(time(NULL));
	sockets(time(NULL));
	fprintf(stderr, "ztprofile: session %s ended (%s)\n", session_name, why);
	flush("end");
	prune(session_name);
	if (cap >= 0) {
		munmap(ring, BLOCK_SIZE * BLOCK_NR);
		close(cap);
		cap = -1;
	}
	/* the RAM copy goes: everything is on flash now */
	static const char *names[] = { "samples.csv", "traffic.csv", "talkers.csv", "ports.csv", "sockets.csv",
		"session", "log" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
		unlink(p);
	}
	rmdir(dir);
	unlink(CURRENT);
	snprintf(last_session, sizeof(last_session), "%s", session_name);
	session_name[0] = 0;
	snprintf(dir, sizeof(dir), "%s", logdir);	/* reports read the flash copy */
	if (saved >= 0) {
		dup2(saved, 2);
		close(saved);
	}
	status_write(false);
}

/* the root part: asleep in read() on the control pipe until told to start */
static int service(void) {
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	wlpm_name = "ztprofile";
	unlink(CTL);
	if (mkfifo(CTL, 0666) < 0) { perror("ztprofile: " CTL); return 1; }
	chmod(CTL, 0666);
	ctl = open(CTL, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctl < 0) { perror("ztprofile: " CTL); return 1; }
	static char names[512][32];
	int n = sessions(names, 512);
	if (n) snprintf(last_session, sizeof(last_session), "%s", names[n - 1]);
	status_write(false);
	bool nl = false;
	while (!stopping) {
		struct pollfd p = { ctl, POLLIN, 0 };
		if (poll(&p, 1, -1) < 0 && errno != EINTR) break;
		int c = (p.revents & POLLIN) ? ctl_read() : 0;
		if (c > 0) {
			if (!nl) wlpm_fd = wlpm_open();	/* nl80211 once, when first needed */
			nl = true;
			run(c);
		}
	}
	unlink(CTL);
	return 0;
}

/* ---- the summary: for the report, Settings and the session list --------- */

#define MAXCOL 32
static int split(char *line, char **col) {
	int n = 0;
	line[strcspn(line, "\n")] = 0;
	for (char *p = line; n < MAXCOL; ) {
		col[n++] = p;
		char *c = strchr(p, ',');
		if (!c) break;
		*c = 0;
		p = c + 1;
	}
	return n;
}

static FILE *open_in(const char *d, const char *name, char **hdr, int *nh, char *head, size_t len) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", d, name);
	FILE *f = fopen(p, "r");
	if (!f || !fgets(head, len, f)) {
		if (f) fclose(f);
		return NULL;
	}
	*nh = split(head, hdr);
	return f;
}

static int col(char **hdr, int nh, const char *name) {
	for (int i = 0; i < nh; i++)
		if (!strcmp(hdr[i], name)) return i;
	return 0;
}

struct agg { char key[160]; double n; };
static void agg_add(struct agg *a, int *n, int max, const char *key, double v) {
	for (int i = 0; i < *n; i++)
		if (!strcmp(a[i].key, key)) { a[i].n += v; return; }
	if (*n < max) { snprintf(a[*n].key, sizeof(a[0].key), "%s", key); a[(*n)++].n = v; }
}
static int cmp_agg(const void *x, const void *y) {
	double a = ((const struct agg *)x)->n, b = ((const struct agg *)y)->n;
	return a < b ? 1 : a > b ? -1 : 0;
}

static struct summary {
	char name[32];
	long start;
	int planned;
	double minutes, off_minutes, ma, ma_off, rx, tx, mc, irq, rssi, filtered, intr, idle_entries, idle_pct,
		self_cpu_pct, self_wakes, r;
	bool have_r, ring_drops;
	struct agg bss[16];
	int nbss;
	double mins, frames, kb, by[MAXCOL];
	char kind[MAXCOL][16];
	int nkind;
	struct agg talkers[512], ports[256], socks[1024];
	int ntalk, nport, nsock, stayed, came;
	long snaps;
} S;

static bool summarise(const char *d) {
	char *h[MAXCOL], *c[MAXCOL], line[1024], head[1024];
	int nh;
	memset(&S, 0, sizeof(S));
	const char *base = strrchr(d, '/');
	snprintf(S.name, sizeof(S.name), "%s", base ? base + 1 : d);
	FILE *f = open_in(d, "session", h, &nh, head, sizeof(head));
	while (f && fgets(line, sizeof(line), f)) {
		if (split(line, c) < 2) continue;
		if (!strcmp(c[0], "start")) S.start = atol(c[1]);
		if (!strcmp(c[0], "minutes")) S.planned = atoi(c[1]);
	}
	if (f) fclose(f);

	f = open_in(d, "samples.csv", h, &nh, head, sizeof(head));
	double tsec = 0, off = 0, n_all = 0, n_off = 0, nrssi = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, nc = 0;
	if (f) {
		int idt = col(h, nh, "dt"), isc = col(h, nh, "screen"), irx = col(h, nh, "rx"), itx = col(h, nh, "tx"),
		    imc = col(h, nh, "multicast"), iirq = col(h, nh, "wifi_irq"), iin = col(h, nh, "intr"),
		    iie = col(h, nh, "idle_entries"), iid = col(h, nh, "idle_pct"), ifd = col(h, nh, "filter_discarded"),
		    ib = col(h, nh, "bat_ma"), icpu = col(h, nh, "self_cpu_ms"), iwk = col(h, nh, "self_wakes"),
		    irs = col(h, nh, "rssi"), ibs = col(h, nh, "bssid"), ird = col(h, nh, "ring_drops");
		while (fgets(line, sizeof(line), f)) {
			if (split(line, c) < nh) continue;
			double dt = atof(c[idt]), b = atof(c[ib]);
			if (dt <= 0) continue;
			tsec += dt;
			S.rx += atof(c[irx]); S.tx += atof(c[itx]); S.mc += atof(c[imc]); S.irq += atof(c[iirq]);
			S.intr += atof(c[iin]); S.idle_entries += atof(c[iie]); S.idle_pct += atof(c[iid]) * dt;
			S.filtered += atof(c[ifd]); S.self_cpu_pct += atof(c[icpu]); S.self_wakes += atof(c[iwk]);
			S.ring_drops = atof(c[ird]) > 0;
			if (b > 0) { S.ma += b; n_all++; }
			if (atoi(c[isc]) == 0) {
				off += dt;
				if (b > 0) {
					S.ma_off += b; n_off++;
					double x = atof(c[irx]) / dt;
					sx += x; sy += b; sxx += x * x; syy += b * b; sxy += x * b; nc++;
				}
			}
			if (atoi(c[irs]) < 0) { S.rssi += atoi(c[irs]); nrssi++; }
			agg_add(S.bss, &S.nbss, 16, c[ibs], dt);
		}
		fclose(f);
	}
	if (tsec <= 0) return false;
	double m = tsec / 60;
	S.minutes = m;
	S.off_minutes = off / 60;
	S.ma = n_all ? S.ma / n_all : 0;
	S.ma_off = n_off ? S.ma_off / n_off : 0;
	S.rx /= m; S.tx /= m; S.mc /= m; S.irq /= m; S.intr /= m; S.idle_entries /= m; S.filtered /= m;
	S.self_wakes /= m;
	S.idle_pct /= tsec;
	S.self_cpu_pct /= tsec * 10;	/* ms per s, in percent of one CPU */
	S.rssi = nrssi ? S.rssi / nrssi : 0;
	for (int i = 0; i < S.nbss; i++) S.bss[i].n = 100 * S.bss[i].n / tsec;
	qsort(S.bss, S.nbss, sizeof(S.bss[0]), cmp_agg);
	double den = (nc * sxx - sx * sx) * (nc * syy - sy * sy);
	if (nc > 2 && den > 0) { S.r = (nc * sxy - sx * sy) / sqrt(den); S.have_r = true; }

	f = open_in(d, "traffic.csv", h, &nh, head, sizeof(head));
	if (f) {
		S.nkind = nh - 3;
		for (int i = 3; i < nh; i++) snprintf(S.kind[i - 3], sizeof(S.kind[0]), "%s", h[i]);
		while (fgets(line, sizeof(line), f)) {
			if (split(line, c) < nh) continue;
			S.frames += atof(c[1]);
			S.kb += atof(c[2]) / 1024;
			for (int i = 3; i < nh; i++) S.by[i - 3] += atof(c[i]);
			S.mins++;
		}
		fclose(f);
		if (S.mins) {
			S.frames /= S.mins;
			S.kb /= S.mins;
			for (int i = 0; i < S.nkind; i++) S.by[i] /= S.mins;
		}
	}
	double per = S.mins ? S.mins : 1;
	f = open_in(d, "talkers.csv", h, &nh, head, sizeof(head));
	while (f && fgets(line, sizeof(line), f)) {
		if (split(line, c) < 4) continue;
		char k[160];
		snprintf(k, sizeof(k), "%s %s", c[1], c[2]);
		agg_add(S.talkers, &S.ntalk, 512, k, atof(c[3]) / per);
	}
	if (f) fclose(f);
	qsort(S.talkers, S.ntalk, sizeof(S.talkers[0]), cmp_agg);
	f = open_in(d, "ports.csv", h, &nh, head, sizeof(head));
	while (f && fgets(line, sizeof(line), f)) {
		if (split(line, c) < 4) continue;
		char k[160];
		snprintf(k, sizeof(k), "%s %s", c[1], c[2]);
		agg_add(S.ports, &S.nport, 256, k, atof(c[3]) / per);
	}
	if (f) fclose(f);
	qsort(S.ports, S.nport, sizeof(S.ports[0]), cmp_agg);

	/* sockets: key "proto state local remote process", counted per snapshot */
	f = open_in(d, "sockets.csv", h, &nh, head, sizeof(head));
	long prev = 0;
	while (f && fgets(line, sizeof(line), f)) {
		if (split(line, c) < 7) continue;
		long t = atol(c[0]);
		if (t != prev) { S.snaps++; prev = t; }
		bool est = !strcmp(c[2], "established") || !strcmp(c[2], "connected");
		bool lis = !strcmp(c[2], "listen") || !strcmp(c[2], "open");
		if (!est && !lis) continue;
		if (!strncmp(c[3], "127.", 4) || !strncmp(c[3], "[::1]", 5)) continue;	/* loopback */
		char k[160];
		snprintf(k, sizeof(k), "%s %s %s %s %s", c[1], est ? "up" : "listen", c[3], est ? c[4] : "-", c[6]);
		agg_add(S.socks, &S.nsock, 1024, k, 1);
	}
	if (f) fclose(f);
	qsort(S.socks, S.nsock, sizeof(S.socks[0]), cmp_agg);
	for (int i = 0; i < S.nsock; i++) {
		if (S.socks[i].n >= S.snaps) S.stayed++;
		else S.came++;
	}
	return true;
}

static void text_report(const char *d, bool live_run) {
	printf("ztprofile report: %s%s\n", d, live_run ? " (running)" : "");
	if (!summarise(d)) { printf("  no samples yet\n"); return; }
	printf("\n%.0f min sampled (of %d planned), %.0f min with the screen off\n", S.minutes, S.planned, S.off_minutes);
	printf("battery draw: %.0f mA average, %.0f mA with the screen off\n", S.ma, S.ma_off);
	printf("Wi-Fi: rx %.1f tx %.1f frames/min (multicast %.1f/min), chip interrupts %.1f/min, signal %.0f dBm\n",
	       S.rx, S.tx, S.mc, S.irq, S.rssi);
	if (S.filtered > 0) printf("       the chip's filter kept %.1f frames/min from the host\n", S.filtered);
	printf("       BSSIDs:");
	for (int i = 0; i < S.nbss; i++) printf(" %s %.0f%%", S.bss[i].key, S.bss[i].n);
	printf("\nwakes: %.0f interrupts/min, %.0f idle entries/min, CPUs %.0f%% idle\n", S.intr, S.idle_entries,
	       S.idle_pct);
	printf("the profiler itself: %.2f%% of one CPU, %.1f wakes/min%s\n", S.self_cpu_pct, S.self_wakes,
	       S.ring_drops ? ", some frames lost by its ring" : "");
	if (S.have_r) printf("current vs received frames (screen off, per 15 s sample): r = %.2f\n", S.r);
	if (S.mins) {
		printf("\nframes in, per minute (%.0f min): %.1f (%.1f kB)\n  ", S.mins, S.frames, S.kb);
		int shown = 0;
		for (int i = 0; i < S.nkind; i++)
			if (S.by[i] > 0 || i < 3)
				printf("%s %.1f%s", S.kind[i], S.by[i], ++shown % 8 ? "  " : "\n  ");
		printf("\n");
	}
	if (S.ntalk) printf("\ntop senders (frames/min, MAC IP):\n");
	for (int i = 0; i < S.ntalk && i < 10; i++) printf("  %8.2f  %s\n", S.talkers[i].n, S.talkers[i].key);
	if (S.nport) printf("\nunicast to local ports (frames/min):\n");
	for (int i = 0; i < S.nport && i < 10; i++) printf("  %8.2f  %s\n", S.ports[i].n, S.ports[i].key);
	if (S.snaps) {
		printf("\nsockets over %ld snapshots (every %d min): %d open throughout, %d came and went\n", S.snaps,
		       SOCKETS_S / 60, S.stayed, S.came);
		int shown = 0;
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < S.nsock && shown < 30; i++)
				if (S.socks[i].n >= S.snaps && (strstr(S.socks[i].key, " up ") != NULL) == !pass) {
					printf("  %s\n", S.socks[i].key);
					shown++;
				}
	}
}

static void json_str(FILE *f, const char *s) {
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
		else fputc(*s, f);
	}
	fputc('"', f);
}

static void json_report(FILE *f, const char *d, bool live_run) {
	bool ok = summarise(d);
	fprintf(f, "{\"session\":");
	json_str(f, S.name);
	fprintf(f, ",\"dir\":");
	json_str(f, d);
	fprintf(f, ",\"running\":%s,\"start\":%ld,\"minutes_planned\":%d", live_run ? "true" : "false", S.start,
		S.planned);
	if (!ok) { fprintf(f, ",\"minutes\":0}\n"); return; }
	fprintf(f, ",\"minutes\":%.1f,\"screen_off_minutes\":%.1f,\"battery_ma\":%.0f,\"battery_ma_screen_off\":%.0f",
		S.minutes, S.off_minutes, S.ma, S.ma_off);
	fprintf(f, ",\"wifi\":{\"rx_per_min\":%.1f,\"tx_per_min\":%.1f,\"multicast_per_min\":%.1f,\"irq_per_min\":%.1f,"
		"\"rssi_dbm\":%.0f,\"filtered_per_min\":%.1f,\"bssids\":[", S.rx, S.tx, S.mc, S.irq, S.rssi, S.filtered);
	for (int i = 0; i < S.nbss; i++) {
		fprintf(f, "%s{\"bssid\":", i ? "," : "");
		json_str(f, S.bss[i].key);
		fprintf(f, ",\"percent\":%.0f}", S.bss[i].n);
	}
	fprintf(f, "]},\"wakes\":{\"interrupts_per_min\":%.0f,\"idle_entries_per_min\":%.0f,\"idle_percent\":%.1f}",
		S.intr, S.idle_entries, S.idle_pct);
	fprintf(f, ",\"profiler\":{\"cpu_percent\":%.2f,\"wakes_per_min\":%.1f,\"ring_drops\":%s}", S.self_cpu_pct,
		S.self_wakes, S.ring_drops ? "true" : "false");
	if (S.have_r) fprintf(f, ",\"current_vs_rx_r\":%.2f", S.r);
	else fprintf(f, ",\"current_vs_rx_r\":null");
	fprintf(f, ",\"frames\":{\"minutes\":%.0f,\"per_min\":%.1f,\"kb_per_min\":%.1f,\"by_kind\":{", S.mins, S.frames,
		S.kb);
	for (int i = 0; i < S.nkind; i++) {
		fprintf(f, "%s", i ? "," : "");
		json_str(f, S.kind[i]);
		fprintf(f, ":%.2f", S.by[i]);
	}
	fprintf(f, "}},\"top_senders\":[");
	for (int i = 0; i < S.ntalk && i < 10; i++) {
		char mac[32] = "", ip[INET6_ADDRSTRLEN + 2] = "";
		sscanf(S.talkers[i].key, "%31s %47s", mac, ip);
		fprintf(f, "%s{\"mac\":", i ? "," : "");
		json_str(f, mac);
		fprintf(f, ",\"ip\":");
		json_str(f, ip);
		fprintf(f, ",\"per_min\":%.2f}", S.talkers[i].n);
	}
	fprintf(f, "],\"top_ports\":[");
	for (int i = 0; i < S.nport && i < 10; i++) {
		char proto[8] = "";
		int port = 0;
		sscanf(S.ports[i].key, "%7s %d", proto, &port);
		fprintf(f, "%s{\"proto\":", i ? "," : "");
		json_str(f, proto);
		fprintf(f, ",\"port\":%d,\"per_min\":%.2f}", port, S.ports[i].n);
	}
	fprintf(f, "],\"sockets\":{\"snapshots\":%ld,\"stayed\":%d,\"came_and_went\":%d,\"open_throughout\":[", S.snaps,
		S.stayed, S.came);
	int shown = 0;
	for (int i = 0; i < S.nsock && shown < 50; i++) {
		if (S.socks[i].n < S.snaps) continue;
		char proto[8], state[8], local[64], remote[64], proc[32] = "";
		if (sscanf(S.socks[i].key, "%7s %7s %63s %63s %31[^\n]", proto, state, local, remote, proc) < 4) continue;
		fprintf(f, "%s{\"proto\":", shown++ ? "," : "");
		json_str(f, proto);
		fprintf(f, ",\"state\":");
		json_str(f, state);
		fprintf(f, ",\"local\":");
		json_str(f, local);
		fprintf(f, ",\"remote\":");
		json_str(f, remote);
		fprintf(f, ",\"process\":");
		json_str(f, proc);
		fputc('}', f);
	}
	fprintf(f, "]}}\n");
}

static void write_summary(const char *d, const char *to) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.new", to);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	json_report(f, d, session_name[0] != 0);
	fclose(f);
	rename(tmp, to);
}

/* ---- the command line ---------------------------------------------------- */

static bool is_running(char *d, size_t len) {
	FILE *f = fopen(STATUS, "r");
	char line[600];
	bool on = false;
	if (d) d[0] = 0;
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strcmp(line, "running=yes")) on = true;
		if (d && !strncmp(line, "dir=", 4)) snprintf(d, len, "%s", line + 4);
	}
	if (f) fclose(f);
	return on;
}

static int send_ctl(const char *line) {
	int fd = open(CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "ztprofile: the service is not running (systemctl start ztprofile)\n");
		return 1;
	}
	bool ok = write(fd, line, strlen(line)) == (ssize_t)strlen(line);
	close(fd);
	return ok ? 0 : 1;
}

static const char *find_session(const char *which, char *d, size_t len) {
	if (which) {
		snprintf(d, len, LOG_DIR "/%s", which);
		if (access(d, F_OK)) snprintf(d, len, "%s", which);
	} else if (!is_running(d, len)) {
		static char names[512][32];
		int n = sessions(names, 512);
		d[0] = 0;
		if (n) snprintf(d, len, LOG_DIR "/%s", names[n - 1]);
	}
	return d[0] && !access(d, F_OK) ? d : NULL;
}

int main(int argc, char **argv) {
	if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fputs(HELP, stdout);
		return argc < 2;
	}
	if (getenv("ZTPROFILE_IF")) snprintf(ifname, sizeof(ifname), "%s", getenv("ZTPROFILE_IF"));
	const char *cmd = argv[1];
	bool json = argc > 2 && !strcmp(argv[2], "--json");
	const char *arg = argc > 2 + json ? argv[2 + json] : NULL;
	if (!strcmp(cmd, "service")) return service();
	if (!strcmp(cmd, "start")) {
		char line[32];
		int m = arg ? atoi(arg) : 60;
		snprintf(line, sizeof(line), "start %d\n", m < 1 ? 1 : m > MAX_MIN ? MAX_MIN : m);
		if (is_running(NULL, 0)) { fprintf(stderr, "ztprofile: already running\n"); return 1; }
		return send_ctl(line);
	}
	if (!strcmp(cmd, "stop")) return send_ctl("stop\n");
	if (!strcmp(cmd, "delete")) {
		if (!arg || !valid_session(arg)) { fprintf(stderr, "ztprofile: delete SESSION\n"); return 2; }
		char line[64];
		snprintf(line, sizeof(line), "delete %s\n", arg);
		return send_ctl(line);
	}
	if (!strcmp(cmd, "status")) {
		FILE *f = fopen(STATUS, "r");
		char line[256];
		if (!f) { printf("running=no\n"); return 0; }
		while (fgets(line, sizeof(line), f)) fputs(line, stdout);
		fclose(f);
		return 0;
	}
	if (!strcmp(cmd, "report")) {
		char d[512];
		char live_dir[512];
		bool live_run = is_running(live_dir, sizeof(live_dir));
		if (!find_session(arg, d, sizeof(d))) { fprintf(stderr, "ztprofile: no session found\n"); return 1; }
		live_run = live_run && !strcmp(d, live_dir);
		if (json) json_report(stdout, d, live_run);
		else text_report(d, live_run);
		return 0;
	}
	if (!strcmp(cmd, "list")) {
		static char names[512][32];
		int n = sessions(names, 512);
		long long total = 0;
		if (json) printf("{\"cap_bytes\":%d,\"sessions\":[", LOG_CAP);
		for (int i = n - 1; i >= 0; i--) {
			char d[300], p[400], buf[16384] = "";
			snprintf(d, sizeof(d), LOG_DIR "/%s", names[i]);
			long long size = du(d);
			total += size;
			snprintf(p, sizeof(p), "%s/summary.json", d);
			FILE *f = fopen(p, "r");
			size_t got = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
			if (f) fclose(f);
			buf[got] = 0;
			buf[strcspn(buf, "\n")] = 0;
			if (json) {
				printf("%s{\"session\":", i == n - 1 ? "" : ",");
				json_str(stdout, names[i]);
				printf(",\"bytes\":%lld,\"summary\":%s}", size, got ? buf : "null");
			} else {
				summarise(d);
				printf("%s  %5.0f min  %4.0f mA screen off  %6.1f frames/min  %6.0f kB\n", names[i], S.minutes,
				       S.ma_off, S.frames, size / 1024.0);
			}
		}
		if (json) printf("],\"total_bytes\":%lld}\n", total);
		else printf("%d sessions, %.1f MB of %d MB\n", n, total / 1048576.0, LOG_CAP >> 20);
		return 0;
	}
	fputs(HELP, stderr);
	return 2;
}
