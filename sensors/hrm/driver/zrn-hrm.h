/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* zrn-hrm: the record read() returns from /dev/zrn_hrm (24 bytes, little-endian) */
#ifndef ZRN_HRM_H
#define ZRN_HRM_H

#include <linux/types.h>

struct zrn_hrm_sample {
	__u64 t_ns;	/* CLOCK_MONOTONIC, estimated: 10 ms apart within a batch */
	__u32 seq;	/* jumps over FIFO overflows */
	__u32 ir;	/* 19-bit ADC counts, slot 1 (IR LED) */
	__u32 red;	/* 19-bit ADC counts, slot 2 (red LED) */
	__u32 pad;
};

#endif
