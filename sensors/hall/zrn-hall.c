// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-hall: the flip cover's hall sensor as a lid switch (SW_LID), for the
 * kernel that is already flashed (ROADMAP 8.34).
 *
 * It adds a gpio-keys node to the running device tree (an of_changeset, the
 * dynamic form of an overlay) and creates its device, so mainline's own
 * gpio-keys driver reports the switch: "zrn-hall" in /proc/bus/input, EV_SW
 * SW_LID, 1 with the cover closed. Unloading removes the device and the node.
 * Nothing about the board is in this file: which pin it is, its polarity and
 * the debounce are module parameters, set for the S9 in
 * /etc/modprobe.d/zrn-hall.conf (sensors/hall/zrn-hall.conf), and the same
 * node goes into kernel/make-dts.py for the next boot image, after which this
 * module is no longer needed. (Our device tree has no __symbols__, so a
 * compiled overlay could not name the pin's bank; here the bank's phandle is
 * looked up by node name when the module loads.)
 *
 * The S9 (stock device tree, sensors/COVERS.md): the hall IC on gpa3-6, the
 * line low while a magnet is over it; stock's driver reads it 10 ms after an
 * open, at once on a close, and wakes the phone on either edge.
 *
 * What loading does to the pin: gpio-keys requests it as an input and its
 * interrupt on both edges, which sets the pin's function to the external
 * interrupt (EINT, function 0xf, as stock has it). The pull is left as it is
 * (pinctrl is not touched). That is a GPIO configuration change: do not load
 * it on the phone without the owner's approval.
 */
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <dt-bindings/gpio/gpio.h>

static char *bank;
module_param(bank, charp, 0444);
MODULE_PARM_DESC(bank, "the pin's GPIO bank, by device tree node name (gpa3-gpio-bank on the S9)");
static int pin = -1;
module_param(pin, int, 0444);
MODULE_PARM_DESC(pin, "the pin in that bank (6 on the S9)");
static bool active_low = true;
module_param(active_low, bool, 0444);
MODULE_PARM_DESC(active_low, "the line is low with the cover closed (default yes)");
static int debounce_ms = 15;
module_param(debounce_ms, int, 0444);
MODULE_PARM_DESC(debounce_ms, "gpio-keys' debounce in ms (default 15)");
static bool wakeup = true;
module_param(wakeup, bool, 0444);
MODULE_PARM_DESC(wakeup, "the cover wakes a sleeping phone (default yes)");

static struct of_changeset cs;
static bool cs_applied;
static struct platform_device *pdev;

static int __init zrn_hall_init(void)
{
	struct device_node *bank_np, *np, *key;
	u32 gpios[3];
	int ret;

	if (!bank || pin < 0 || pin > 7) {
		pr_err("zrn-hall: set bank= and pin= (zrn-hall.conf)\n");
		return -EINVAL;
	}
	bank_np = of_find_node_by_name(NULL, bank);
	if (!bank_np || !bank_np->phandle || !of_property_read_bool(bank_np, "gpio-controller")) {
		pr_err("zrn-hall: no GPIO bank %s with a phandle\n", bank);
		of_node_put(bank_np);
		return -ENODEV;
	}
	gpios[0] = bank_np->phandle;
	gpios[1] = pin;
	gpios[2] = active_low ? GPIO_ACTIVE_LOW : GPIO_ACTIVE_HIGH;
	of_node_put(bank_np);

	of_changeset_init(&cs);
	np = of_changeset_create_node(&cs, of_root, "zrn-hall");
	key = np ? of_changeset_create_node(&cs, np, "lid") : NULL;
	if (!np || !key) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = of_changeset_add_prop_string(&cs, np, "compatible", "gpio-keys");
	ret = ret ?: of_changeset_add_prop_string(&cs, np, "label", "zrn-hall");
	ret = ret ?: of_changeset_add_prop_string(&cs, key, "label", "Flip cover");
	ret = ret ?: of_changeset_add_prop_u32(&cs, key, "linux,input-type", EV_SW);
	ret = ret ?: of_changeset_add_prop_u32(&cs, key, "linux,code", SW_LID);
	ret = ret ?: of_changeset_add_prop_u32_array(&cs, key, "gpios", gpios, 3);
	ret = ret ?: of_changeset_add_prop_u32(&cs, key, "debounce-interval", debounce_ms);
	if (!ret && wakeup)
		ret = of_changeset_add_prop_bool(&cs, key, "wakeup-source");
	if (ret)
		goto fail;
	ret = of_changeset_apply(&cs);
	if (ret)
		goto fail;
	cs_applied = true;

	pdev = of_platform_device_create(np, "zrn-hall", NULL);
	if (!pdev) {
		ret = -ENODEV;
		goto fail;
	}
	pr_info("zrn-hall: %s-%d, active %s, as SW_LID\n", bank, pin, active_low ? "low" : "high");
	return 0;
fail:
	pr_err("zrn-hall: %d\n", ret);
	if (cs_applied)
		of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
	return ret;
}

static void __exit zrn_hall_exit(void)
{
	if (pdev)
		of_platform_device_destroy(&pdev->dev, NULL);
	of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
}

module_init(zrn_hall_init);
module_exit(zrn_hall_exit);
MODULE_DESCRIPTION("Flip cover hall sensor as SW_LID, through gpio-keys");
MODULE_LICENSE("GPL");
