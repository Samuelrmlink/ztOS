// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztos-podsd: AirPods, for the rest of ztOS. One per session.
 *
 * It keeps an AAP channel to every pair BlueZ says is connected, and listens in short
 * bursts for their BLE broadcasts when they are not, so the batteries show either way.
 * Everything about bytes and state is in pods-model.h; this file is only the I/O.
 *
 *   $XDG_RUNTIME_DIR/ztos-pods-state.json   the state, whole, for ztpods
 *   $XDG_RUNTIME_DIR/ztos-pods              the short form, for the status bar and Settings
 *   $XDG_RUNTIME_DIR/ztos-pods.sock         commands, one datagram each; a sender with a
 *                                           bound address gets "ok" or "err WHY":
 *       set MAC listening_mode off|anc|transparency|adaptive
 *       set MAC KEY VALUE        a setting of PODS_SETTINGS
 *       rename MAC NAME
 *       forget MAC               its keys and readings
 *       scan-hint on             an app is showing this: lasts PODS_HINT_S, send it again
 *   ~/.local/share/ztos/pods/keys       the pairs' keys, mode 0600, never logged
 *   ~/.local/state/ztos/pods/readings   last readings, and what was last set
 *
 * It never powers Bluetooth, connects, pairs or changes anything in BlueZ, and never
 * starts bluetoothd: every call carries NoAutoStart. An L2CAP connect pages the device,
 * so a channel is opened only to a pair BlueZ reports connected, asked again just before.
 *
 * ZTOS_PODSD_TRACE=FILE  appends every packet in hex, for making test data out of real
 *   pods. The key packet and device information (serials) go in as their length only.
 * ZTOS_PODSD_TEST_AAP_DIR=DIR  tests only: a channel is AF_UNIX SEQPACKET to DIR/MAC.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "pods-model.h"

/* from <bluetooth/l2cap.h>, so that building this needs no bluez headers */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define SOL_L2CAP 6
#define L2CAP_CONNINFO 2
#define BDADDR_BREDR 0
struct sockaddr_l2_ {
	sa_family_t l2_family;
	uint16_t l2_psm;
	uint8_t l2_bdaddr[6];
	uint16_t l2_cid;
	uint8_t l2_bdaddr_type;
};
struct l2cap_conninfo_ { uint16_t hci_handle; uint8_t dev_class[3]; };

#define PODS_HINT_S 25
#define CONNECT_LIMIT_MS 45000		/* the kernel gives up at 40 s */
#define HANDSHAKE_LIMIT_MS 6000		/* it has taken 1.2 s on a fresh link */
#define BACKOFF_MAX_S 60
#define READINGS_SAVE_MS 60000		/* flash, not tmpfs: not at every advert */
#define HEARTBEAT_MS 30000

static const char BLUEZ[] = "org.bluez", DEVICE1[] = "org.bluez.Device1", ADAPTER1[] = "org.bluez.Adapter1",
	TRANSPORT1[] = "org.bluez.MediaTransport1";

enum io_state { IO_IDLE, IO_ASKING, IO_CONNECTING, IO_UP };

/* one per pair: the channel. Never let go of while its connect is pending: that leaves
 * the pods holding a channel we forgot, and when they close it the live link goes too. */
struct pod_io {
	char mac[18];
	bool used, airpods, connected;
	char bluez_name[AAP_MAX_NAME + 1];
	char path[128];
	enum io_state state;
	int fd;
	sd_event_source *src;
	uint64_t connect_at, ready_at, retry_at;
	int backoff_s;
};

static sd_bus *bus;
static sd_event *event;
static sd_event_source *timer;
static struct pods_world world;
static struct pod_io io[PODS_MAX];
static char runtime[200], keys_path[300], readings_path[300];
static char adapter[64];
static bool powered, discovering, bluez_up;
static bool dirty, readings_dirty;
static uint64_t readings_saved;
static long hint_until;
static bool scan_wanted, scan_started, scan_pending;
static uint64_t scan_next, scan_end;
static char transports[8][128];
static FILE *trace;

static uint64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("ztos-podsd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* Run the tick as soon as this callback is over: it does what is due and sets the next. */
static void kick(void) {
	if (timer) {
		sd_event_source_set_time(timer, 0);
		sd_event_source_set_enabled(timer, SD_EVENT_ONESHOT);
	}
}

static void trace_packet(const char *dir, const char *mac, const uint8_t *b, size_t n) {
	if (!trace) return;
	unsigned op;
	const uint8_t *p;
	size_t len;
	bool secret = aap_split(b, n, &op, &p, &len) && (op == AAP_OP_KEYS || op == AAP_OP_INFO);
	fprintf(trace, "%ld %s %s ", (long)time(NULL), dir, mac);
	if (secret) {
		fprintf(trace, "op=%02x len=%zu (not recorded)", op, n);
	} else {
		for (size_t i = 0; i < n; i++) fprintf(trace, "%02x", b[i]);
	}
	fputc('\n', trace);
	fflush(trace);
}

/* ---- files ---- */

static void mkdirs(const char *path) {
	char p[300];
	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0700);
			*s = '/';
		}
	}
}

