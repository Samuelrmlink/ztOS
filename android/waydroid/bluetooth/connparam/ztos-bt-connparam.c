// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztos-bt-connparam: one operation for ztos-btbridge, the LE Connection Update
 * an Android app asks for with BluetoothGatt.requestConnectionPriority (a
 * firmware updater's HIGH: a 11.25-15 ms interval instead of BlueZ's 30-50).
 *
 * BlueZ has no D-Bus call for it. The kernel's own route (mgmt Load
 * Connection Parameters with one entry) also stores the parameters for every
 * later connection to that device and, when it has none yet, drops other
 * devices' stored ones; and it says nothing about the interval the
 * peripheral settled on. So this sends the HCI command itself, on a raw HCI
 * socket (which takes CAP_NET_RAW), and waits for its LE Connection Update
 * Complete. The kernel sees that event too and keeps its own record right.
 *
 * What it does, and all it does:
 *   - Started by its socket (ztos-bt-connparam.socket, SOCK_SEQPACKET in /run);
 *     a client must be root or a user with a session (/run/user/UID).
 *   - A request (16 bytes): address (6, little-endian), min interval, max
 *     interval (units of 1.25 ms), latency (events), supervision timeout
 *     (units of 10 ms), each 2 bytes little-endian, and 2 bytes zero.
 *   - It checks the values against the Core spec (interval 7.5 ms-4 s, min <=
 *     max, latency <= 499, timeout 100 ms-32 s and longer than
 *     (1 + latency) * max interval * 2), and that there is an LE connection to
 *     that address on which this side is the central.
 *   - It sends one HCI command, LE Connection Update (0x2013), for that
 *     connection's handle, and waits up to WAIT_SECONDS for its Command Status
 *     and LE Connection Update Complete.
 *   - The answer (7 bytes): status (0, an HCI error code, or one of the ST_
 *     codes below), the interval, latency and timeout now in use.
 *   - No other command, ever; nothing is read but the events it waits for.
 *   - With no client for IDLE_SECONDS it exits.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_ 1
#define SOL_HCI_ 0
#define HCI_FILTER_ 2
#define HCI_CHANNEL_RAW_ 0
#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT 0x04
#define EVT_CMD_STATUS 0x0f
#define EVT_LE_META 0x3e
#define LE_CONN_UPDATE_COMPLETE 0x03
#define OP_LE_CONN_UPDATE 0x2013
#define LE_LINK 0x80
#define HCI_LM_MASTER 0x0001
#define HCIGETCONNINFO_ _IOR('H', 213, int)
#define MAX_DEVS 4
#define WAIT_SECONDS 5
#define IDLE_SECONDS 30
#define LISTEN_FD 3

enum { ST_OK = 0, ST_INVALID = 0xf0, ST_NO_LINK = 0xf1, ST_NOT_CENTRAL = 0xf2, ST_TIMEOUT = 0xf3, ST_IO = 0xf4 };

struct request {
	uint8_t addr[6];
	uint16_t min, max, latency, timeout;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* A request packet, read and checked: false if it is not one. */
bool connparam_request(const uint8_t *buf, size_t n, struct request *r) {
	if (n != 16 || le16(buf + 14)) return false;
	memcpy(r->addr, buf, 6);
	r->min = le16(buf + 6);
	r->max = le16(buf + 8);
	r->latency = le16(buf + 10);
	r->timeout = le16(buf + 12);
	return true;
}

/* The Core spec's limits (Vol 4 Part E 7.8.18). */
bool connparam_valid(const struct request *r) {
	if (r->min < 0x0006 || r->max > 0x0c80 || r->min > r->max) return false;
	if (r->latency > 0x01f3) return false;
	if (r->timeout < 0x000a || r->timeout > 0x0c80) return false;
	/* timeout (10 ms units) > (1 + latency) * max (1.25 ms units) * 2, in 1.25 ms units */
	return (uint32_t)r->timeout * 8 > (uint32_t)(1 + r->latency) * r->max * 2;
}

/* The command packet for a connection handle. */
size_t connparam_command(uint8_t *out, uint16_t handle, const struct request *r) {
	out[0] = HCI_COMMAND_PKT;
	put16(out + 1, OP_LE_CONN_UPDATE);
	out[3] = 14;
	put16(out + 4, handle);
	put16(out + 6, r->min);
	put16(out + 8, r->max);
	put16(out + 10, r->latency);
	put16(out + 12, r->timeout);
	put16(out + 14, 0);	/* connection event length: the controller's choice */
	put16(out + 16, 0);
	return 18;
}

/* An event packet for our command: 1 if it settles the request (status in
 * *st, and the parameters in *got when it is the completion), 0 if not ours. */
int connparam_event(const uint8_t *p, size_t n, uint16_t handle, uint8_t *st, uint16_t got[3]) {
	if (n < 3 || p[0] != HCI_EVENT_PKT || (size_t)p[2] + 3 != n) return 0;
	if (p[1] == EVT_CMD_STATUS && n >= 7 && le16(p + 5) == OP_LE_CONN_UPDATE) {
		if (p[3] == 0) return 0;	/* accepted: the completion follows */
		*st = p[3];
		return 1;
	}
	if (p[1] == EVT_LE_META && n >= 13 && p[3] == LE_CONN_UPDATE_COMPLETE && (le16(p + 5) & 0x0fff) == handle) {
		*st = p[4];
		got[0] = le16(p + 7);
		got[1] = le16(p + 9);
		got[2] = le16(p + 11);
		return 1;
	}
	return 0;
}

#ifndef CONNPARAM_NO_MAIN
struct sockaddr_hci_ { sa_family_t hci_family; unsigned short hci_dev; unsigned short hci_channel; };
struct hci_filter_ { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };
struct hci_conn_info_ { uint16_t handle; uint8_t bdaddr[6]; uint8_t type; uint8_t out; uint16_t state; uint32_t link_mode; };
struct hci_conn_info_req_ { uint8_t bdaddr[6]; uint8_t type; struct hci_conn_info_ info; };

static bool allowed(int fd) {
	struct ucred cred;
	socklen_t size = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) || size != sizeof(cred)) return false;
	if (cred.uid == 0) return true;
	char path[64];
	struct stat st;
	snprintf(path, sizeof(path), "/run/user/%u", (unsigned)cred.uid);
	return !stat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == cred.uid;
}

