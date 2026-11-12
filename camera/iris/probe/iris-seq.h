/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The iris experiment (camera/iris/README.md, "Smallest safe experiment"),
 * steps 1 to 8, written once against a small table of hardware operations so
 * the same code runs in the kernel module (zrn-iris-probe.c) and in the host
 * test (test-iris-seq.c), which checks every access against the approved list.
 *
 * Rules carried in the code, not only in the text:
 *   - S2MPB02 0x13 (FLED_CTRL2, the IR LED) is read, and written only to 0x00.
 *   - gpg0-7 (gpio_iris_en) has no operation here at all.
 *   - No SMC: there is no operation for one.
 *   - No sensor register is written: the only I2C to 0x10 is a register
 *     address (write phase of a combined read) followed by a read.
 *   - The first failure stops the sequence and goes straight to step 8.
 */
#ifndef IRIS_SEQ_H
#define IRIS_SEQ_H

/* S2MPB02 (I2C 0x59, bit-banged gpp6, zrn-flash's bus): s2mpb02-private.h */
#define PB02_FLED_CTRL2	0x13
#define PB02_FLED_CUR2	0x16
#define PB02_FLED_TIME2	0x17
#define PB02_FLED_IRON1	0x18
#define PB02_FLED_IRON2	0x19
#define PB02_FLED_IRD1	0x1a
#define PB02_FLED_IRD2	0x1b

/* S2MPS18 through ACPM: LDO41 VDDIO 1.8, LDO40 VDDA 2.8, LDO39 VDDD 1.05 */
#define MPS_L39		0x70
#define MPS_L40		0x71
#define MPS_L41		0x72
#define MPS_ON		0xc0	/* the enable field zrn-cam uses; voltage untouched */

/* CMU_PERIC1, cmucal-sfr.c (SFR_BLOCK PERIC1 0x10800000) */
#define CMU_PERIC1		0x10800000u
#define GATE_USI_I2C		(CMU_PERIC1 + 0x2050)	/* GATE_CLK_PERIC1_USI_I2C */
#define GATE_USI07_RSTNSYNC	(CMU_PERIC1 + 0x2090)
#define GATE_USI07_IPCLK	(CMU_PERIC1 + 0x20dc)
#define GATE_USI07_PCLK		(CMU_PERIC1 + 0x20e0)
#define QCH_USI07_I2C		(CMU_PERIC1 + 0x3034)	/* read only */
#define GATE_MANUAL		(1u << 20)
#define GATE_CG_VAL		(1u << 21)
#define GATE_AUTO_CLKGATING	(1u << 28)

/* PERIC1 pinctrl, gpc1 bank: MCLK2 on gpc1-6 */
#define PERIC1_GPIO		0x10830000u
#define GPC1			(PERIC1_GPIO + 0x80)
#define GPC1_CON		(GPC1 + 0x0)
#define GPC1_DAT		(GPC1 + 0x4)
#define GPC1_PUD		(GPC1 + 0x8)
#define GPC1_DRV		(GPC1 + 0xc)
#define MCLK_PIN		6

/* USI07_I2C (hsi2c@108D0000), i2c-exynos5.c */
#define HSI2C			0x108d0000u
#define HSI2C_CTL		(HSI2C + 0x00)
#define HSI2C_FIFO_CTL		(HSI2C + 0x04)
#define HSI2C_TRAILIG_CTL	(HSI2C + 0x08)
#define HSI2C_INT_ENABLE	(HSI2C + 0x20)
#define HSI2C_INT_STATUS	(HSI2C + 0x24)
#define HSI2C_FIFO_STATUS	(HSI2C + 0x30)
#define HSI2C_TX_DATA		(HSI2C + 0x34)
#define HSI2C_RX_DATA		(HSI2C + 0x38)
#define HSI2C_CONF		(HSI2C + 0x40)
#define HSI2C_AUTO_CONF		(HSI2C + 0x44)
#define HSI2C_TIMEOUT		(HSI2C + 0x48)
#define HSI2C_TRANS_STATUS	(HSI2C + 0x50)
#define HSI2C_TIMING_FS1	(HSI2C + 0x60)
#define HSI2C_TIMING_FS2	(HSI2C + 0x64)
#define HSI2C_TIMING_FS3	(HSI2C + 0x68)
#define HSI2C_ADDR		(HSI2C + 0x70)
#define HSI2C_USI_CON		(HSI2C + 0xc4)