/* Replaced whole, so that a reader never sees half of it. */
static bool write_atomic(const char *path, mode_t mode, const char *data, size_t n) {
	char tmp[320];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) return false;
	bool ok = fchmod(fd, mode) == 0 && write(fd, data, n) == (ssize_t)n;
	close(fd);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static void save_with(const char *path, void (*writer)(FILE *, const struct pods_world *)) {
	char *data = NULL;
	size_t n = 0;
	FILE *f = open_memstream(&data, &n);
	if (!f) return;
	writer(f, &world);
	fclose(f);
	mkdirs(path);
	if (!write_atomic(path, 0600, data, n)) say("could not write %s: %s", path, strerror(errno));
	free(data);
}

static void save_readings(void) {
	save_with(readings_path, pods_readings_write);
	readings_dirty = false;
	readings_saved = now_ms();
}

static void load_files(void) {
	char line[400];
	FILE *f = fopen(keys_path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		pods_keys_line(&world, line);
	}
	if (f) fclose(f);
	f = fopen(readings_path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		pods_readings_line(&world, line);
	}
	if (f) fclose(f);
}

static struct pod_io *io_find(const char *mac, bool create) {
	struct pod_io *free_slot = NULL;
	for (int i = 0; i < PODS_MAX; i++) {
		if (io[i].used && !strcmp(io[i].mac, mac)) return &io[i];
		if (!io[i].used && !free_slot) free_slot = &io[i];
	}
	if (!create || !free_slot) return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->fd = -1;
	snprintf(free_slot->mac, sizeof(free_slot->mac), "%s", mac);
	return free_slot;
}

static const char *bluez_name_of(const char *mac) {
	struct pod_io *i = io_find(mac, false);
	return i ? i->bluez_name : NULL;
}

