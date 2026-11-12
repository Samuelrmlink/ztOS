// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-rails: the owner-approved rail and DVFS fixes of power/RAIL-MAP.md
 * (ROADMAP 8.31, approved 2026-11-12). Two things, nothing else:
 *
 * 1. The memory interface's (MIF) DVFS level (gap 1). Mainline has no MIF
 *    devfreq, so the memory stays at the boot level, 1794 MHz (vdd_mif
 *    0.719 V); stock's devfreq idles it at 421 MHz (0.581 V). This sends the
 *    one request that changes it through the APM's DVFS channel 0, exactly as
 *    snd-zrn-abox does for dvfs_aud and zrn-cam for dvfs_cam: a 4-word command
 *    [domain | seq << 16, kHz, 0 = set, 0] into the channel's SRAM ring, the
 *    channel's doorbell in mailbox INTGR0, the reply taken from the mirror ring
 *    and acknowledged in INTCR1. The APM firmware applies the ASV voltage.
 *    Domain 0 (dvfs_mif) only, 421000 or 1794000 kHz only; INT is not
 *    approved.
 *
 *	echo 421000  > /sys/module/zrn_rails/parameters/mif_khz	screen off
 *	echo 1794000 > /sys/module/zrn_rails/parameters/mif_khz	screen on
 *	echo N > /sys/module/zrn_rails/parameters/mif_enable	kill switch:
 *					puts 1794000 back and refuses lower
 *
 *    zrnperfd writes mif_khz at screen off/on, and holds 1794000 while DP is
 *    connected. Loading sends nothing; unloading puts 1794000 back.
 *
 * 2. The touch controller's analog supply, S2MPS18 LDO43 (tsp_avdd) (gap 5).
 *    The bootloader leaves it at code 0x3c, 3.3 V; stock's DT and kernel have
 *    3.0 V (code 0x30). At load, through the kernel's ACPM PMIC path
 *    (s9p-acpm), register 0x74 gets 0x30 under mask 0x3f: the voltage bits
 *    only, the enable bits untouched. l43_fix=0 at load skips it.
 *
 * Shared channel: snd-zrn-abox (dvfs_aud) and zrn-cam (dvfs_cam) use the same
 * ring with their own locks. Requests from two modules at the same instant
 * could take the same slot; this one uses sequence numbers 60-63 and only
 * sends at screen changes. A lost request shows as a timeout (logged, counted
 * in mif_failures); zrnperfd sends again at the next change.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define APM_MBOX		0x14100000
#define APM_SRAM		0x02039000
#define APM_SRAM_SIZE		0x30000
#define APM_SRAM_MAP		0x3000	/* the first three pages fault */
#define APM_INITDATA		0x7f00
#define APM_INTGR0		0x08
#define APM_INTGR1		0x1c
#define APM_INTCR1		0x20
#define APM_INTSR1		0x28
#define APM_DVFS_CHANNEL	0
#define DVFS_MIF		0	/* acpm_dvfs_exynos9810.h: dvfs_mif */
#define MIF_LOW_KHZ		421000	/* stock's resume level, every ASV row */
#define MIF_BOOT_KHZ		1794000	/* the boot level */

#define S2MPS18_L43CTRL		0x74
#define L43_VSEL_MASK		0x3f
#define L43_3V0			0x30	/* 1.8 V + 48 x 25 mV (s2mps18.c: LDO43 MIN3/STEP2) */

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);
bool s9p_acpm_is_ready(void);

static void __iomem *apm_mbox, *apm_sram;
static DEFINE_MUTEX(lock);
static struct {
	bool ok;
	u32 id, q_len, elem, tx_rear, tx_front, tx_base, rx_rear, rx_front, rx_base;
} ch;
static u32 seq = 60;
static unsigned int cur_khz = MIF_BOOT_KHZ;	/* what the firmware last accepted */
static int mif_last_err;
static unsigned int mif_requests, mif_failures;
static bool mif_enable = true;
static bool l43_fix = true;
module_param(l43_fix, bool, 0444);
MODULE_PARM_DESC(l43_fix, "at load: LDO43 (touch AVDD) to 3.0 V, as stock (0: leave it)");
static unsigned int l43_before, l43_after;
module_param(l43_before, uint, 0444);
module_param(l43_after, uint, 0444);

