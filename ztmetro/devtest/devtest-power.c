// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnDevTest's temperatures and charging pages (ROADMAP 8.32): what stock's
 * BatteryStatusEng (*#0228#) shows, as far as this kernel can supply it, and
 * what it cannot, said so. Read-only: plain sysfs reads, redrawn once a
 * second only while the page is open (devtest-pages.c).
 *
 * The thermistors and their decoding tables: power/thermistors/README.md and
 * starlte-rev26.json. Stock reads them all on the SoC's ADC (adc@14230000);
 * this kernel has no driver for it, so none of them is measured here.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ztos-hw.h"
#include "zrnmetrocore.h"
#include "devtest-power.h"
#include "../power/battery-health.h"

#define PSY "/sys/class/power_supply/"

/* a whole small file on one line ("a; b; c"), or "" */
static void read_all(const char *path, char *out, size_t size) {
	out[0] = '\0';
	FILE *f = fopen(path, "re");
	if (!f) return;
	size_t n = fread(out, 1, size - 1, f);
	fclose(f);
	out[n] = '\0';
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = '\0';
	for (char *p = out; (p = strchr(p, '\n')); ) {
		*p = ';';
	}
}

static void psy(const char *supply, const char *leaf, char *out, size_t size) {
	char path[160];
	snprintf(path, sizeof(path), PSY "%s/%s", supply, leaf);
	zmc_read_file(path, out, size);
}

static long psy_long(const char *supply, const char *leaf, long fallback) {
	char text[48];
	psy(supply, leaf, text, sizeof(text));
	return text[0] ? atol(text) : fallback;
}

/* /sys/class/hwmon/hwmonN whose name is `name`, or "" */
static void hwmon_dir(const char *name, char *out, size_t size) {
	out[0] = '\0';
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[300], text[64];
		if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
		zmc_read_file(path, text, sizeof(text));
		if (!strcmp(text, name)) {
			snprintf(out, size, "/sys/class/hwmon/%s", e->d_name);
			break;
		}
	}
	if (d) closedir(d);
}

static long hwmon_long(const char *dir, const char *leaf) {
	char path[300], text[32];
	if (!dir[0]) return -1;
	snprintf(path, sizeof(path), "%s/%s", dir, leaf);
	zmc_read_file(path, text, sizeof(text));
	return text[0] ? atol(text) : -1;
}

static const char *or_unavailable(const char *s) { return s[0] ? s : "unavailable"; }

/* Stock's thermistors (stock DT rev26, BatteryStatusEng), in its order. */
static const struct {
	const char *name, *where;
} thermistors[] = {
	{ "Battery", "SoC ADC channel 4; stock's charging limits and swelling steps" },
	{ "USB connector", "SoC ADC channel 1; stock cuts VBUS when it overheats" },
	{ "Charger", "SoC ADC channel 3; stock's charger-temperature current limit" },
	{ "Wireless coil", "SoC ADC channel 4 (the battery's: no coil thermistor of its own); stock's pad current limit" },
	{ "AP (sec-ap-thermistor)", "SoC ADC channel 0; stock's thermal throttling" },
	{ "Wi-Fi (sec-wf-thermistor)", "SoC ADC channel 2" },
	{ "PA (sec-pa-thermistor)", "SoC ADC channel 10; the modem's power amplifier" },
};