static void publish_file(const char *name, char **last, void (*fill)(FILE *)) {
	char *data = NULL, path[300];
	size_t n = 0;
	FILE *f = open_memstream(&data, &n);
	if (!f) return;
	fill(f);
	fclose(f);
	if (*last && !strcmp(*last, data)) {
		free(data);
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", runtime, name);
	write_atomic(path, 0600, data, n);
	free(*last);
	*last = data;
}

static void fill_state(FILE *f) { pods_json(f, &world, powered, scan_started, (long)time(NULL), bluez_name_of); }
static void fill_bar(FILE *f) { pods_bar_write(f, &world); }

static void publish(void) {
	static char *last_state, *last_bar;
	publish_file("ztos-pods-state.json", &last_state, fill_state);
	publish_file("ztos-pods", &last_bar, fill_bar);
	dirty = false;
}

/* ---- the channel ---- */

static int on_channel(sd_event_source *s, int fd, uint32_t revents, void *data);

static void send_out(struct pod_io *i, const struct pods_out *o, bool *failed) {
	for (int k = 0; k < o->n; k++) {
		trace_packet("tx", i->mac, o->pkt[k], o->len[k]);
		if (send(i->fd, o->pkt[k], o->len[k], MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
			say("%s: send: %s", i->mac, strerror(errno));
			if (failed) *failed = true;
		}
	}
}

static void channel_close(struct pod_io *i, bool failed) {
	struct pods_device *d = pods_find(&world, i->mac);
	if (i->src) i->src = sd_event_source_unref(i->src);
	if (i->fd >= 0) {
		shutdown(i->fd, SHUT_RDWR);
		close(i->fd);
	}
	i->fd = -1;
	bool had_link = i->state == IO_UP;
	i->state = IO_IDLE;
	if (d && had_link) pods_link_gone(d, i->connected);
	else if (d) d->link = i->connected ? PODS_LINK_CONNECTED : PODS_LINK_NONE;
	if (failed) {
		i->backoff_s = i->backoff_s ? (i->backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : i->backoff_s * 2) : 2;
		i->retry_at = now_ms() + (uint64_t)i->backoff_s * 1000;
	} else {
		i->retry_at = 0;
	}
	readings_dirty = readings_dirty || had_link;
	dirty = true;
}

static void channel_up(struct pod_io *i) {
	struct pods_device *d = pods_find(&world, i->mac);
	/* BlueZ said no meanwhile: the connect had to finish before we could let go of it */
	if (!d || !i->connected) {
		channel_close(i, false);
		return;
	}
	struct pods_out o = { 0 };
	if (sd_event_add_io(event, &i->src, i->fd, EPOLLIN, on_channel, i) < 0) {
		channel_close(i, true);
		return;
	}
	i->state = IO_UP;
	i->ready_at = 0;
	i->connect_at = now_ms();
	pods_link_open(d, (uint32_t)now_ms(), &o);
	bool failed = false;
	send_out(i, &o, &failed);
	say("%s: AAP channel up", i->mac);
	if (failed) channel_close(i, true);
	dirty = true;
}

/* A connect that is pending looks like nothing at all: polled until it is one thing or the other. */
static void channel_check(struct pod_io *i) {
	int err = 0;
	socklen_t len = sizeof(err);
	if (getsockopt(i->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
	if (err) {
		say("%s: AAP connect: %s", i->mac, strerror(err));
		channel_close(i, i->connected);
		return;
	}
	struct l2cap_conninfo_ info;
	len = sizeof(info);
	struct sockaddr_storage local;
	socklen_t alen = sizeof(local);
	bool up;
	if (getsockname(i->fd, (struct sockaddr *)&local, &alen) == 0 && local.ss_family == AF_UNIX) up = true;
	else up = getsockopt(i->fd, SOL_L2CAP, L2CAP_CONNINFO, &info, &len) == 0;
	if (up) {
		channel_up(i);
	} else if (now_ms() - i->connect_at > CONNECT_LIMIT_MS) {
		say("%s: AAP connect: gave up", i->mac);
		channel_close(i, i->connected);
	}
}

static void channel_connect(struct pod_io *i) {
	const char *test_dir = getenv("ZTOS_PODSD_TEST_AAP_DIR");
	int r;
	if (test_dir) {
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", test_dir, i->mac);
		i->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		r = i->fd < 0 ? -1 : connect(i->fd, (struct sockaddr *)&a, sizeof(a));
	} else {
		struct sockaddr_l2_ a = { .l2_family = AF_BLUETOOTH, .l2_psm = htole16(AAP_PSM), .l2_bdaddr_type = BDADDR_BREDR };
		uint8_t mac[6];
		if (!pods_mac_parse(i->mac, mac)) return;
		for (int b = 0; b < 6; b++) a.l2_bdaddr[b] = mac[5 - b];	/* least significant first */
		i->fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
		r = i->fd < 0 ? -1 : connect(i->fd, (struct sockaddr *)&a, sizeof(a));
	}
	if (i->fd < 0 || (r < 0 && errno != EINPROGRESS && errno != EAGAIN)) {
		say("%s: AAP connect: %s", i->mac, strerror(errno));
		i->state = IO_IDLE;
		channel_close(i, true);
		return;
	}
	i->state = IO_CONNECTING;
	i->connect_at = now_ms();
	kick();
}

static void apply_connected(struct pod_io *i, struct pods_device *d, bool connected);

/* A last word from BlueZ before anything pages the pair. */
static int on_connected_answer(sd_bus_message *m, void *data, sd_bus_error *ret) {
	struct pod_io *i = data;
	int connected = 0;
	if (i->state != IO_ASKING) return 0;
	i->state = IO_IDLE;
	if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "v", "b", &connected) < 0) {
		i->retry_at = now_ms() + 2000;	/* no answer is no permission: asked again */
	} else if (connected) {
		channel_connect(i);
	} else {
		apply_connected(i, pods_find(&world, i->mac), false);
	}
	kick();
	return 0;
}

static void channel_ask(struct pod_io *i) {
	sd_bus_message *m = NULL;
	if (!i->path[0] || sd_bus_message_new_method_call(bus, &m, BLUEZ, i->path, "org.freedesktop.DBus.Properties", "Get") < 0) return;
	sd_bus_message_set_auto_start(m, 0);
	if (sd_bus_message_append(m, "ss", DEVICE1, "Connected") >= 0 && sd_bus_call_async(bus, NULL, m, on_connected_answer, i, 5000000) >= 0) {
		i->state = IO_ASKING;
	}
	sd_bus_message_unref(m);
}

static int on_channel(sd_event_source *s, int fd, uint32_t revents, void *data) {
	struct pod_io *i = data;
	struct pods_device *d = pods_find(&world, i->mac);
	uint8_t buf[2048];
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
		if (n <= 0) {
			say("%s: AAP channel closed%s%s", i->mac, n < 0 ? ": " : "", n < 0 ? strerror(errno) : "");
			channel_close(i, i->connected);
			kick();
			return 0;
		}
		trace_packet("rx", i->mac, buf, (size_t)n);
		if (!d) continue;
		struct pods_out o = { 0 };
		int ev = pods_link_packet(d, (uint32_t)now_ms(), (long)time(NULL), buf, (size_t)n, &o);
		bool failed = false;
		send_out(i, &o, &failed);
		if (ev & PODS_EV_CHANGED) dirty = true;
		if (ev & PODS_EV_SAVE) readings_dirty = true;
		if (ev & PODS_EV_KEYS) {
			say("%s: keys stored", i->mac);
			save_with(keys_path, pods_keys_write);
		}
		if (d->link == PODS_LINK_READY && !i->ready_at) i->ready_at = now_ms();
		if (failed) {
			channel_close(i, i->connected);
			break;
		}
	}
	kick();
	return 0;
}

/* ---- discovery, in bursts ---- */

static sd_bus_message *adapter_call(const char *method) {
	sd_bus_message *m = NULL;
	if (!adapter[0] || sd_bus_message_new_method_call(bus, &m, BLUEZ, adapter, ADAPTER1, method) < 0) return NULL;
	sd_bus_message_set_auto_start(m, 0);
	return m;
}

static int read_objects(sd_bus_message *m);

static int on_objects(sd_bus_message *m, void *data, sd_bus_error *ret) {
	if (sd_bus_message_is_method_error(m, NULL)) return 0;
	read_objects(m);
	kick();
	return 0;
}

static void get_objects(void) {
	sd_bus_message *m = NULL;
	if (sd_bus_message_new_method_call(bus, &m, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects") < 0) return;
	sd_bus_message_set_auto_start(m, 0);
	sd_bus_call_async(bus, NULL, m, on_objects, NULL, 10000000);
	sd_bus_message_unref(m);
}

static void discovery_stop(void);

/* InProgress is not "started": BlueZ also says it when the kernel refused, and drops us. */
static int on_started(sd_bus_message *m, void *data, sd_bus_error *ret) {
	scan_pending = false;
	if (sd_bus_message_is_method_error(m, NULL)) {
		const sd_bus_error *e = sd_bus_message_get_error(m);
		say("discovery: %s", e && e->name ? e->name : "refused");
		scan_wanted = false;
		return 0;
	}
	scan_started = true;
	dirty = true;
	if (!scan_wanted) discovery_stop();
	else get_objects();	/* another client discovering: what it has already heard */
	kick();
	return 0;
}

static int on_filter(sd_bus_message *m, void *data, sd_bus_error *ret) {
	sd_bus_message *call = adapter_call("StartDiscovery");
	if (sd_bus_message_is_method_error(m, NULL) || !call || !scan_wanted ||
			sd_bus_call_async(bus, NULL, call, on_started, NULL, 10000000) < 0) {
		scan_pending = false;
		scan_wanted = false;
	}
	sd_bus_message_unref(call);
	return 0;
}

static void discovery_start(void) {
	sd_bus_message *m = adapter_call("SetDiscoveryFilter");
	if (!m) return;
	if (sd_bus_message_append(m, "a{sv}", 2, "Transport", "s", "le", "DuplicateData", "b", 1) >= 0 &&
			sd_bus_call_async(bus, NULL, m, on_filter, NULL, 10000000) >= 0) {
		scan_wanted = scan_pending = true;
	}
	sd_bus_message_unref(m);
}

static void discovery_stop(void) {
	scan_wanted = false;
	if (!scan_started) return;
	scan_started = false;
	dirty = true;
	sd_bus_message *m = adapter_call("StopDiscovery");
	if (m) {
		sd_bus_call_async(bus, NULL, m, NULL, NULL, 10000000);
		sd_bus_message_unref(m);
	}
}

static bool screen_on(void) {
	char path[300], line[64];
	snprintf(path, sizeof(path), "%s/ztmetro.state", runtime);
	FILE *f = fopen(path, "r");
	bool on = false;
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "screen ", 7)) on = !strncmp(line + 7, "on", 2);
	}
	if (f) fclose(f);
	return on;
}