#define CTL_MASTER		(1u << 3)
#define CTL_RXCHON		(1u << 6)
#define CTL_TXCHON		(1u << 7)
#define CTL_SW_RST		(1u << 31)
#define FIFO_RX_EN		(1u << 0)
#define FIFO_TX_EN		(1u << 1)
#define INT_TRANSFER_DONE	(1u << 7)
#define INT_NODEV		(1u << 10)
#define INT_ERRORS		(0xfu << 8)
#define FIFO_RX_EMPTY		(1u << 24)
#define FIFO_TX_EMPTY		(1u << 8)
#define CONF_AUTO_MODE		(1u << 31)
#define AUTO_READ		(1u << 16)
#define AUTO_STOP		(1u << 17)
#define AUTO_RUN		(1u << 31)
#define TIMEOUT_EN		(1u << 31)

#define SENSOR_ADDR		0x10
/*
 * The IP clock rate is not read (no CMU mux/divider read was approved).
 * Timing assumes 200 MHz, stock's default-clk, for 100 kHz; a slower real
 * clock only makes SCL slower (26 MHz gives about 13 kHz).
 */
#define IPCLK_ASSUMED_MHZ	200
#define SCL_TARGET_HZ		100000

struct iris_hw {
	void *ctx;
	int (*mmio_read)(void *ctx, unsigned int addr, unsigned int *val);
	int (*mmio_write)(void *ctx, unsigned int addr, unsigned int val);
	int (*pb02_read)(void *ctx, unsigned char reg, unsigned char *val);
	int (*pb02_write)(void *ctx, unsigned char reg, unsigned char val);
	int (*pmic_set)(void *ctx, unsigned char reg, int on);	/* enable field only */
	int (*reset_set)(void *ctx, int high);			/* gpg2-3 */
	void (*delay_us)(void *ctx, unsigned int us);
	unsigned long (*now_us)(void *ctx);
	void (*mark)(void *ctx, const char *what);		/* to the console before a risky step */
	void (*log)(void *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

struct iris_result {
	int step_failed;		/* 0 when all of 1..7 ran */
	int err;
	unsigned char fled[7];		/* 0x13, 0x16..0x1b as read at step 1 */
	int fled_disarmed;
	unsigned int gate[4], qch;	/* as read at step 5 */
	unsigned int ctl;		/* the step-6 read */
	int id_ok;
	unsigned int id;		/* sensor 0x0000, two bytes */
	unsigned int version;		/* sensor 0x0002, one byte */
};

/* gate rule from ra.c ra_set_gate(pass=1) */
static inline unsigned int gate_opened(unsigned int v)
{
	if (v & GATE_MANUAL)
		return v | GATE_CG_VAL;
	return v & ~GATE_AUTO_CLKGATING;
}

#define HW(fn, ...) (s->hw->fn(s->hw->ctx, ##__VA_ARGS__))

struct iris_state {
	const struct iris_hw *hw;
	struct iris_result *r;
	int rails[3];			/* L41, L40, L39 switched on by us */
	int reset_driven, mclk_on, gates_touched[4], hsi2c_touched;
	unsigned int gate_saved[4];
};

static int rd(struct iris_state *s, unsigned int a, unsigned int *v)
{
	return HW(mmio_read, a, v);
}

static int wr(struct iris_state *s, unsigned int a, unsigned int v)
{
	return HW(mmio_write, a, v);
}

static int rmw_field4(struct iris_state *s, unsigned int a, unsigned int pin, unsigned int val)
{
	unsigned int v;
	int ret = rd(s, a, &v);

	if (ret)
		return ret;
	v &= ~(0xfu << (pin * 4));
	return wr(s, a, v | (val << (pin * 4)));
}

static int hsi2c_reset(struct iris_state *s)
{
	unsigned int ctl;
	int ret = rd(s, HSI2C_CTL, &ctl);

	if (!ret)
		ret = wr(s, HSI2C_CTL, ctl | CTL_SW_RST);
	if (!ret)
		ret = wr(s, HSI2C_CTL, ctl & ~CTL_SW_RST);
	return ret;
}

static int hsi2c_setup(struct iris_state *s)
{
	unsigned int v, div, h, sh;
	int ret;

	/* USI_RESET is active high and resets to 1; clear it before use */
	ret = wr(s, HSI2C_USI_CON, 0);
	if (!ret)
		ret = hsi2c_reset(s);
	if (ret)
		return ret;
	/* i2c-exynos5.c fast-mode formula, op_clk = SCL_TARGET_HZ */
	div = (IPCLK_ASSUMED_MHZ * 1000000u / (SCL_TARGET_HZ * 15)) & 0xff;
	h = (9 * IPCLK_ASSUMED_MHZ) / ((div + 1) * 10);
	sh = h ? h - 1 : 0;
	if (h > 7)
		h = 7;
	if (sh > 7)
		sh = 7;
	ret = rd(s, HSI2C_TIMING_FS3, &v);
	if (!ret)
		ret = wr(s, HSI2C_TIMING_FS3, (v & ~0x00ff0000u) | (div << 16));
	if (!ret)
		ret = rd(s, HSI2C_TIMING_FS2, &v);
	if (!ret)
		ret = wr(s, HSI2C_TIMING_FS2, (v & ~0xffu) | (((0xffffffffu >> h) << h) & 0xff));
	if (!ret)
		ret = rd(s, HSI2C_TIMING_FS1, &v);
	if (!ret)
		ret = wr(s, HSI2C_TIMING_FS1, (v & ~0x00ff0000u) | ((((0xffffffffu >> sh) << sh) & 0xff) << 16));
	if (!ret)
		ret = wr(s, HSI2C_CTL, CTL_MASTER);
	if (!ret)
		ret = wr(s, HSI2C_TRAILIG_CTL, 0xffffff);
	if (!ret)
		ret = rd(s, HSI2C_CONF, &v);
	if (!ret)
		ret = wr(s, HSI2C_CONF, v | CONF_AUTO_MODE);
	return ret;
}

/* one auto-mode message, polled; len <= 2 so it never waits on FIFO space */
static int hsi2c_msg(struct iris_state *s, int read, unsigned char *buf, unsigned int len, int stop)
{
	unsigned int ctl, autoc, v, st, i = 0;
	unsigned long t0;
	int ret;

	ret = rd(s, HSI2C_CTL, &ctl);
	if (!ret)
		ret = rd(s, HSI2C_AUTO_CONF, &autoc);
	if (!ret)
		ret = rd(s, HSI2C_TIMEOUT, &v);
	if (!ret)
		ret = wr(s, HSI2C_TIMEOUT, v & ~TIMEOUT_EN);
	if (!ret)
		ret = wr(s, HSI2C_FIFO_CTL, FIFO_RX_EN | FIFO_TX_EN | (len << 4) | (len << 16));
	if (ret)
		return ret;
	if (read) {
		ctl = (ctl & ~CTL_TXCHON) | CTL_RXCHON;
		autoc |= AUTO_READ;
	} else {
		ctl = (ctl & ~CTL_RXCHON) | CTL_TXCHON;
		autoc &= ~AUTO_READ;
	}
	autoc = stop ? autoc | AUTO_STOP : autoc & ~AUTO_STOP;
	ret = rd(s, HSI2C_INT_STATUS, &v);
	if (!ret)
		ret = wr(s, HSI2C_INT_STATUS, v);
	if (!ret)
		ret = wr(s, HSI2C_ADDR, (SENSOR_ADDR & 0x7f) << 10);
	if (!ret)
		ret = wr(s, HSI2C_CTL, ctl);
	if (!ret)
		ret = wr(s, HSI2C_INT_ENABLE, INT_TRANSFER_DONE);
	autoc = (autoc & ~0xffffu) | len;
	if (!ret)
		ret = wr(s, HSI2C_AUTO_CONF, autoc);
	if (!ret)
		ret = wr(s, HSI2C_AUTO_CONF, autoc | AUTO_RUN);
	if (!read)
		for (; !ret && i < len; i++)
			ret = wr(s, HSI2C_TX_DATA, buf[i]);
	if (ret)
		return ret;
	t0 = HW(now_us);
	for (i = 0;;) {
		ret = rd(s, HSI2C_INT_STATUS, &st);
		if (!ret)
			ret = rd(s, HSI2C_FIFO_STATUS, &v);
		if (ret)
			return ret;
		if (st & INT_NODEV) {
			wr(s, HSI2C_INT_STATUS, st);
			return -6;	/* ENXIO: no ACK */
		}
		if (st & INT_ERRORS) {
			wr(s, HSI2C_INT_STATUS, st);
			return -5;	/* EIO */
		}
		if (read) {
			while (!(v & FIFO_RX_EMPTY) && i < len) {
				unsigned int b;

				ret = rd(s, HSI2C_RX_DATA, &b);
				if (ret)
					return ret;
				buf[i++] = b & 0xff;
				ret = rd(s, HSI2C_FIFO_STATUS, &v);
				if (ret)
					return ret;
			}
			if (i >= len)
				break;
		} else if ((st & INT_TRANSFER_DONE) && (v & FIFO_TX_EMPTY)) {
			wr(s, HSI2C_INT_STATUS, st);
			break;
		}
		if (HW(now_us) - t0 > 50000)
			return -110;	/* ETIMEDOUT */
		HW(delay_us, 10);
	}
	return 0;
}

static int sensor_read(struct iris_state *s, unsigned int reg, unsigned char *out, unsigned int len)
{
	unsigned char a[2] = { reg >> 8, reg & 0xff };
	int ret = hsi2c_msg(s, 0, a, 2, 0);

	if (!ret)
		ret = hsi2c_msg(s, 1, out, len, 1);
	if (ret)
		hsi2c_reset(s);
	return ret;
}

static const unsigned int gate_addr[4] = {
	GATE_USI_I2C, GATE_USI07_RSTNSYNC, GATE_USI07_IPCLK, GATE_USI07_PCLK,
};

/* step 8: undo exactly what was done, in reverse, ignoring errors */
static void iris_power_down(struct iris_state *s)
{
	static const unsigned char rail_reg[3] = { MPS_L41, MPS_L40, MPS_L39 };
	int i;

	HW(log, "step 8: power down");
	if (s->hsi2c_touched) {
		hsi2c_reset(s);
		wr(s, HSI2C_USI_CON, 1);	/* back to its reset value */
	}
	for (i = 3; i >= 0; i--)
		if (s->gates_touched[i])
			wr(s, gate_addr[i], s->gate_saved[i]);
	if (s->mclk_on) {
		unsigned int d;

		if (!rd(s, GPC1_DAT, &d))
			wr(s, GPC1_DAT, d & ~(1u << MCLK_PIN));
		rmw_field4(s, GPC1_CON, MCLK_PIN, 1);	/* output, low */
		rmw_field4(s, GPC1_PUD, MCLK_PIN, 1);
		HW(delay_us, 200);
	}
	if (s->reset_driven)
		HW(reset_set, 0);
	/* stock off order: VDDD, VDDA, VDDIO */
	for (i = 2; i >= 0; i--)
		if (s->rails[i])
			HW(pmic_set, rail_reg[i], 0);
}

static int iris_run(const struct iris_hw *hw, struct iris_result *r)
{
	static const unsigned char fled_reg[7] = {
		PB02_FLED_CTRL2, PB02_FLED_CUR2, PB02_FLED_TIME2, PB02_FLED_IRON1,
		PB02_FLED_IRON2, PB02_FLED_IRD1, PB02_FLED_IRD2,
	};
	static const unsigned char rail_reg[3] = { MPS_L41, MPS_L40, MPS_L39 };
	struct iris_state st = { .hw = hw, .r = r }, *s = &st;
	unsigned char b[2];
	int i, step = 0, ret = 0;

	/* 1: the IR LED channel, read */
	step = 1;
	for (i = 0; i < 7 && !ret; i++)
		ret = HW(pb02_read, fled_reg[i], &r->fled[i]);
	if (ret)
		goto fail;
	HW(log, "step 1: S2MPB02 CTRL2 %02x CUR2 %02x TIME2 %02x IRON %02x %02x IRD %02x %02x",
	   r->fled[0], r->fled[1], r->fled[2], r->fled[3], r->fled[4], r->fled[5], r->fled[6]);
	/* 1a: disarm only; 0x13 is never written with anything but 0 */
	if (r->fled[0] & 0xf0) {
		step = 11;
		ret = HW(pb02_write, PB02_FLED_CTRL2, 0x00);
		if (!ret)
			ret = HW(pb02_read, PB02_FLED_CTRL2, b);
		if (!ret && b[0] != 0x00)
			ret = -5;
		if (ret)
			goto fail;
		r->fled_disarmed = 1;
		HW(log, "step 1a: FLED_CTRL2 was %02x, now 00", r->fled[0]);
	}
	/* 2: reset low */
	step = 2;
	ret = HW(reset_set, 0);
	if (ret)
		goto fail;
	s->reset_driven = 1;
	/* 3: rails in stock's order, enable field only */
	step = 3;
	for (i = 0; i < 3; i++) {
		ret = HW(pmic_set, rail_reg[i], 1);
		if (ret)
			goto fail;
		s->rails[i] = 1;
	}
	HW(delay_us, 1000);
	/* 4: reset high, MCLK2 function (fimc_is_mclk2_fn: pud 0, drv 1) */
	step = 4;
	ret = HW(reset_set, 1);
	if (!ret)
		ret = rmw_field4(s, GPC1_PUD, MCLK_PIN, 0);
	if (!ret)
		ret = rmw_field4(s, GPC1_DRV, MCLK_PIN, 1);
	if (!ret) {
		s->mclk_on = 1;
		ret = rmw_field4(s, GPC1_CON, MCLK_PIN, 2);
	}
	if (ret)
		goto fail;
	HW(delay_us, 2000);
	/* 5: clock gates, read; 5a: open the closed ones */
	step = 5;
	for (i = 0; i < 4 && !ret; i++)
		ret = rd(s, gate_addr[i], &r->gate[i]);
	if (!ret)
		ret = rd(s, QCH_USI07_I2C, &r->qch);
	if (ret)
		goto fail;
	HW(log, "step 5: gates %08x %08x %08x %08x qch %08x",
	   r->gate[0], r->gate[1], r->gate[2], r->gate[3], r->qch);
	step = 51;
	for (i = 0; i < 4; i++) {
		unsigned int n = gate_opened(r->gate[i]);

		if (n == r->gate[i])
			continue;
		s->gate_saved[i] = r->gate[i];
		s->gates_touched[i] = 1;
		ret = wr(s, gate_addr[i], n);
		if (ret)
			goto fail;
		HW(log, "step 5a: %08x: %08x -> %08x", gate_addr[i], r->gate[i], n);
	}
	/* 6: the one risky read */
	step = 6;
	HW(mark, "step 6: reading USI07_I2C CTL at 0x108d0000");
	ret = rd(s, HSI2C_CTL, &r->ctl);
	if (ret)
		goto fail;
	HW(log, "step 6: CTL = %08x (survived)", r->ctl);
	/* 7: controller up, then two reads from the sensor */
	step = 7;
	s->hsi2c_touched = 1;
	ret = hsi2c_setup(s);
	if (!ret)
		ret = sensor_read(s, 0x0000, b, 2);
	if (ret) {
		HW(log, "step 7: no answer from 0x10 at 0x0000 (%d)", ret);
		goto fail;
	}
	r->id = (b[0] << 8) | b[1];
	ret = sensor_read(s, 0x0002, b, 1);
	if (ret) {
		HW(log, "step 7: ID %04x, then no answer at 0x0002 (%d)", r->id, ret);
		goto fail;
	}
	r->version = b[0];
	r->id_ok = 1;
	HW(log, "step 7: sensor 0x10 answers: 0x0000 = %04x, 0x0002 = %02x", r->id, r->version);
	iris_power_down(s);
	r->step_failed = 0;
	return 0;
fail:
	HW(log, "step %d failed (%d): stopping", step, ret);
	r->step_failed = step;
	r->err = ret;
	iris_power_down(s);
	return ret;
}

#endif