double devtest_draw_temps(cairo_t *cr) {
	char value[256], a[64];
	double y = zmc_header(cr, "temperatures");

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "Stock reads every thermistor on the SoC's ADC. This kernel has no "
		"driver for it, so none of them is measured here: unavailable, not zero.",
		FONT_REGULAR, 13, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 16;

	for (size_t i = 0; i < sizeof(thermistors) / sizeof(thermistors[0]); i++) {
		snprintf(value, sizeof(value), "unavailable (%s)", thermistors[i].where);
		y = zmc_field(cr, y, thermistors[i].name, value);
	}

	/* what can be read today, each for what it is */
	snprintf(value, sizeof(value), "%.1f C, not a measurement: the last value Android wrote "
		"into the gauge; nothing on ztOS updates it", psy_long("max170xx_battery", "temp", 0) / 10.0);
	y = zmc_field(cr, y, "Fuel gauge Temp register", value);

	psy("p9320-wireless", "die_temp_raw", a, sizeof(a));
	char online[16];
	psy("max77705-wireless", "online", online, sizeof(online));
	if (a[0] && online[0] == '1')
		snprintf(value, sizeof(value), "raw %s (the chip's own die sensor; its scale is not known here)", a);
	else
		snprintf(value, sizeof(value), "off the pad");
	y = zmc_field(cr, y, "Wireless receiver die", value);

	for (int zone = 0; zone < 16; zone++) {
		char path[96], caption[96];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", zone);
		zmc_read_file(path, a, sizeof(a));
		if (!a[0]) break;
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
		zmc_read_file(path, value, sizeof(value));
		snprintf(caption, sizeof(caption), "Thermal zone %d: %s", zone, a);
		snprintf(value, sizeof(value), "%.1f C%s", atol(value) / 1000.0,
			 !strcmp(a, "max170xx_battery") ? " (the gauge's register, as above)" : "");
		y = zmc_field(cr, y, caption, value);
	}
	return y + 10;
}