static bool audio_active(void) {
	for (size_t i = 0; i < sizeof(transports) / sizeof(transports[0]); i++) {
		if (transports[i][0]) return true;
	}
	return false;
}

static struct pods_scan_plan scan_plan(void) {
	struct pods_scan_in s = { .powered = powered && bluez_up && adapter[0], .screen_on = screen_on(),
		.ui_open = time(NULL) < hint_until, .audio_active = audio_active(), .known = world.n };
	for (int i = 0; i < world.n; i++) {
		if (world.dev[i].link == PODS_LINK_READY) s.linked++;
		else if (world.dev[i].has_keys) s.keyed_unlinked++;
	}
	return pods_scan_policy(&s);
}

/* ---- what BlueZ says ---- */

struct dev_props {
	const char *address, *alias, *name, *modalias;
	int connected;		/* -1 not said */
	bool airpods;
	const uint8_t *mfr;
	size_t mfr_len;
};

static void apply_connected(struct pod_io *i, struct pods_device *d, bool connected) {
	bool was = i->connected;
	i->connected = connected;
	if (connected == was) return;
	say("%s: BlueZ says %s", i->mac, connected ? "connected" : "not connected");
	if (connected) {
		i->retry_at = 0;
		i->backoff_s = 0;
		if (d && d->link == PODS_LINK_NONE) d->link = PODS_LINK_CONNECTED;
	} else if (i->state == IO_UP) {
		channel_close(i, false);
	} else if (d && i->state != IO_CONNECTING) {
		d->link = PODS_LINK_NONE;
	}
	dirty = true;
}

