// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Settings > Battery: the battery temperature sensor (zrn-therm), and "Try
 * again" when its driver's last load never finished (power/thermistors/README.md).
 *
 * zrn-therm-load leaves /var/lib/zrn-therm/load-failed then and loads nothing
 * more: the temperature is unknown and charging stays at 5 V. "Try again"
 * runs `sudo -n /usr/local/sbin/zrn-therm-load --retry`, the one command the
 * sudoers rule /etc/sudoers.d/zrn-therm allows without a password. ztnotifyd
 * takes the notification away when the mark goes (notifyd/therm-watch.h).
 */
#ifndef ZT_THERM_RETRY_H
#define ZT_THERM_RETRY_H
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define THERM_FAILED	"/var/lib/zrn-therm/load-failed"
#define THERM_BATTERY	"/sys/kernel/zrn_therm/battery"
#define ACT_THERM_RETRY	90000	/* between ztsettings.c's own actions and SOUND_ACTIONS */

static double therm_section(cairo_t *cr, double y)
{
	char t[32] = "", value[160];
	FILE *f = fopen(THERM_BATTERY, "re");

	if (f) {
		if (!fgets(t, sizeof(t), f))
			t[0] = 0;
		fclose(f);
	}
	if (access(THERM_FAILED, F_OK) == 0) {
		y = zmc_field(cr, y, "Battery temperature sensor off",
			"Its driver did not finish loading at the last start, so it is not loaded again "
			"by itself. Without it the temperature is unknown and charging stays at 5 V.");
		return zmc_button(cr, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN, "Try again", ACT_THERM_RETRY, 0) + 16;
	}
	if (t[0])
		snprintf(value, sizeof(value), "%.1f C, measured", atol(t) / 10.0);
	else
		snprintf(value, sizeof(value), "unknown: charging stays at 5 V");
	return zmc_field(cr, y, "Battery temperature", value);
}

/* Clears the mark and loads the driver, as root; takes about a second. */
static void therm_retry(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		execl("/usr/bin/sudo", "sudo", "-n", "/usr/local/sbin/zrn-therm-load", "--retry", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
	zmc_redraw();
}
#endif
