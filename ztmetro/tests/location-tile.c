/* The Location tile and the status bar's location mark: check-location
 *  - no gate (no state file): unknown, so the tile is drawn faint
 *  - enabled=, engine= read; only starting/tracking count as "in use"
 *  - a read that changes nothing reports no change (no redraw each second)
 *  - just tapped, the tile keeps what was asked until the gate follows
 *  - the switch writes "on"/"off" to the gate's FIFO, and fails without it */
#define LOC_STATE loc_test_state
#define LOC_CTL loc_test_ctl
static char loc_test_state[200], loc_test_ctl[200];
#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <stdio.h>

static void put(const char *text) {
	FILE *f = fopen(loc_test_state, "w");
	fputs(text, f);
	fclose(f);
}

int main(void) {
	char dir[] = "/tmp/ztlocXXXXXX";
	int bad = 0;
	if (!mkdtemp(dir)) return 2;
	snprintf(loc_test_state, sizeof(loc_test_state), "%s/state", dir);
	snprintf(loc_test_ctl, sizeof(loc_test_ctl), "%s/control", dir);
	static struct zt_server s;

	loc_state_read(&s);
	if (s.loc_known || s.shade_location || s.loc_active) { printf("FAIL: no gate read as known\n"); bad = 1; }

	put("enabled=1\nengine=off\nreaders=0\n");
	if (!loc_state_read(&s)) { printf("FAIL: the gate appearing is no change\n"); bad = 1; }
	if (!s.loc_known || !s.shade_location || s.loc_active) { printf("FAIL: enabled/off misread\n"); bad = 1; }
	if (loc_state_read(&s)) { printf("FAIL: the same state read as a change\n"); bad = 1; }

	put("enabled=1\nengine=starting\nreaders=1\n");
	if (!loc_state_read(&s) || !s.loc_active) { printf("FAIL: starting is not in use\n"); bad = 1; }
	put("enabled=1\nengine=tracking\nreaders=2\n");
	loc_state_read(&s);
	if (!s.loc_active) { printf("FAIL: tracking is not in use\n"); bad = 1; }
	put("enabled=1\nengine=capped\nreaders=1\n");
	loc_state_read(&s);
	if (s.loc_active) { printf("FAIL: capped shown as in use\n"); bad = 1; }
	put("enabled=0\nengine=disabled\nreaders=1\n");
	loc_state_read(&s);
	if (s.shade_location || s.loc_active) { printf("FAIL: disabled misread\n"); bad = 1; }

	/* tapped on; the gate has not written yet: the tile stays on */
	s.shade_location = true;
	s.loc_tile_ms = now_ms() ? now_ms() : 1;
	loc_state_read(&s);
	if (!s.shade_location) { printf("FAIL: the tile flicked back before the gate followed\n"); bad = 1; }
	s.loc_tile_ms = now_ms() - 3000;
	loc_state_read(&s);
	if (s.shade_location) { printf("FAIL: the gate's word not taken after the grace\n"); bad = 1; }

	/* gone: unknown again */
	unlink(loc_test_state);
	if (!loc_state_read(&s) || s.loc_known) { printf("FAIL: the gate going away not seen\n"); bad = 1; }

	/* the switch */
	if (loc_switch(false)) { printf("FAIL: switched with no gate\n"); bad = 1; }
	if (mkfifo(loc_test_ctl, 0600) != 0) return 2;
	int r = open(loc_test_ctl, O_RDONLY | O_NONBLOCK);
	char buf[32] = "";
	if (!loc_switch(false) || !loc_switch(true)) { printf("FAIL: switch not written\n"); bad = 1; }
	ssize_t n = read(r, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	if (strcmp(buf, "off\non\n") != 0) { printf("FAIL: wrote '%s'\n", buf); bad = 1; }
	close(r);
	unlink(loc_test_ctl);
	rmdir(dir);

	if (!bad) printf("PASS: location tile and mark follow the gate; the switch writes its FIFO\n");
	return bad;
}
