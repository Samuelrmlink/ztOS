// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd's network counters: three small eBPF programs, written out as
 * instructions (no compiler for BPF, no libbpf on the phone), attached to the
 * root cgroup so every socket on the phone passes them, Waydroid's included.
 *
 *   sock_create	(cgroup/sock, INET_SOCK_CREATE) runs in the process that
 *			makes an inet socket: owners[cookie] = {tgid, netns}
 *   skb_ingress	(cgroup/skb) every packet delivered to a socket
 *   skb_egress		(cgroup/skb) every packet a socket sends:
 *			bytes[{cookie, ifindex, dir}] += {len, 1}
 *
 * Loopback (ifindex 1 in every network namespace) is skipped; packets with
 * no socket (forwarded, NAT for Waydroid's bridge) never reach these hooks,
 * so nothing is counted twice. Lengths are network-layer (IP header up).
 *
 * The programs are held by BPF links on our file descriptors: if ztacctd
 * dies, the kernel detaches them. Maps are LRU hashes, so a burst of sockets
 * cannot make an update fail; userspace deletes entries it has read and that
 * have stopped moving.
 */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct acct_skb_key {
	uint64_t cookie;
	uint32_t ifindex;
	uint32_t dir;		/* 0 received, 1 sent */
};
struct acct_skb_val {
	uint64_t bytes, packets;
};
struct acct_owner {
	uint32_t tgid, uid;
	uint64_t netns;
};

struct acct_bpf {
	int map_bytes, map_owners;
	int prog[3];
	int link[3];
	int cgroup;
	bool up;
	char error[160];
};

static inline long acct_sys_bpf(int cmd, union bpf_attr *attr) {
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

/* instructions */
#define I_ALU64_IMM(OP, D, IMM) ((struct bpf_insn){ .code = BPF_ALU64 | BPF_OP(OP) | BPF_K, .dst_reg = D, .imm = IMM })
#define I_MOV64_REG(D, S) ((struct bpf_insn){ .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = D, .src_reg = S })
#define I_MOV64_IMM(D, IMM) ((struct bpf_insn){ .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = D, .imm = IMM })
#define I_LDX(SZ, D, S, OFF) ((struct bpf_insn){ .code = BPF_LDX | BPF_SIZE(SZ) | BPF_MEM, .dst_reg = D, .src_reg = S, .off = OFF })
#define I_STX(SZ, D, S, OFF) ((struct bpf_insn){ .code = BPF_STX | BPF_SIZE(SZ) | BPF_MEM, .dst_reg = D, .src_reg = S, .off = OFF })
#define I_ST(SZ, D, OFF, IMM) ((struct bpf_insn){ .code = BPF_ST | BPF_SIZE(SZ) | BPF_MEM, .dst_reg = D, .off = OFF, .imm = IMM })
#define I_XADD64(D, S, OFF) ((struct bpf_insn){ .code = BPF_STX | BPF_DW | BPF_ATOMIC, .dst_reg = D, .src_reg = S, .off = OFF, .imm = BPF_ADD })
#define I_JEQ_IMM(D, IMM, OFF) ((struct bpf_insn){ .code = BPF_JMP | BPF_JEQ | BPF_K, .dst_reg = D, .off = OFF, .imm = IMM })
#define I_JNE_IMM(D, IMM, OFF) ((struct bpf_insn){ .code = BPF_JMP | BPF_JNE | BPF_K, .dst_reg = D, .off = OFF, .imm = IMM })
#define I_CALL(F) ((struct bpf_insn){ .code = BPF_JMP | BPF_CALL, .imm = F })
#define I_EXIT() ((struct bpf_insn){ .code = BPF_JMP | BPF_EXIT })
/* two slots: a map's fd, turned into the map by the loader */
#define I_LD_MAP(D, FD) \
	((struct bpf_insn){ .code = BPF_LD | BPF_DW | BPF_IMM, .dst_reg = D, .src_reg = BPF_PSEUDO_MAP_FD, .imm = FD }), \
	((struct bpf_insn){ 0 })

static int acct_bpf_map(int type, int key, int value, int entries) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.map_type = type;
	a.key_size = key;
	a.value_size = value;
	a.max_entries = entries;
	return acct_sys_bpf(BPF_MAP_CREATE, &a);
}

static char acct_bpf_log[16384];

static int acct_bpf_load(int type, int expected, const struct bpf_insn *insns, int n) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.prog_type = type;
	a.expected_attach_type = expected;
	a.insns = (uint64_t)(uintptr_t)insns;
	a.insn_cnt = n;
	a.license = (uint64_t)(uintptr_t)"GPL";
	a.log_buf = (uint64_t)(uintptr_t)acct_bpf_log;
	a.log_size = sizeof(acct_bpf_log);
	a.log_level = 1;
	acct_bpf_log[0] = 0;
	int fd = acct_sys_bpf(BPF_PROG_LOAD, &a);
	if (fd < 0) {
		/* again without the log: some kernels refuse a log that fills up */
		a.log_buf = 0;
		a.log_size = 0;
		a.log_level = 0;
		fd = acct_sys_bpf(BPF_PROG_LOAD, &a);
	}
	return fd;
}