static void apply_device(const char *path, const struct dev_props *p, bool snapshot) {
	char mac[18];
	if (!pods_path_mac(path, mac)) return;
	struct pods_device *d = pods_find(&world, mac);
	if (p->airpods && !d) {
		d = pods_add(&world, mac);
		if (!d) say("%s: no room for another pair", mac);
		dirty = true;
	}
	if (d) {
		struct pod_io *i = io_find(mac, true);
		if (i) {
			snprintf(i->path, sizeof(i->path), "%s", path);
			if (p->alias) snprintf(i->bluez_name, sizeof(i->bluez_name), "%s", p->alias);
			else if (p->name && !i->bluez_name[0]) snprintf(i->bluez_name, sizeof(i->bluez_name), "%s", p->name);
			if (p->airpods) i->airpods = true;
			unsigned product = pods_modalias_product(p->modalias);
			if (product && product != d->product) {
				d->product = product;
				dirty = readings_dirty = true;
			}
			if (!d->in_bluez) {
				d->in_bluez = true;
				dirty = true;
			}
			/* only a pair known by its service; not an advert from a pod's own RPA */
			if (p->connected >= 0) apply_connected(i, d, p->connected);
		}
	}
	/* a snapshot's advert may be old, unless someone is discovering right now */
	if (p->mfr && (!snapshot || discovering)) {
		uint8_t addr[6];
		int which = -1;
		if (!pods_mac_parse(mac, addr)) return;
		long now = (long)time(NULL);
		int r = pods_advert(&world, addr, p->mfr, p->mfr_len, now, &which);
		if (trace && r != PODS_ADV_IGNORED) {
			const uint8_t *q = ble_find_proximity(p->mfr, p->mfr_len);
			fprintf(trace, "%ld adv %s result=%d clear=", now, mac, r);
			for (int b = 0; q && b < 9; b++) fprintf(trace, "%02x", q[b]);
			fputc('\n', trace);
			fflush(trace);
		}
		if (r == PODS_ADV_MISMATCH) say("an advert resolved to a pair we know and did not open");
		if (r == PODS_ADV_READING || r == PODS_ADV_NEARBY) dirty = true;
		if (r == PODS_ADV_READING) readings_dirty = true;
	}
}

static void transport_state(const char *path, const char *state) {
	int empty = -1, at = -1;
	for (int i = 0; i < (int)(sizeof(transports) / sizeof(transports[0])); i++) {
		if (!strcmp(transports[i], path)) at = i;
		if (!transports[i][0]) empty = i;
	}
	if (state && !strcmp(state, "active")) {
		if (at < 0 && empty >= 0) snprintf(transports[empty], sizeof(transports[empty]), "%s", path);
	} else if (at >= 0) {
		transports[at][0] = 0;
	}
}

