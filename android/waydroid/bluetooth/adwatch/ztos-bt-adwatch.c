// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztos-bt-adwatch: the advertisements nearby devices actually sent, for
 * ztos-btbridge, so an Android app scanning gets the bytes a real phone would
 * give it, not a record rebuilt from BlueZ's parsed properties.
 *
 * BlueZ's D-Bus API has no raw advertising data. The kernel's management
 * interface does: its Device Found event carries the advertising data and the
 * scan response as received, the RSSI and a few flags (not connectable, scan
 * response only). Reading those events needs a trusted management socket,
 * which needs CAP_NET_ADMIN, so this is a small program of its own, not the
 * bridge (python3) with a capability.
 *
 * What it does, and all it does:
 *   - It is started by its socket (ztos-bt-adwatch.socket, a SOCK_SEQPACKET
 *     socket in /run). A client that connects must be root or a user with a
 *     session (/run/user/UID exists); others are closed at once.
 *   - While at least one client is connected, it holds a management socket
 *     open and passes on Device Found events, and nothing else: one packet
 *     each, the controller index (2 bytes, little-endian) and the event's
 *     parameters unchanged (address 6, address type 1, RSSI 1, flags 4, data
 *     length 2, data). Events that don't add up are dropped.
 *   - It never writes to the management socket: no command is ever sent, so
 *     it can't change the adapter, start or stop a scan, pair or connect.
 *     Device Found events only come while something (bluetoothd, for the
 *     bridge's discovery) is scanning.
 *   - What it reads from clients is ignored; a client that can't keep up loses
 *     packets rather than holding up the others.
 *   - With no client for IDLE_SECONDS it exits; the socket starts it again.
 *
 * The same advertisements are already readable through BlueZ's D-Bus API by
 * any local user, parsed; this passes them on unparsed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_ 1
#define HCI_DEV_NONE_ 0xffff
#define HCI_CHANNEL_CONTROL_ 3
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_HDR 6
#define FOUND_FIXED 14          /* address 6, type 1, rssi 1, flags 4, eir length 2 */
#define MAX_CLIENTS 4
#define IDLE_SECONDS 30
#define LISTEN_FD 3             /* sd_listen_fds' first */

struct sockaddr_hci_ {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A management event, if it is a well-formed Device Found: the packet to pass
 * on (index + parameters) in out, its length; 0 otherwise. */
size_t adwatch_packet(const uint8_t *buf, size_t n, uint8_t *out, size_t out_size) {
	if (n < MGMT_HDR) return 0;
	uint16_t op = le16(buf), index = le16(buf + 2), len = le16(buf + 4);
	if (op != MGMT_EV_DEVICE_FOUND || (size_t)len + MGMT_HDR != n || len < FOUND_FIXED) return 0;
	const uint8_t *ev = buf + MGMT_HDR;
	if ((size_t)le16(ev + 12) + FOUND_FIXED != len) return 0;
	if ((size_t)len + 2 > out_size) return 0;
	out[0] = (uint8_t)index;
	out[1] = (uint8_t)(index >> 8);
	memcpy(out + 2, ev, len);
	return (size_t)len + 2;
}

#ifndef ADWATCH_NO_MAIN
/* Root, or someone with a session on this machine. */
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

static int mgmt_open(void) {
	int fd = socket(AF_BLUETOOTH_, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI_);
	if (fd < 0) return -1;
	struct sockaddr_hci_ a = { .hci_family = AF_BLUETOOTH_, .hci_dev = HCI_DEV_NONE_,
		.hci_channel = HCI_CHANNEL_CONTROL_ };
	if (bind(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "ztos-bt-adwatch: management socket: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int main(void) {
	const char *fds = getenv("LISTEN_FDS"), *pid = getenv("LISTEN_PID");
	if (!fds || atoi(fds) < 1 || !pid || atoi(pid) != getpid()) {
		fprintf(stderr, "ztos-bt-adwatch: start me through ztos-bt-adwatch.socket\n");
		return 1;
	}
	int clients[MAX_CLIENTS], n_clients = 0, mgmt = -1;
	static uint8_t buf[1024], out[1024];
	int idle_ms = 0;
	for (;;) {
		struct pollfd p[2 + MAX_CLIENTS];
		int n = 0;
		p[n++] = (struct pollfd){ .fd = LISTEN_FD, .events = POLLIN };
		if (mgmt >= 0) p[n++] = (struct pollfd){ .fd = mgmt, .events = POLLIN };
		int first_client = n;
		for (int i = 0; i < n_clients; i++) p[n++] = (struct pollfd){ .fd = clients[i], .events = POLLIN };
		int r = poll(p, (nfds_t)n, 1000);
		if (r < 0 && errno != EINTR) return 1;
		if (r == 0 && !n_clients && (idle_ms += 1000) >= IDLE_SECONDS * 1000) return 0;
		if (r <= 0) continue;
		idle_ms = 0;
		if (p[0].revents & POLLIN) {
			int c = accept4(LISTEN_FD, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (c >= 0) {
				if (n_clients < MAX_CLIENTS && allowed(c)) {
					clients[n_clients++] = c;
					if (mgmt < 0) mgmt = mgmt_open();
				} else {
					close(c);
				}
			}
		}
		/* clients: anything they send is read and ignored; gone ones are closed */
		for (int i = first_client, k = 0; i < n; i++, k++) {
			if (!p[i].revents) continue;
			ssize_t got = (p[i].revents & POLLIN) ? recv(p[i].fd, buf, sizeof(buf), MSG_DONTWAIT) : 0;
			if (got <= 0 && (got == 0 || (errno != EAGAIN && errno != EINTR))) {
				close(p[i].fd);
				for (int j = 0; j < n_clients; j++) {
					if (clients[j] == p[i].fd) { clients[j] = clients[--n_clients]; break; }
				}
			}
			(void)k;
		}
		if (mgmt >= 0 && first_client == 2 && (p[1].revents & POLLIN)) {
			ssize_t got;
			while ((got = recv(mgmt, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
				size_t len = adwatch_packet(buf, (size_t)got, out, sizeof(out));
				for (int j = 0; len && j < n_clients; j++)
					(void)send(clients[j], out, len, MSG_DONTWAIT | MSG_NOSIGNAL);
			}
		}
		if (!n_clients && mgmt >= 0) {
			close(mgmt);
			mgmt = -1;
		}
	}
}
#endif