static int acct_bpf_link(int prog, int cgroup, int type) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.link_create.prog_fd = prog;
	a.link_create.target_fd = cgroup;
	a.link_create.attach_type = type;
	return acct_sys_bpf(BPF_LINK_CREATE, &a);
}

static void acct_bpf_stop(struct acct_bpf *b) {
	for (int i = 0; i < 3; i++) {
		if (b->link[i] >= 0) close(b->link[i]);
		if (b->prog[i] >= 0) close(b->prog[i]);
		b->link[i] = b->prog[i] = -1;
	}
	if (b->map_bytes >= 0) close(b->map_bytes);
	if (b->map_owners >= 0) close(b->map_owners);
	if (b->cgroup >= 0) close(b->cgroup);
	b->map_bytes = b->map_owners = b->cgroup = -1;
	b->up = false;
}

static bool acct_bpf_start(struct acct_bpf *b, int entries) {
	memset(b, 0, sizeof(*b));
	b->map_bytes = b->map_owners = b->cgroup = -1;
	for (int i = 0; i < 3; i++) b->prog[i] = b->link[i] = -1;
	b->map_bytes = acct_bpf_map(BPF_MAP_TYPE_LRU_HASH, sizeof(struct acct_skb_key), sizeof(struct acct_skb_val), entries);
	b->map_owners = acct_bpf_map(BPF_MAP_TYPE_LRU_HASH, 8, sizeof(struct acct_owner), entries);
	if (b->map_bytes < 0 || b->map_owners < 0) {
		snprintf(b->error, sizeof(b->error), "map: %s", strerror(errno));
		acct_bpf_stop(b);
		return false;
	}
	const struct bpf_insn sock[] = {
		I_MOV64_REG(BPF_REG_6, BPF_REG_1),
		I_CALL(BPF_FUNC_get_current_pid_tgid),
		I_ALU64_IMM(BPF_RSH, BPF_REG_0, 32),
		I_STX(BPF_W, BPF_REG_10, BPF_REG_0, -24),	/* owner.tgid */
		I_CALL(BPF_FUNC_get_current_uid_gid),
		I_STX(BPF_W, BPF_REG_10, BPF_REG_0, -20),	/* owner.uid (low half) */
		I_MOV64_REG(BPF_REG_1, BPF_REG_6),
		I_CALL(BPF_FUNC_get_netns_cookie),
		I_STX(BPF_DW, BPF_REG_10, BPF_REG_0, -16),	/* owner.netns */
		I_MOV64_REG(BPF_REG_1, BPF_REG_6),
		I_CALL(BPF_FUNC_get_socket_cookie),
		I_STX(BPF_DW, BPF_REG_10, BPF_REG_0, -8),	/* key: the cookie */
		I_LD_MAP(BPF_REG_1, b->map_owners),
		I_MOV64_REG(BPF_REG_2, BPF_REG_10),
		I_ALU64_IMM(BPF_ADD, BPF_REG_2, -8),
		I_MOV64_REG(BPF_REG_3, BPF_REG_10),
		I_ALU64_IMM(BPF_ADD, BPF_REG_3, -24),
		I_MOV64_IMM(BPF_REG_4, BPF_ANY),
		I_CALL(BPF_FUNC_map_update_elem),
		I_MOV64_IMM(BPF_REG_0, 1),
		I_EXIT(),
	};
	#define SKB_PROG(DIR) { \
		I_MOV64_REG(BPF_REG_6, BPF_REG_1), \
		I_CALL(BPF_FUNC_get_socket_cookie), \
		I_JEQ_IMM(BPF_REG_0, 0, 31), \
		I_STX(BPF_DW, BPF_REG_10, BPF_REG_0, -16), \
		I_LDX(BPF_W, BPF_REG_2, BPF_REG_6, offsetof(struct __sk_buff, ifindex)), \
		I_JEQ_IMM(BPF_REG_2, 1, 28), \
		I_STX(BPF_W, BPF_REG_10, BPF_REG_2, -8), \
		I_ST(BPF_W, BPF_REG_10, -4, DIR), \
		I_LD_MAP(BPF_REG_1, b->map_bytes), \
		I_MOV64_REG(BPF_REG_2, BPF_REG_10), \
		I_ALU64_IMM(BPF_ADD, BPF_REG_2, -16), \
		I_CALL(BPF_FUNC_map_lookup_elem), \
		I_JNE_IMM(BPF_REG_0, 0, 16), \
		I_ST(BPF_DW, BPF_REG_10, -32, 0), \
		I_ST(BPF_DW, BPF_REG_10, -24, 0), \
		I_LD_MAP(BPF_REG_1, b->map_bytes), \
		I_MOV64_REG(BPF_REG_2, BPF_REG_10), \
		I_ALU64_IMM(BPF_ADD, BPF_REG_2, -16), \
		I_MOV64_REG(BPF_REG_3, BPF_REG_10), \
		I_ALU64_IMM(BPF_ADD, BPF_REG_3, -32), \
		I_MOV64_IMM(BPF_REG_4, BPF_NOEXIST), \
		I_CALL(BPF_FUNC_map_update_elem), \
		I_LD_MAP(BPF_REG_1, b->map_bytes), \
		I_MOV64_REG(BPF_REG_2, BPF_REG_10), \
		I_ALU64_IMM(BPF_ADD, BPF_REG_2, -16), \
		I_CALL(BPF_FUNC_map_lookup_elem), \
		I_JEQ_IMM(BPF_REG_0, 0, 4), \
		/* add: */ \
		I_LDX(BPF_W, BPF_REG_1, BPF_REG_6, offsetof(struct __sk_buff, len)), \
		I_XADD64(BPF_REG_0, BPF_REG_1, 0), \
		I_MOV64_IMM(BPF_REG_1, 1), \
		I_XADD64(BPF_REG_0, BPF_REG_1, 8), \
		/* out: */ \
		I_MOV64_IMM(BPF_REG_0, 1), \
		I_EXIT(), \
	}
	const struct bpf_insn in[] = SKB_PROG(0);
	const struct bpf_insn out[] = SKB_PROG(1);
	#undef SKB_PROG
	b->prog[0] = acct_bpf_load(BPF_PROG_TYPE_CGROUP_SOCK, BPF_CGROUP_INET_SOCK_CREATE, sock, sizeof(sock) / sizeof(sock[0]));
	b->prog[1] = acct_bpf_load(BPF_PROG_TYPE_CGROUP_SKB, BPF_CGROUP_INET_INGRESS, in, sizeof(in) / sizeof(in[0]));
	b->prog[2] = acct_bpf_load(BPF_PROG_TYPE_CGROUP_SKB, BPF_CGROUP_INET_EGRESS, out, sizeof(out) / sizeof(out[0]));
	for (int i = 0; i < 3; i++) {
		if (b->prog[i] < 0) {
			snprintf(b->error, sizeof(b->error), "program %d: %s: %.100s", i, strerror(errno), acct_bpf_log);
			acct_bpf_stop(b);
			return false;
		}
	}
	b->cgroup = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	static const int types[3] = { BPF_CGROUP_INET_SOCK_CREATE, BPF_CGROUP_INET_INGRESS, BPF_CGROUP_INET_EGRESS };
	for (int i = 0; i < 3; i++) {
		b->link[i] = b->cgroup < 0 ? -1 : acct_bpf_link(b->prog[i], b->cgroup, types[i]);
		if (b->link[i] < 0) {
			snprintf(b->error, sizeof(b->error), "attach %d: %s", i, strerror(errno));
			acct_bpf_stop(b);
			return false;
		}
	}
	b->up = true;
	return true;
}

static inline int acct_bpf_next(int map, const void *key, void *next) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.map_fd = map;
	a.key = (uint64_t)(uintptr_t)key;
	a.next_key = (uint64_t)(uintptr_t)next;
	return acct_sys_bpf(BPF_MAP_GET_NEXT_KEY, &a);
}

static inline int acct_bpf_get(int map, const void *key, void *value) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.map_fd = map;
	a.key = (uint64_t)(uintptr_t)key;
	a.value = (uint64_t)(uintptr_t)value;
	return acct_sys_bpf(BPF_MAP_LOOKUP_ELEM, &a);
}

static inline int acct_bpf_del(int map, const void *key) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.map_fd = map;
	a.key = (uint64_t)(uintptr_t)key;
	return acct_sys_bpf(BPF_MAP_DELETE_ELEM, &a);
}

static inline int acct_bpf_put(int map, const void *key, const void *value) {
	union bpf_attr a;
	memset(&a, 0, sizeof(a));
	a.map_fd = map;
	a.key = (uint64_t)(uintptr_t)key;
	a.value = (uint64_t)(uintptr_t)value;
	a.flags = BPF_ANY;
	return acct_sys_bpf(BPF_MAP_UPDATE_ELEM, &a);
}