/* An a{sv} of one of the interfaces we care about; anything else is skipped. */
static int read_props(const char *path, const char *iface, sd_bus_message *m, bool snapshot) {
	struct dev_props p = { .connected = -1 };
	const char *transport = NULL;
	int adapter_powered = -1, adapter_discovering = -1;
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) return r;
		int b;
		const char *s;
		if (!strcmp(iface, ADAPTER1) && !strcmp(key, "Powered")) {
			if (sd_bus_message_read(m, "v", "b", &b) >= 0) adapter_powered = b;
		} else if (!strcmp(iface, ADAPTER1) && !strcmp(key, "Discovering")) {
			if (sd_bus_message_read(m, "v", "b", &b) >= 0) adapter_discovering = b;
		} else if (!strcmp(iface, TRANSPORT1) && !strcmp(key, "State")) {
			if (sd_bus_message_read(m, "v", "s", &s) >= 0) transport = s;
		} else if (strcmp(iface, DEVICE1)) {
			sd_bus_message_skip(m, "v");
		} else if (!strcmp(key, "Connected")) {
			if (sd_bus_message_read(m, "v", "b", &b) >= 0) p.connected = b;
		} else if (!strcmp(key, "Alias")) {
			sd_bus_message_read(m, "v", "s", &p.alias);
		} else if (!strcmp(key, "Name")) {
			sd_bus_message_read(m, "v", "s", &p.name);
		} else if (!strcmp(key, "Modalias")) {
			sd_bus_message_read(m, "v", "s", &p.modalias);
		} else if (!strcmp(key, "UUIDs")) {
			if ((r = sd_bus_message_enter_container(m, 'v', "as")) < 0) return r;
			if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) return r;
			while (sd_bus_message_read(m, "s", &s) > 0) {
				if (!strcasecmp(s, PODS_SERVICE_UUID)) p.airpods = true;
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else if (!strcmp(key, "ManufacturerData")) {
			if ((r = sd_bus_message_enter_container(m, 'v', "a{qv}")) < 0) return r;
			if ((r = sd_bus_message_enter_container(m, 'a', "{qv}")) < 0) return r;
			while (sd_bus_message_enter_container(m, 'e', "qv") > 0) {
				uint16_t company;
				sd_bus_message_read(m, "q", &company);
				if (company == APPLE_COMPANY_ID && sd_bus_message_enter_container(m, 'v', "ay") > 0) {
					const void *bytes;
					size_t len;
					if (sd_bus_message_read_array(m, 'y', &bytes, &len) >= 0) {
						p.mfr = bytes;
						p.mfr_len = len;
					}
					sd_bus_message_exit_container(m);
				} else {
					sd_bus_message_skip(m, "v");
				}
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		if ((r = sd_bus_message_exit_container(m)) < 0) return r;
	}
	if (r < 0) return r;
	r = sd_bus_message_exit_container(m);
	if (!strcmp(iface, ADAPTER1)) {
		if (!adapter[0]) snprintf(adapter, sizeof(adapter), "%s", path);
		if (!strcmp(adapter, path)) {
			if (adapter_powered >= 0 && (bool)adapter_powered != powered) {
				powered = adapter_powered;
				dirty = true;
				if (!powered) scan_started = scan_wanted = false;	/* it ended them all */
			}
			if (adapter_discovering >= 0) discovering = adapter_discovering;
		}
	} else if (!strcmp(iface, DEVICE1)) {
		apply_device(path, &p, snapshot);
	} else if (!strcmp(iface, TRANSPORT1) && transport) {
		transport_state(path, transport);
	}
	return r;
}

/* {interface: properties}, as InterfacesAdded and GetManagedObjects have them */
static int read_interfaces(const char *path, sd_bus_message *m, bool snapshot) {
	int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface;
		if ((r = sd_bus_message_read(m, "s", &iface)) < 0) return r;
		if ((r = read_props(path, iface, m, snapshot)) < 0) return r;
		if ((r = sd_bus_message_exit_container(m)) < 0) return r;
	}
	if (r < 0) return r;
	return sd_bus_message_exit_container(m);
}

static int read_objects(sd_bus_message *m) {
	int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path;
		if ((r = sd_bus_message_read(m, "o", &path)) < 0) return r;
		if ((r = read_interfaces(path, m, true)) < 0) return r;
		if ((r = sd_bus_message_exit_container(m)) < 0) return r;
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

static int on_added(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *path;
	if (sd_bus_message_read(m, "o", &path) >= 0) read_interfaces(path, m, false);
	kick();
	return 0;
}

static int on_removed(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *path, *iface;
	if (sd_bus_message_read(m, "o", &path) < 0 || sd_bus_message_enter_container(m, 'a', "s") < 0) return 0;
	while (sd_bus_message_read(m, "s", &iface) > 0) {
		if (!strcmp(iface, ADAPTER1) && !strcmp(path, adapter)) {
			adapter[0] = 0;
			powered = discovering = scan_started = scan_wanted = false;
			dirty = true;
		} else if (!strcmp(iface, TRANSPORT1)) {
			transport_state(path, NULL);
		} else if (!strcmp(iface, DEVICE1)) {
			char mac[18];
			struct pods_device *d = pods_path_mac(path, mac) ? pods_find(&world, mac) : NULL;
			/* The keys stay until the user asks: an adapter that goes (btattach restarting)
			 * takes its devices with it, and that forgets nothing. */
			if (d) {
				struct pod_io *i = io_find(mac, true);
				if (i) {
					apply_connected(i, d, false);
					i->path[0] = 0;
				}
				d->in_bluez = false;
				dirty = true;
			}
		}
	}
	kick();
	return 0;
}

static int on_changed(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *iface;
	if (sd_bus_message_read(m, "s", &iface) >= 0) read_props(sd_bus_message_get_path(m), iface, m, false);
	kick();
	return 0;
}

/* bluetoothd went or came: nothing else tells us. What it held of ours has ended. */
static void bluez_gone(void) {
	bluez_up = powered = discovering = scan_started = scan_wanted = scan_pending = false;
	adapter[0] = 0;
	memset(transports, 0, sizeof(transports));
	for (int k = 0; k < PODS_MAX; k++) {
		if (!io[k].used) continue;
		io[k].path[0] = 0;
		struct pods_device *d = pods_find(&world, io[k].mac);
		if (d) d->in_bluez = false;
		apply_connected(&io[k], d, false);
	}
	dirty = true;
}

static int on_owner(sd_bus_message *m, void *data, sd_bus_error *ret) {
	const char *name, *from, *to;
	if (sd_bus_message_read(m, "sss", &name, &from, &to) < 0 || strcmp(name, BLUEZ)) return 0;
	bluez_gone();
	if (to[0]) {
		say("bluetoothd is on the bus");
		bluez_up = true;
		get_objects();
	} else {
		say("bluetoothd has left the bus");
	}
	kick();
	return 0;
}

static int on_has_owner(sd_bus_message *m, void *data, sd_bus_error *ret) {
	int has = 0;
	if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read(m, "b", &has) >= 0 && has) {
		bluez_up = true;
		get_objects();
	}
	kick();
	return 0;
}