static u32 rd(u32 off) { return readl(apm_sram + off - APM_SRAM_MAP); }
static void wr(u32 off, u32 v) { writel(v, apm_sram + off - APM_SRAM_MAP); }
static bool off_ok(u32 off) { return off >= APM_SRAM_MAP && off < APM_SRAM_SIZE - 16; }

static void find_channel(void)
{
	u32 table = rd(APM_INITDATA + 2 * 4), ap_max = rd(APM_INITDATA + 6 * 4), i;

	for (i = 0; i < ap_max && i < 16 && off_ok(table + i * 72); i++) {
		u32 b = table + i * 72;

		if (rd(b) != APM_DVFS_CHANNEL)
			continue;
		ch.id = rd(b);
		ch.tx_rear = rd(b + 4 * 4);
		ch.tx_front = rd(b + 5 * 4);
		ch.tx_base = rd(b + 6 * 4);
		ch.rx_rear = rd(b + 10 * 4);
		ch.rx_front = rd(b + 11 * 4);
		ch.rx_base = rd(b + 12 * 4);
		ch.q_len = rd(b + 13 * 4);
		ch.elem = rd(b + 14 * 4);
		ch.ok = ch.q_len > 1 && ch.q_len < 64 && ch.elem >= 16 &&
			off_ok(ch.tx_rear) && off_ok(ch.tx_front) && off_ok(ch.tx_base) &&
			off_ok(ch.rx_rear) && off_ok(ch.rx_front) && off_ok(ch.rx_base);
		break;
	}
}

static int apm_set(u32 domain, u32 khz)
{
	u32 s, front, nxt, i, k, rear;
	unsigned long until;

	if (!ch.ok)
		return -ENODEV;
	s = seq = seq >= 63 ? 60 : seq + 1;
	front = rd(ch.tx_front);
	nxt = (front + 1) % ch.q_len;
	until = jiffies + HZ;
	while (nxt == rd(ch.tx_rear)) {
		if (time_after(jiffies, until))
			return -EBUSY;
		usleep_range(200, 400);
	}
	wr(ch.tx_base + ch.elem * front, domain | (s << 16));
	wr(ch.tx_base + ch.elem * front + 4, khz);
	wr(ch.tx_base + ch.elem * front + 8, 0);
	wr(ch.tx_base + ch.elem * front + 12, 0);
	wr(ch.tx_front, nxt);
	writel((1 << ch.id) << 16, apm_mbox + APM_INTGR0);
	until = jiffies + HZ;
	while (!time_after(jiffies, until)) {
		if (readl(apm_mbox + APM_INTSR1) & (1 << ch.id)) {
			front = rd(ch.rx_front);
			rear = rd(ch.rx_rear);
			for (i = rear; i != front; i = (i + 1) % ch.q_len) {
				u32 w0 = rd(ch.rx_base + ch.elem * i);

				if (((w0 >> 16) & 0x3f) != s)
					continue;
				if (i != rear)
					for (k = 0; k < ch.elem / 4; k++)
						wr(ch.rx_base + ch.elem * i + 4 * k,
						   rd(ch.rx_base + ch.elem * rear + 4 * k));
				rear = (rear + 1) % ch.q_len;
				wr(ch.rx_rear, rear);
				if (rear == rd(ch.rx_front)) {
					writel(1 << ch.id, apm_mbox + APM_INTCR1);
					if (rear != rd(ch.rx_front))
						writel(1 << ch.id, apm_mbox + APM_INTGR1);
				}
				return (w0 & 0xffff) ? -EIO : 0;
			}
		}
		usleep_range(200, 400);
	}
	return -ETIMEDOUT;
}