double devtest_draw_charging(cairo_t *cr) {
	char value[512], a[96], b[96], c[96], hw[300];
	double y = zmc_header(cr, "charging");
	const char *chg = "max77705-charger", *usbc = "max77705-usbc", *rx = "p9320-wireless";

	/* wired */
	psy(chg, "status", a, sizeof(a));
	psy(chg, "charge_type", b, sizeof(b));
	psy(chg, "charge_behaviour", c, sizeof(c));
	snprintf(value, sizeof(value), "%s, %s; %s", or_unavailable(a), or_unavailable(b), or_unavailable(c));
	y = zmc_field(cr, y, "Charger state", value);
	snprintf(value, sizeof(value), "%ld mA charge, %.2f V float",
		 psy_long(chg, "constant_charge_current", 0) / 1000, psy_long(chg, "constant_charge_voltage", 0) / 1e6);
	y = zmc_field(cr, y, "Charge current and voltage", value);
	psy(chg, "input_current_ramp", a, sizeof(a));
	snprintf(value, sizeof(value), "%ld mA asked, %ld mA in effect, ramp %s; AICL found %ld mA, %ld over-current trips",
		 psy_long(chg, "input_current_limit", 0) / 1000, psy_long(chg, "input_current_limit_effective", 0) / 1000,
		 or_unavailable(a), psy_long(chg, "input_current_limit_found", 0) / 1000, psy_long(chg, "input_current_trips", 0));
	y = zmc_field(cr, y, "Input limit (cable)", value);

	hwmon_dir("max77705", hw, sizeof(hw));
	if (hw[0])
		snprintf(value, sizeof(value), "VBYP %.2f V, VSYS %.2f V, input %ld mA, system %ld mA",
			 hwmon_long(hw, "in0_input") / 1000.0, hwmon_long(hw, "in1_input") / 1000.0,
			 hwmon_long(hw, "curr1_input"), hwmon_long(hw, "curr2_input"));
	else
		snprintf(value, sizeof(value), "unavailable");
	y = zmc_field(cr, y, "PMIC measurements", value);

	psy(usbc, "charger_type", a, sizeof(a));
	psy(usbc, "usb_type", b, sizeof(b));
	snprintf(value, sizeof(value), "%s (%s), %.2f V, up to %ld mA, 9 V %s", or_unavailable(a), or_unavailable(b),
		 psy_long(usbc, "voltage_now", 0) / 1e6, psy_long(usbc, "current_max", 0) / 1000,
		 psy_long(usbc, "hv_enable", 0) ? "allowed" : "off");
	y = zmc_field(cr, y, "Cable and charger", value);
	read_all(PSY "max77705-usbc/pd_source_caps", value, sizeof(value));
	y = zmc_field(cr, y, "PD source offers", value[0] ? value : "none (no PD charger)");
	psy(usbc, "pd_last_event", a, sizeof(a));
	snprintf(value, sizeof(value), "%ld hard resets, %ld renegotiations; last: %s",
		 psy_long(usbc, "pd_hard_resets", 0), psy_long(usbc, "pd_caps_renegotiated", 0), a[0] ? a : "none");
	y = zmc_field(cr, y, "PD events", value);

	/* wireless */
	psy("max77705-wireless", "online", a, sizeof(a));
	if (a[0] != '1') {
		y = zmc_field(cr, y, "Wireless", "off the pad");
	} else {
		snprintf(value, sizeof(value), "Vrect %.2f V, Vout %.2f V (set %ld mV), Iout %ld mA, %ld kHz",
			 psy_long(rx, "vrect", 0) / 1000.0, psy_long(rx, "voltage_now", 0) / 1e6, psy_long(rx, "vout_set", 0),
			 psy_long(rx, "current_now", 0) / 1000, psy_long(rx, "freq", 0));
		y = zmc_field(cr, y, "Wireless receiver", value);
		psy(rx, "pad", a, sizeof(a));
		psy(rx, "tx_id", b, sizeof(b));
		psy(rx, "hv_state", c, sizeof(c));
		snprintf(value, sizeof(value), "%s, TX %s; fast: %s (pad %s, ztOS %s)", or_unavailable(a), or_unavailable(b),
			 or_unavailable(c), psy_long(rx, "hv_capable", 0) ? "can" : "cannot",
			 psy_long(rx, "hv_enable", 0) ? "allows it" : "keeps 5 V");
		y = zmc_field(cr, y, "Pad", value);
		psy(rx, "tx_error", a, sizeof(a));
		psy(rx, "rx_status", b, sizeof(b));
		psy(rx, "fod", c, sizeof(c));
		snprintf(value, sizeof(value), "TX error %s; %s; FOD %s", or_unavailable(a), or_unavailable(b), or_unavailable(c));
		y = zmc_field(cr, y, "Receiver status", value);
		snprintf(value, sizeof(value), "%ld mA asked, %ld mA in effect",
			 psy_long("max77705-wireless", "input_current_limit", 0) / 1000,
			 psy_long("max77705-wireless", "input_current_limit_effective", 0) / 1000);
		y = zmc_field(cr, y, "Input limit (pad)", value);
	}
	psy(rx, "fw", a, sizeof(a));
	y = zmc_field(cr, y, "Receiver firmware", or_unavailable(a));

	/* the battery side */
	struct battery_health bh;
	battery_health_read(hw_battery(), &bh);
	snprintf(value, sizeof(value), "%ld %%, %.3f V, %+ld mA now, %+ld mA average", psy_long("max170xx_battery", "capacity", 0),
		 psy_long("max170xx_battery", "voltage_now", 0) / 1e6, psy_long("max170xx_battery", "current_now", 0) / 1000,
		 psy_long("max170xx_battery", "current_avg", 0) / 1000);
	y = zmc_field(cr, y, "Battery", value);
	battery_health_capacity_text(&bh, value, sizeof(value));
	y = zmc_field(cr, y, "Capacity it holds", value);
	battery_health_gauge_text(&bh, value, sizeof(value));
	y = zmc_field(cr, y, "Cycles (fuel gauge)", value);
	battery_health_samsung_text(&bh, value, sizeof(value));
	y = zmc_field(cr, y, "Cycles (Samsung's count)", value);
	y = zmc_field(cr, y, "Temperature limits", "not in force: zrn-charge and the wireless 10 V check "
		"read the gauge's temperature, which is not measured (temperatures page)");
	return y + 10;
}