/* ---- commands ---- */

static const char *command(char *line) {
	char *save = NULL;
	char *verb = strtok_r(line, " \n", &save);
	if (!verb) return "empty";
	if (!strcmp(verb, "scan-hint")) {
		char *arg = strtok_r(NULL, " \n", &save);
		bool on = arg && !strcmp(arg, "on");
		hint_until = on ? (long)time(NULL) + PODS_HINT_S : 0;
		if (on && scan_next > now_ms() + 1000) scan_next = now_ms();
		return NULL;
	}
	char *mac = strtok_r(NULL, " \n", &save);
	struct pods_device *d = mac ? pods_find(&world, mac) : NULL;
	if (!d) return "unknown-device";
	struct pod_io *i = io_find(mac, false);
	struct pods_out o = { 0 };
	const char *why;
	if (!strcmp(verb, "set")) {
		char *key = strtok_r(NULL, " \n", &save), *value = strtok_r(NULL, " \n", &save);
		if (!key || !value) return "usage";
		why = !strcmp(key, "listening_mode") ? pods_cmd_mode(d, (uint32_t)now_ms(), value, &o) : pods_cmd_set(d, key, value, &o);
		readings_dirty = readings_dirty || !why;
	} else if (!strcmp(verb, "rename")) {
		char *name = save;	/* the rest of the line, spaces and all */
		if (!name) return "usage";
		name[strcspn(name, "\n")] = 0;
		why = pods_cmd_rename(d, name, &o);
		readings_dirty = readings_dirty || !why;
	} else if (!strcmp(verb, "forget")) {
		if (i && i->connected) return "connected";
		if (i && i->state != IO_IDLE) return "busy";
		pods_remove(&world, mac);
		if (i) i->used = false;
		save_with(keys_path, pods_keys_write);
		save_readings();
		dirty = true;
		return NULL;
	} else {
		return "unknown-command";
	}
	if (why) return why;
	if (!i || i->state != IO_UP) return "not-linked";
	bool failed = false;
	send_out(i, &o, &failed);
	dirty = true;
	return failed ? "send-failed" : NULL;
}

static int on_command(sd_event_source *s, int fd, uint32_t revents, void *data) {
	char buf[512];
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
	if (n <= 0) return 0;
	buf[n] = 0;
	const char *why = command(buf);
	if (fromlen > sizeof(sa_family_t)) {
		char reply[64];
		snprintf(reply, sizeof(reply), why ? "err %s" : "ok", why);
		sendto(fd, reply, strlen(reply), MSG_DONTWAIT, (struct sockaddr *)&from, fromlen);
	}
	kick();
	return 0;
}

/* ---- the clock ---- */

static uint64_t earlier(uint64_t a, uint64_t b) { return !a ? b : !b ? a : a < b ? a : b; }

static int on_tick(sd_event_source *s, uint64_t usec, void *data) {
	uint64_t now = now_ms(), next = 0;
	bool anything = world.n > 0 || time(NULL) < hint_until;

	for (int k = 0; k < PODS_MAX; k++) {
		struct pod_io *i = &io[k];
		if (!i->used) continue;
		struct pods_device *d = pods_find(&world, i->mac);
		if (i->state == IO_CONNECTING) {
			channel_check(i);
			if (i->state == IO_CONNECTING) next = earlier(next, now + 100);
		}
		if (i->state == IO_UP && d) {
			if (d->link == PODS_LINK_WAIT_ANSWER && now > i->connect_at + HANDSHAKE_LIMIT_MS) {
				say("%s: no answer to the handshake", i->mac);
				channel_close(i, i->connected);
			} else if (pods_tick(d, (uint32_t)now)) {
				dirty = true;
			}
			if (i->state == IO_UP && d->link == PODS_LINK_READY && i->ready_at && i->backoff_s && now > i->ready_at + 10000) i->backoff_s = 0;
			if (d->link == PODS_LINK_WAIT_ANSWER) next = earlier(next, i->connect_at + HANDSHAKE_LIMIT_MS + 1);
			if (d->mode_pending) next = earlier(next, now + PODS_OFF_DEBOUNCE_MS);
			if (d->link == PODS_LINK_READY && !d->settings_known) next = earlier(next, now + 1000);
		}
		if (i->state == IO_IDLE && i->connected && d && bluez_up) {
			if (now >= i->retry_at) channel_ask(i);
			else next = earlier(next, i->retry_at);
		}
	}

	/* the bursts */
	struct pods_scan_plan plan = bluez_up ? scan_plan() : (struct pods_scan_plan){ 0, 0 };
	if ((scan_started || scan_wanted) && (!plan.on_s || now >= scan_end)) {
		if (!scan_pending || scan_started) discovery_stop();
		else scan_wanted = false;
	} else if (plan.on_s && !scan_started && !scan_wanted && !scan_pending && now >= scan_next) {
		scan_end = now + (uint64_t)plan.on_s * 1000;
		scan_next = now + (uint64_t)plan.every_s * 1000;
		discovery_start();
	}
	if (plan.on_s) {
		next = earlier(next, scan_started || scan_wanted ? scan_end : scan_next);
	}

	if (dirty) publish();
	if (readings_dirty && now >= readings_saved + READINGS_SAVE_MS) save_readings();
	if (readings_dirty) next = earlier(next, readings_saved + READINGS_SAVE_MS);

	/* with nothing to look after, no timer: the phone sleeps through it */
	if (anything) next = earlier(next, now + HEARTBEAT_MS);
	if (next) {
		sd_event_source_set_time(s, next <= now ? 0 : next * 1000);
		sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
	}
	return 0;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *data) {
	sd_event_exit(event, 0);
	return 0;
}