static long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* The update, on the adapter that has the connection. */
static uint8_t update(const struct request *r, uint16_t got[3]) {
	for (int dev = 0; dev < MAX_DEVS; dev++) {
		int fd = socket(AF_BLUETOOTH_, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI_);
		if (fd < 0) return ST_IO;
		struct sockaddr_hci_ a = { AF_BLUETOOTH_, (unsigned short)dev, HCI_CHANNEL_RAW_ };
		if (bind(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); continue; }
		struct hci_conn_info_req_ req;
		memset(&req, 0, sizeof(req));
		memcpy(req.bdaddr, r->addr, 6);
		req.type = LE_LINK;
		if (ioctl(fd, HCIGETCONNINFO_, &req)) { close(fd); continue; }
		if (!(req.info.link_mode & HCI_LM_MASTER)) { close(fd); return ST_NOT_CENTRAL; }
		uint16_t handle = req.info.handle;
		struct hci_filter_ f = { 1u << HCI_EVENT_PKT, { 1u << EVT_CMD_STATUS, 1u << (EVT_LE_META - 32) }, 0 };
		uint8_t cmd[18], buf[260], st = ST_TIMEOUT;
		if (setsockopt(fd, SOL_HCI_, HCI_FILTER_, &f, sizeof(f))) { close(fd); return ST_IO; }
		size_t n = connparam_command(cmd, handle, r);
		if (write(fd, cmd, n) != (ssize_t)n) { close(fd); return ST_IO; }
		long end = now_ms() + WAIT_SECONDS * 1000L;
		for (long left; (left = end - now_ms()) > 0;) {
			struct pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, (int)left) <= 0) continue;
			ssize_t got_n = read(fd, buf, sizeof(buf));
			if (got_n > 0 && connparam_event(buf, (size_t)got_n, handle, &st, got)) break;
		}
		close(fd);
		return st;
	}
	return ST_NO_LINK;
}

int main(void) {
	const char *fds = getenv("LISTEN_FDS"), *pid = getenv("LISTEN_PID");
	if (!fds || atoi(fds) < 1 || !pid || atoi(pid) != getpid()) {
		fprintf(stderr, "ztos-bt-connparam: start me through ztos-bt-connparam.socket\n");
		return 1;
	}
	for (;;) {
		struct pollfd p = { LISTEN_FD, POLLIN, 0 };
		int r = poll(&p, 1, IDLE_SECONDS * 1000);
		if (r == 0) return 0;
		if (r < 0) { if (errno == EINTR) continue; return 1; }
		int c = accept4(LISTEN_FD, NULL, NULL, SOCK_CLOEXEC);
		if (c < 0) continue;
		if (!allowed(c)) { close(c); continue; }
		/* one client at a time, its requests one at a time, until it goes */
		for (;;) {
			struct pollfd q = { c, POLLIN, 0 };
			if (poll(&q, 1, IDLE_SECONDS * 1000) <= 0) break;
			uint8_t in[32], out[7] = { ST_INVALID };
			ssize_t n = recv(c, in, sizeof(in), 0);
			if (n <= 0) break;
			struct request req;
			uint16_t got[3] = { 0, 0, 0 };
			if (connparam_request(in, (size_t)n, &req) && connparam_valid(&req)) {
				out[0] = update(&req, got);
				if (out[0] == ST_OK) {
					fprintf(stderr, "ztos-bt-connparam: %02X:%02X:%02X:%02X:%02X:%02X interval %u latency %u timeout %u\n",
						req.addr[5], req.addr[4], req.addr[3], req.addr[2], req.addr[1], req.addr[0],
						got[0], got[1], got[2]);
				} else {
					fprintf(stderr, "ztos-bt-connparam: update failed: 0x%02x\n", out[0]);
				}
			}
			put16(out + 1, got[0]);
			put16(out + 3, got[1]);
			put16(out + 5, got[2]);
			if (send(c, out, sizeof(out), MSG_NOSIGNAL) != (ssize_t)sizeof(out)) break;
		}
		close(c);
	}
}
#endif
