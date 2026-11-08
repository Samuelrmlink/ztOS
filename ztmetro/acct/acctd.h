// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd's tables, shared by its parts: ztacctd.c (processes, apps, the
 * windows, the state file and the socket), acct-net.c (bytes per socket) and
 * acct-energy.c (the power model and the battery gauge).
 */
#pragma once
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

/* network classes */
enum { NET_WIFI, NET_MOBILE, NET_OTHER, NCLS };
/* where an app's energy went */
enum { E_CPU, E_NET, E_SCREEN, E_GPU, E_BASE, NEK };

extern const char *const net_names[NCLS];
extern const char *const e_names[NEK];

/* Tallies only grow; a window is a tally that starts at zero. */
struct tally {
	double cpu_s;		/* on a CPU (user + system) */
	double fg_s;		/* in front, with the screen on */
	double bg_cpu_s;	/* on a CPU while not in front (or the screen off) */
	double rx[NCLS], tx[NCLS];	/* bytes */
	double e[NEK];		/* joules */
};

enum group_kind {
	K_APP = 'a',		/* an app the shell launched (desktop file) */
	K_ANDROID = 'A',	/* an Android app in Waydroid, by package */
	K_SERVICE = 's',	/* a system service (systemd unit) */
	K_USER = 'u',		/* the user's own services */
	K_SHELL = 'S',		/* ztMETRO and its helpers */
	K_KERNEL = 'k',
	K_LOGIN = 'l',		/* ssh and other login sessions */
	K_MODEL = 'm',		/* what no process does: screen idle, Wi-Fi idle, deep sleep, ... */
	K_OTHER = 'o',
	K_GAUGE = 'g',		/* not shown: the battery gauge's measured drain */
};

#define HOURS		24
#define DAYS		8
#define EXITED_KEEP	10	/* exited processes kept per app, by name */

struct exited {
	char comm[24];
	uint32_t n;
	struct tally t;
};

struct agroup {
	char name[64];		/* what people see: "Brave", "System: NetworkManager" */
	char id[64];		/* what it was matched by: desktop id, unit, package */
	char kind;
	struct tally chg;	/* since the last full charge */
	struct tally hour[HOURS];
	int32_t hour_at[HOURS];	/* which hour (since the epoch) each slot holds */
	struct tally day[DAYS];
	int32_t day_at[DAYS];	/* which local day (since the epoch) */
	struct tally exited_t;	/* its processes that have exited (since the charge) */
	uint32_t n_exited;
	struct exited ex[EXITED_KEEP];
	double pend[NEK];	/* modelled joules not yet checked against the gauge */
	time_t last_active;
	int live;		/* processes now (counted each scan) */
};

struct proc {
	pid_t pid, ppid, root;	/* root: the app's first process (or 0) */
	unsigned long long start;	/* clock ticks after boot: a pid used again differs */
	int g;			/* its group */
	char comm[24];
	unsigned long long ticks;	/* utime + stime when last read */
	double exit_since;	/* seconds of threads that exited since then (taskstats) */
	int cpu;		/* the processor it last ran on */
	int threads;
	bool kthread, gone;
	uint32_t gen;
	struct tally t;		/* since it started (or since we first saw it) */
	double d_cpu;		/* this interval's CPU seconds, for the energy model */
	double e_raw;		/* modelled joules, before checking against the gauge */
};

#define MAX_GROUPS	320
#define MAX_PROCS	4096

extern struct agroup groups[MAX_GROUPS];
extern int n_groups;
extern struct proc procs[MAX_PROCS];
extern int n_procs;

/* the state the energy model and the network need */
struct acct_now {
	double boot_s, mono_s;	/* CLOCK_BOOTTIME and CLOCK_MONOTONIC, seconds */
	time_t wall;
	bool screen_on;
	pid_t front_pid;	/* the app in front (0: Start, or unknown) */
	int front_g;		/* its group, or the shell's */
	bool front_known;	/* zrnperfd says what is in front */
	bool charging;
};
extern struct acct_now now;

int group_get(const char *name, const char *id, char kind);
void group_add(int g, const struct tally *d);	/* into every window, now */
struct proc *proc_find(pid_t pid);
int group_of_pid(pid_t pid);			/* live, or recently exited; -1 */
int group_unattributed_net(void);
double tick_ticks(void);			/* clock ticks per second */

/* acct-net.c */
bool net_start(void);
void net_tick(double dt);
void net_status(FILE *f);			/* JSON fields (no braces) */
bool net_up(void);
int net_default_class(void);

/* acct-energy.c */
void energy_start(void);
void energy_tick(double dt_boot, double dt_mono);	/* after the scan: procs' d_cpu are set */
void energy_screen_change(void);		/* before the screen or the front app changes */
void energy_settle(bool force);
void energy_status(FILE *f);
void energy_add_net(int g, struct proc *p, int cls, double bytes, double packets);
void energy_save_calib(void);

void tally_add(struct tally *to, const struct tally *d);