int main(void) {
	const char *rt = getenv("XDG_RUNTIME_DIR"), *home = getenv("HOME");
	const char *data_home = getenv("XDG_DATA_HOME"), *state_home = getenv("XDG_STATE_HOME");
	snprintf(runtime, sizeof(runtime), "%s", rt ? rt : "/tmp");
	if (data_home) snprintf(keys_path, sizeof(keys_path), "%s/ztos/pods/keys", data_home);
	else snprintf(keys_path, sizeof(keys_path), "%s/.local/share/ztos/pods/keys", home ? home : "");
	if (state_home) snprintf(readings_path, sizeof(readings_path), "%s/ztos/pods/readings", state_home);
	else snprintf(readings_path, sizeof(readings_path), "%s/.local/state/ztos/pods/readings", home ? home : "");
	const char *trace_path = getenv("ZTOS_PODSD_TRACE");
	if (trace_path) trace = fopen(trace_path, "a");

	/* Two of these would open two channels to one pair, which the pods do not forgive. */
	char path[300];
	snprintf(path, sizeof(path), "%s/ztos-pods.lock", runtime);
	int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
		say("already running");
		return 0;
	}

	load_files();

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);

	int r = sd_event_default(&event);
	if (r < 0) return 1;
	sd_event_add_signal(event, NULL, SIGTERM, on_signal, NULL);
	sd_event_add_signal(event, NULL, SIGINT, on_signal, NULL);

	snprintf(path, sizeof(path), "%s/ztos-pods.sock", runtime);
	unlink(path);
	int cmd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%.100s", path);
	mode_t old = umask(0077);
	if (cmd < 0 || bind(cmd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		say("could not bind %s: %s", path, strerror(errno));
		return 1;
	}
	umask(old);
	sd_event_add_io(event, NULL, cmd, EPOLLIN, on_command, NULL);

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		say("no system bus: %s", strerror(-r));
		return 1;
	}
	sd_bus_set_exit_on_disconnect(bus, 1);	/* and systemd starts us again */
	sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
	const char *om = "org.freedesktop.DBus.ObjectManager", *dbus_name = "org.freedesktop.DBus";
	sd_bus_match_signal_async(bus, NULL, BLUEZ, NULL, om, "InterfacesAdded", on_added, NULL, NULL);
	sd_bus_match_signal_async(bus, NULL, BLUEZ, NULL, om, "InterfacesRemoved", on_removed, NULL, NULL);
	sd_bus_match_signal_async(bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", on_changed, NULL, NULL);
	sd_bus_match_signal_async(bus, NULL, dbus_name, "/org/freedesktop/DBus", dbus_name, "NameOwnerChanged", on_owner, NULL, NULL);
	/* whether it is there at all: asking the bus starts nothing */
	sd_bus_call_method_async(bus, NULL, dbus_name, "/org/freedesktop/DBus", dbus_name, "NameHasOwner", on_has_owner, NULL, "s", BLUEZ);

	sd_event_add_time(event, &timer, CLOCK_MONOTONIC, 0, 1000, on_tick, NULL);
	sd_event_source_set_enabled(timer, SD_EVENT_ONESHOT);

	dirty = true;
	r = sd_event_loop(event);

	/* A channel that is pending is left for the process's end to close: nothing better
	 * is open to us. An open one is closed; BlueZ ends our discovery when we leave the bus. */
	for (int k = 0; k < PODS_MAX; k++) {
		if (io[k].used && io[k].state == IO_UP) channel_close(&io[k], false);
	}
	if (readings_dirty) save_readings();
	publish();
	sd_bus_flush_close_unref(bus);
	return r < 0 ? 1 : 0;
}
