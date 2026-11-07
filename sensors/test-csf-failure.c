/* Exercise CSF recovery exhaustion without services, sysfs writes, or delays:
 * make -C ztmetro check-csf
 */
#define CSF_CTL "/tmp/test-csf-failure.ctl"
#define main csf_main
#define system mock_system
#define access mock_access
#define fopen mock_fopen
#define sleep mock_sleep
#define opendir mock_opendir
#include "../ztmetro/csf/ztoscsf.c"
#undef main
#undef system
#undef access
#undef fopen
#undef sleep
#undef opendir
#include <assert.h>

static int resets, restarts, stops, sensord_restarts;
static bool reset_fails, hub_unit_there = true, stop_mark, ready_now;

int mock_system(const char *command) {
	if (!strcmp(command, "systemctl stop ztoscsf-sensord.service")) {
		assert(stops == resets * 2);
		stops++;
	} else if (!strcmp(command, "systemctl stop ztoscsf-sensorhub.service")) {
		assert(stops == resets * 2 + 1);
		stops++;
	} else if (strstr(command, "/sensorhub/reset")) {
		resets++;
		assert(stops == resets * 2);
		return reset_fails ? 256 : 0;
	} else if (!strcmp(command, "systemctl restart --no-block ztoscsf-sensorhub.service")) {
		restarts++;
	} else if (!strcmp(command, "systemctl restart --no-block ztoscsf-sensord.service")) {
		sensord_restarts++;
	} else {
		assert(!strcmp(command, "systemctl stop ztoscsf-sensord.service ztoscsf-sensorhub.service"));
		assert(resets == 2 && stops == 4);
		stops++;
	}
	return 0;
}
int mock_access(const char *path, int mode) {
	(void)mode;
	if (strstr(path, "/systemd/system/ztoscsf-sensorhub.service"))
		return hub_unit_there ? 0 : -1;
	if (strstr(path, ".stopped"))
		return stop_mark ? 0 : -1;
	return -1;
}
DIR *mock_opendir(const char *path) { return NULL; }
FILE *mock_fopen(const char *path, const char *mode) {
	if (strcmp(path, "/sys/class/sec/sensorhub/ready"))	/* the phone's user.conf: none here */
		return NULL;
	FILE *stream = tmpfile();
	assert(stream);
	fputs(ready_now ? "1\n" : "0\n", stream);
	rewind(stream);
	return stream;
}
/* The main loop must never wait for the firmware. */
unsigned int mock_sleep(unsigned int seconds) { (void)seconds; assert(!"slept in the recovery"); return 0; }

static void start_fresh(void) {
	resets = restarts = stops = sensord_restarts = 0;
	hub_attempt = 0;
	hub_dialog_shown = false;
	q_len = 0;
	started_at = now_s() - 100;
}

int main(int argc, char **argv) {
	reset_fails = argc > 1;

	/* Both attempts fail: one reset, then the wait, without blocking. */
	start_fresh();
	sensorhub_look();
	if (reset_fails) {
		assert(!hub_recovering());	/* both failed at once */
	} else {
		assert(hub_recovering() && hub_attempt == 1 && resets == 1 && restarts == 1 && stops == 2);
		hub_recovery_step();		/* the wait isn't over: nothing happens */
		assert(hub_attempt == 1 && resets == 1);
		sensorhub_look();		/* the periodic look leaves it alone */
		assert(hub_attempt == 1 && resets == 1);
		hub_deadline = 0;
		hub_recovery_step();
		assert(hub_attempt == 2 && resets == 2 && restarts == 2 && stops == 4);
		hub_deadline = 0;
		hub_recovery_step();
	}
	assert(!hub_recovering());
	assert(resets == 2 && stops == 5);
	assert(restarts == (reset_fails ? 0 : 2));
	assert(q_len == 1 && strstr(queue[0], "Sensor hub failed to initialize."));
	sensorhub_look();
	assert(resets == 2 && stops == 5 && q_len == 1);
	puts("CSF failure: both units stopped after two attempts; no repeated recovery.");

	if (reset_fails)
		return 0;

	/* Firmware comes up during the first wait: sensord back, nothing else. */
	start_fresh();
	sensorhub_look();
	ready_now = true;
	hub_recovery_step();
	assert(!hub_recovering() && !hub_dialog_shown && resets == 1 && sensord_restarts == 1);
	ready_now = false;
	puts("CSF recovery: stops waiting as soon as the hub is ready.");

	/* The button during a wait: no more attempts. */
	start_fresh();
	sensorhub_look();
	stop_mark = true;
	hub_deadline = 0;
	hub_recovery_step();
	assert(!hub_recovering() && !hub_dialog_shown && resets == 1);
	stop_mark = false;
	puts("CSF recovery: a stop during the wait ends it.");

	/* A device without the hub: not looked at. */
	start_fresh();
	hub_unit_there = false;
	sensorhub_look();
	assert(!hub_recovering() && resets == 0 && stops == 0 && q_len == 0);
	hub_unit_there = true;
	puts("CSF: no hub check on a device without the hub.");

	/* The control pipe: the phone's user and root, nobody else. */
	int fd = ctl_open();
	struct stat st;
	assert(fd >= 0 && lstat(CSF_CTL, &st) == 0);
	assert((st.st_mode & 07777) == 0620 && S_ISFIFO(st.st_mode));
	close(fd);
	unlink(CSF_CTL);
	puts("CSF: control pipe is 0620.");
	return 0;
}
