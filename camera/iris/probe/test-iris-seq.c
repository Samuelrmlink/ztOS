// SPDX-License-Identifier: GPL-2.0
/*
 * Host test for iris-seq.h: runs the experiment against simulated hardware
 * and checks every access against the list the owner approved.
 *
 *	cc -Wall -Wextra -o /tmp/test-iris-seq test-iris-seq.c && /tmp/test-iris-seq
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iris-seq.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

struct fake {
	/* config */
	int sensor_present, pb02_fail, verbose;
	unsigned char pb02[0x20];
	unsigned int gate_init[4];
	/* state */
	unsigned int mmio_val[64], mmio_addr[64];
	int nmmio;
	int rails[3], reset, pb02_writes, ctrl2_bad_write, sensor_reg_writes;
	unsigned long t;
	/* HSI2C model */
	int run_read, run_len;
	unsigned char tx[8];
	int ntx;
	unsigned char rx[4];
	int nrx, rxpos;
	unsigned int last_reg;
	int done;
	int bad_access;
};

static unsigned int *slot(struct fake *f, unsigned int a)
{
	int i;

	for (i = 0; i < f->nmmio; i++)
		if (f->mmio_addr[i] == a)
			return &f->mmio_val[i];
	f->mmio_addr[f->nmmio] = a;
	f->mmio_val[f->nmmio] = 0;
	return &f->mmio_val[f->nmmio++];
}

/* the approved register list */
static int read_ok(unsigned int a)
{
	static const unsigned int ok[] = {
		GATE_USI_I2C, GATE_USI07_RSTNSYNC, GATE_USI07_IPCLK, GATE_USI07_PCLK, QCH_USI07_I2C,
		GPC1_CON, GPC1_DAT, GPC1_PUD, GPC1_DRV,
		HSI2C_CTL, HSI2C_FIFO_CTL, HSI2C_TRAILIG_CTL, HSI2C_INT_ENABLE, HSI2C_INT_STATUS,
		HSI2C_FIFO_STATUS, HSI2C_RX_DATA, HSI2C_CONF, HSI2C_AUTO_CONF, HSI2C_TIMEOUT,
		HSI2C_TRANS_STATUS, HSI2C_TIMING_FS1, HSI2C_TIMING_FS2, HSI2C_TIMING_FS3, HSI2C_ADDR,
	};
	unsigned int i;

	for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
		if (ok[i] == a)
			return 1;
	return 0;
}

static int write_ok(unsigned int a)
{
	return a == GATE_USI_I2C || a == GATE_USI07_RSTNSYNC || a == GATE_USI07_IPCLK ||
	       a == GATE_USI07_PCLK || a == GPC1_CON || a == GPC1_DAT || a == GPC1_PUD ||
	       a == GPC1_DRV || (a >= HSI2C && a <= HSI2C_ADDR && a != HSI2C_RX_DATA &&
	       a != HSI2C_FIFO_STATUS && a != HSI2C_TRANS_STATUS) || a == HSI2C_USI_CON;
}

static int f_read(void *c, unsigned int a, unsigned int *v)
{
	struct fake *f = c;

	if (!read_ok(a)) {
		f->bad_access++;
		printf("  unapproved read %08x\n", a);
	}
	if (a == HSI2C_INT_STATUS) {
		*v = f->done ? (f->sensor_present ? INT_TRANSFER_DONE : INT_NODEV | INT_TRANSFER_DONE) : 0;
		return 0;
	}
	if (a == HSI2C_FIFO_STATUS) {
		*v = FIFO_TX_EMPTY | (f->rxpos < f->nrx ? 0 : FIFO_RX_EMPTY);
		return 0;
	}
	if (a == HSI2C_RX_DATA) {
		*v = f->rxpos < f->nrx ? f->rx[f->rxpos++] : 0;
		return 0;
	}
	*v = *slot(f, a);
	return 0;
}

static int f_write(void *c, unsigned int a, unsigned int v)
{
	struct fake *f = c;

	if (!write_ok(a)) {
		f->bad_access++;
		printf("  unapproved write %08x <- %08x\n", a, v);
	}
	if (f->verbose)
		printf("    w %08x <- %08x\n", a, v);
	if (a == HSI2C_AUTO_CONF && (v & AUTO_RUN)) {
		f->run_read = !!(v & AUTO_READ);
		f->run_len = v & 0xffff;
		f->ntx = 0;
		f->done = 1;
		if (f->run_read) {
			if (f->sensor_present) {
				f->nrx = 0;
				f->rxpos = 0;
				if (f->last_reg == 0x0000) {
					f->rx[0] = 0x5f;
					f->rx[1] = 0x10;
					f->nrx = 2;
				} else if (f->last_reg == 0x0002) {
					f->rx[0] = 0xa0;
					f->nrx = 1;
				}
			}
		}
	}
	if (a == HSI2C_TX_DATA) {
		f->tx[f->ntx++] = v;
		if (f->ntx == 2)
			f->last_reg = (f->tx[0] << 8) | f->tx[1];
		if (f->ntx > 2 || f->run_len != 2 || (v & ~0xffu))
			f->sensor_reg_writes++;	/* more than a register address */
	}
	if (a == HSI2C_INT_STATUS)
		f->done = 0;
	*slot(f, a) = v;
	return 0;
}

static int f_pb02_read(void *c, unsigned char r, unsigned char *v)
{
	struct fake *f = c;

	if (f->pb02_fail)
		return -121;
	if (!(r == 0x13 || (r >= 0x16 && r <= 0x1b))) {
		f->bad_access++;
		printf("  unapproved PB02 read %02x\n", r);
	}
	*v = f->pb02[r];
	return 0;
}