static int mif_set(unsigned int khz)
{
	ktime_t t0 = ktime_get();
	int ret;

	mutex_lock(&lock);
	ret = apm_set(DVFS_MIF, khz);
	mif_requests++;
	mif_last_err = ret;
	if (ret)
		mif_failures++;
	else
		cur_khz = khz;
	mutex_unlock(&lock);
	pr_info("zrn-rails: MIF %u kHz: %d (%lld us)\n", khz, ret,
		ktime_us_delta(ktime_get(), t0));
	return ret;
}

static int khz_store(const char *val, const struct kernel_param *kp)
{
	unsigned int khz;
	int ret = kstrtouint(val, 0, &khz);

	if (ret)
		return ret;
	if (khz != MIF_LOW_KHZ && khz != MIF_BOOT_KHZ)
		return -EINVAL;		/* only the two approved levels */
	if (!mif_enable && khz != MIF_BOOT_KHZ)
		return -EPERM;
	return mif_set(khz);
}

static int khz_show(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", cur_khz);
}

static const struct kernel_param_ops khz_ops = { .set = khz_store, .get = khz_show };
module_param_cb(mif_khz, &khz_ops, NULL, 0644);
MODULE_PARM_DESC(mif_khz, "MIF level: 421000 or 1794000 (write to request)");

static int enable_store(const char *val, const struct kernel_param *kp)
{
	bool v;
	int ret = kstrtobool(val, &v);

	if (ret)
		return ret;
	mif_enable = v;
	if (!v && cur_khz != MIF_BOOT_KHZ)
		mif_set(MIF_BOOT_KHZ);
	return 0;
}

static int enable_show(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%c\n", mif_enable ? 'Y' : 'N');
}

static const struct kernel_param_ops enable_ops = { .set = enable_store, .get = enable_show };
module_param_cb(mif_enable, &enable_ops, NULL, 0644);
MODULE_PARM_DESC(mif_enable, "N: put 1794000 back and refuse lower levels");
module_param(mif_last_err, int, 0444);
module_param(mif_requests, uint, 0444);
module_param(mif_failures, uint, 0444);

static void touch_avdd(void)
{
	u8 v = 0;
	int ret;

	if (!l43_fix)
		return;
	if (!s9p_acpm_is_ready() || s9p_acpm_pmic_read(S2MPS18_L43CTRL, &v)) {
		pr_warn("zrn-rails: ACPM not ready: LDO43 left as it is\n");
		return;
	}
	l43_before = v;
	if ((v & L43_VSEL_MASK) == L43_3V0) {
		l43_after = v;
		return;
	}
	ret = s9p_acpm_pmic_update(S2MPS18_L43CTRL, L43_3V0, L43_VSEL_MASK);
	s9p_acpm_pmic_read(S2MPS18_L43CTRL, &v);
	l43_after = v;
	pr_info("zrn-rails: LDO43 (touch AVDD) %02x -> %02x, 3.0 V as stock (%d)\n",
		l43_before, l43_after, ret);
}

static int __init zrn_rails_init(void)
{
	apm_mbox = ioremap(APM_MBOX, 0x1000);
	apm_sram = ioremap(APM_SRAM + APM_SRAM_MAP, APM_SRAM_SIZE - APM_SRAM_MAP);
	if (!apm_mbox || !apm_sram) {
		if (apm_mbox)
			iounmap(apm_mbox);
		if (apm_sram)
			iounmap(apm_sram);
		return -ENOMEM;
	}
	find_channel();
	if (!ch.ok)
		pr_err("zrn-rails: APM DVFS channel 0 not found: no MIF requests\n");
	else
		pr_info("zrn-rails: APM DVFS channel %u, %u slots; MIF unchanged until asked\n",
			ch.id, ch.q_len);
	touch_avdd();
	return 0;
}

static void __exit zrn_rails_exit(void)
{
	if (cur_khz != MIF_BOOT_KHZ)
		mif_set(MIF_BOOT_KHZ);
	iounmap(apm_sram);
	iounmap(apm_mbox);
}

module_init(zrn_rails_init);
module_exit(zrn_rails_exit);
MODULE_DESCRIPTION("Exynos 9810 approved rail fixes: MIF DVFS at screen off, touch AVDD at 3.0 V");
MODULE_LICENSE("GPL");