static int f_pb02_write(void *c, unsigned char r, unsigned char v)
{
	struct fake *f = c;

	f->pb02_writes++;
	if (r != 0x13 || v != 0x00)
		f->ctrl2_bad_write++;
	f->pb02[r] = v;
	return 0;
}

static int f_pmic_set(void *c, unsigned char r, int on)
{
	struct fake *f = c;

	if (r < MPS_L39 || r > MPS_L41) {
		f->bad_access++;
		return -1;
	}
	f->rails[r - MPS_L39] = on;
	return 0;
}

static int f_reset(void *c, int h)
{
	((struct fake *)c)->reset = h;
	return 0;
}

static void f_delay(void *c, unsigned int us)
{
	((struct fake *)c)->t += us;
}

static unsigned long f_now(void *c)
{
	return ((struct fake *)c)->t += 1;
}

static void f_mark(void *c, const char *w)
{
	(void)c;
	printf("    mark: %s\n", w);
}

static void f_log(void *c, const char *fmt, ...)
{
	va_list ap;

	(void)c;
	printf("    ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static struct iris_result run(struct fake *f)
{
	struct iris_hw hw = {
		.ctx = f, .mmio_read = f_read, .mmio_write = f_write, .pb02_read = f_pb02_read,
		.pb02_write = f_pb02_write, .pmic_set = f_pmic_set, .reset_set = f_reset,
		.delay_us = f_delay, .now_us = f_now, .mark = f_mark, .log = f_log,
	};
	struct iris_result r;
	int i;

	memset(&r, 0, sizeof(r));
	for (i = 0; i < 4; i++)
		*slot(f, gate_addr[i]) = f->gate_init[i];
	*slot(f, GPC1_CON) = 0x00000000;	/* boot: gpc1-6 CON 0 */
	*slot(f, GPC1_PUD) = 0x01000000;
	*slot(f, GPC1_DRV) = 0x02000000;
	*slot(f, HSI2C_USI_CON) = 1;
	iris_run(&hw, &r);
	return r;
}

static void common_checks(struct fake *f)
{
	int i;

	CHECK(!f->bad_access, "%d unapproved accesses", f->bad_access);
	CHECK(!f->ctrl2_bad_write, "S2MPB02 written with something other than 0x13 <- 0x00");
	CHECK(!f->sensor_reg_writes, "a sensor register was written");
	for (i = 0; i < 3; i++)
		CHECK(!f->rails[i], "rail %d left on", i);
	CHECK(!f->reset, "reset left high");
	for (i = 0; i < 4; i++)
		CHECK(*slot(f, gate_addr[i]) == f->gate_init[i], "gate %08x not restored (%08x)",
		      gate_addr[i], *slot(f, gate_addr[i]));
}

int main(void)
{
	struct fake *f;
	struct iris_result r;

	printf("A: armed LED, gates closed, sensor answers\n");
	f = calloc(1, sizeof(*f));
	f->sensor_present = 1;
	f->pb02[0x13] = 0x38;
	f->pb02[0x16] = 0xaf;
	f->gate_init[0] = GATE_MANUAL;			/* manual, closed */
	f->gate_init[1] = GATE_MANUAL | GATE_CG_VAL;	/* manual, open */
	f->gate_init[2] = GATE_AUTO_CLKGATING;		/* auto */
	f->gate_init[3] = 0;				/* auto off: open */
	r = run(f);
	common_checks(f);
	CHECK(r.step_failed == 0, "failed at step %d", r.step_failed);
	CHECK(r.id_ok && r.id == 0x5f10 && r.version == 0xa0, "id %04x ver %02x", r.id, r.version);
	CHECK(r.fled_disarmed && f->pb02[0x13] == 0 && f->pb02_writes == 1, "LED not disarmed once");
	CHECK(*slot(f, HSI2C_USI_CON) == 1, "USI_CON not back to 1");
	CHECK(((*slot(f, GPC1_CON) >> 24) & 0xf) == 1, "MCLK pad not output");
	CHECK(!(*slot(f, GPC1_DAT) & (1u << 6)), "MCLK pad not low");
	free(f);

	printf("B: no answer from the sensor\n");
	f = calloc(1, sizeof(*f));
	f->sensor_present = 0;
	f->gate_init[0] = GATE_MANUAL;
	r = run(f);
	common_checks(f);
	CHECK(r.step_failed == 7 && r.err == -6, "step %d err %d", r.step_failed, r.err);
	CHECK(f->pb02_writes == 0, "LED written though not armed");
	free(f);

	printf("C: S2MPB02 does not answer: nothing else happens\n");
	f = calloc(1, sizeof(*f));
	f->pb02_fail = 1;
	r = run(f);
	common_checks(f);
	CHECK(r.step_failed == 1, "step %d", r.step_failed);
	CHECK(f->nmmio == 8, "MMIO touched (%d slots)", f->nmmio);	/* only the preset slots */
	free(f);

	printf("D: gate opening rule (ra.c)\n");
	CHECK(gate_opened(GATE_MANUAL) == (GATE_MANUAL | GATE_CG_VAL), "manual closed");
	CHECK(gate_opened(GATE_AUTO_CLKGATING | GATE_CG_VAL) == GATE_CG_VAL, "auto");
	CHECK(gate_opened(GATE_MANUAL | GATE_CG_VAL) == (GATE_MANUAL | GATE_CG_VAL), "open stays");

	printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
	return failures != 0;
}
