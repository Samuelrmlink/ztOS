#include <assert.h>
#include <stdlib.h>
#include "../audio-apps.h"

#define SPEAKER "{\"name\": \"ztos-speaker\", \"desc\": \"Speaker\", \"kind\": \"speaker\", \"percent\": 70, \"mute\": false, \"current\": true}"
#define BT "bluez_output.00_0D_44_EB_50_54.1"

/* in the daemon's format, with the keys Brave and Grayjay really get */
static const char REAL[] =
	"{\"version\": 1, \"active\": {\"name\": \"ztos-speaker\", \"desc\": \"Speaker\", \"kind\": \"speaker\", "
	"\"percent\": 70, \"mute\": false}, \"outputs\": [" SPEAKER "], \"apps\": ["
	"{\"key\": \"Output/Audio:application.name:Brave\", \"name\": \"Brave\", \"running\": true, \"streams\": [70, 71], "
	"\"percent\": 50, \"mute\": true, \"playing_on\": \"ztos-speaker\", \"output\": \"default\", \"output_present\": true}, "
	"{\"key\": \"Output/Audio:application.name:Chromium\", \"name\": \"Grayjay\", \"running\": true, \"streams\": [72], "
	"\"percent\": 40, \"mute\": false, \"playing_on\": \"ztos-speaker\", \"output\": \"" BT "\", "
	"\"output_present\": false, \"output_desc\": \"UE MINI BOOM\"}, "
	"{\"key\": \"Output/Audio:application.id:mpv\", \"name\": \"mpv\", \"running\": false, \"streams\": [], "
	"\"percent\": null, \"mute\": false, \"playing_on\": null, \"output\": \"default\", \"output_present\": true}]}";

static bool parse(const char *text, struct audio_apps *a) {
	return audio_apps_parse(text, strlen(text), a);
}

static struct audio_apps A;

static void test_real(void) {
	assert(parse(REAL, &A));
	assert(A.n == 3);
	assert(!strcmp(A.app[0].key, "Output/Audio:application.name:Brave"));
	assert(A.app[0].running && A.app[0].mute && A.app[0].percent == 50);
	assert(!strcmp(A.app[0].playing_on, "ztos-speaker") && audio_app_is_default(&A.app[0]));
	assert(!strcmp(A.app[1].name, "Grayjay") && !strcmp(A.app[1].output, BT));
	assert(!A.app[1].output_present && !strcmp(A.app[1].output_desc, "UE MINI BOOM"));
	assert(!A.app[2].running && A.app[2].percent == -1 && !A.app[2].playing_on[0]);
}

/* the file cut short anywhere, as if the write had stopped there */
static void test_truncated(void) {
	static struct audio_apps a;
	a.n = 5;
	size_t len = strlen(REAL);
	for (size_t i = 0; i < len; i++) {
		char *copy = malloc(i ? i : 1);	/* exactly i bytes: a read past them is caught */
		memcpy(copy, REAL, i);
		assert(!audio_apps_parse(copy, i, &a));
		assert(a.n == 5);	/* the last good list stands */
		free(copy);
	}
}

static void test_odd(void) {
	static struct audio_apps a;
	/* no apps at all is a good file: an older daemon */
	a.n = 5;
	assert(parse("{\"version\": 1, \"outputs\": []}", &a) && a.n == 0);
	a.n = 5;
	assert(parse("{\"apps\": []}", &a) && a.n == 0);
	assert(!parse("{\"version\": 2, \"apps\": []}", &a));
	assert(!parse("{\"apps\": [1]}", &a));
	assert(!parse("{\"apps\": {}}", &a));
	assert(!parse("{\"apps\": []} trailing", &a));
	/* no key: nothing to send, so no row. No output: Default. No name: the key */
	assert(parse("{\"apps\": [{\"name\": \"x\"}, {}, {\"key\": \"k\"}]}", &a));
	assert(a.n == 1 && !strcmp(a.app[0].key, "k") && !strcmp(a.app[0].name, "k") && audio_app_is_default(&a.app[0]));
	/* wrong types are empty, not fatal */
	assert(parse("{\"apps\": [{\"key\": \"k\", \"name\": 3, \"percent\": \"x\", \"mute\": 1, \"running\": \"y\"}]}", &a));
	assert(a.n == 1 && a.app[0].percent == -1 && !a.app[0].mute && !a.app[0].running);
	/* numbers out of range are brought in */
	assert(parse("{\"apps\": [{\"key\": \"k\", \"percent\": 900}]}", &a) && a.app[0].percent == 100);
	/* a key that doesn't fit would be some other app */
	char big[1024] = "{\"apps\": [{\"key\": \"";
	size_t at = strlen(big);
	memset(big + at, 'k', 300);
	strcpy(big + at + 300, "\"}, {\"key\": \"ok\"}]}");
	assert(parse(big, &a) && a.n == 1 && !strcmp(a.app[0].key, "ok"));
	/* more apps than rows: the first ones */
	char *many = malloc(AUDIO_STATE_MAX_BYTES);
	at = (size_t)sprintf(many, "{\"apps\": [");
	for (int i = 0; i < AUDIO_MAX_APPS + 10; i++) at += (size_t)sprintf(many + at, "%s{\"key\": \"k%d\"}", i ? ", " : "", i);
	strcpy(many + at, "]}");
	assert(parse(many, &a) && a.n == AUDIO_MAX_APPS && !strcmp(a.app[AUDIO_MAX_APPS - 1].key, "k23"));
	free(many);
}

static void test_choices(void) {
	struct audio_state st;
	static const char two[] = "{\"outputs\": [" SPEAKER ", {\"name\": \"" BT "\", \"desc\": \"UE MINI BOOM\", \"kind\": \"bluetooth\"}]}";
	assert(audio_state_parse(two, strlen(two), &st));
	struct audio_app_choice c[AUDIO_APP_CHOICES];
	struct audio_app a = { .output = "default" };
	int sel = -1;
	/* on Default, the first row says where Default goes now */
	assert(audio_app_choices(&st, &a, c, &sel) == 3 && sel == 0);
	assert(!strcmp(c[0].label, "Default (Speaker)") && !strcmp(c[0].name, "default"));
	assert(!strcmp(c[1].label, "Speaker") && !strcmp(c[1].name, "ztos-speaker"));
	assert(!strcmp(c[2].label, "UE MINI BOOM") && !strcmp(c[2].name, BT));
	/* pinned to the speaker while another output is the global one */
	st.out[0].current = false;
	st.out[1].current = true;
	snprintf(a.output, sizeof(a.output), "ztos-speaker");
	assert(audio_app_choices(&st, &a, c, &sel) == 3 && sel == 1);
	assert(!strcmp(c[0].label, "Default (UE MINI BOOM)"));
	/* pinned to an output that has gone: still a row, still chosen */
	struct audio_state one;
	static const char lone[] = "{\"outputs\": [" SPEAKER "]}";
	assert(audio_state_parse(lone, strlen(lone), &one));
	snprintf(a.output, sizeof(a.output), BT);
	snprintf(a.output_desc, sizeof(a.output_desc), "UE MINI BOOM");
	assert(audio_app_choices(&one, &a, c, &sel) == 3 && sel == 2);
	assert(!strcmp(c[2].label, "UE MINI BOOM (not connected)") && !strcmp(c[2].name, BT));
	/* no description known: the name will do */
	a.output_desc[0] = 0;
	assert(audio_app_choices(&one, &a, c, &sel) == 3 && strstr(c[2].label, BT));
	/* no outputs at all */
	struct audio_state none = { .n = 0 };
	snprintf(a.output, sizeof(a.output), "default");
	assert(audio_app_choices(&none, &a, c, &sel) == 1 && !strcmp(c[0].label, "Default"));
	/* the most rows there can be */
	struct audio_state full = { .n = AUDIO_MAX_OUTPUTS };
	for (int i = 0; i < AUDIO_MAX_OUTPUTS; i++) snprintf(full.out[i].name, sizeof(full.out[i].name), "o%d", i);
	snprintf(a.output, sizeof(a.output), "gone");
	assert(audio_app_choices(&full, &a, c, &sel) == AUDIO_APP_CHOICES && sel == AUDIO_APP_CHOICES - 1);
}

static void test_status(void) {
	struct audio_state st;
	static const char lone[] = "{\"outputs\": [" SPEAKER "]}";
	assert(audio_state_parse(lone, strlen(lone), &st));
	char line[256];
	struct audio_app a = { .output = "default", .running = true, .playing_on = "ztos-speaker" };
	audio_app_status(&st, &a, line, sizeof(line));
	assert(!strcmp(line, "playing on Speaker"));
	snprintf(a.output, sizeof(a.output), BT);
	snprintf(a.output_desc, sizeof(a.output_desc), "UE MINI BOOM");
	audio_app_status(&st, &a, line, sizeof(line));
	assert(!strcmp(line, "playing on Speaker until UE MINI BOOM returns"));
	a.running = false;
	a.playing_on[0] = 0;
	audio_app_status(&st, &a, line, sizeof(line));
	assert(!strcmp(line, "not playing, UE MINI BOOM isn't connected"));
	snprintf(a.output, sizeof(a.output), "default");
	audio_app_status(&st, &a, line, sizeof(line));
	assert(!strcmp(line, "not playing"));
	a.running = true;
	audio_app_status(&st, &a, line, sizeof(line));
	assert(!strcmp(line, "open, not linked to an output"));
}

static void test_commands(void) {
	char line[AUDIO_APP_CMD_MAX];
	assert(audio_cmd_app_volume(line, sizeof(line), "Output/Audio:application.name:Brave", 40));
	assert(!strcmp(line, "app-volume 'Output/Audio:application.name:Brave' '40'"));
	assert(audio_cmd_app_mute(line, sizeof(line), "k", true) && !strcmp(line, "app-mute 'k' '1'"));
	assert(audio_cmd_app_mute(line, sizeof(line), "k", false) && !strcmp(line, "app-mute 'k' '0'"));
	assert(audio_cmd_app_output(line, sizeof(line), "k", "default") && !strcmp(line, "app-output 'k' 'default'"));
	/* spaces and quotes in what an app calls itself */
	assert(audio_cmd_app_volume(line, sizeof(line), "Output/Audio:application.name:Bob's Player", 5));
	assert(!strcmp(line, "app-volume 'Output/Audio:application.name:Bob'\\''s Player' '5'"));
	/* a line feed doesn't make a command; nor does what won't fit */
	assert(!audio_cmd_app_output(line, sizeof(line), "a\nb", "default"));
	assert(!audio_cmd_app_output(line, 8, "k", "default"));
	char q[8];
	assert(!audio_shell_quote(q, sizeof(q), "toolong!"));
	assert(!audio_shell_quote(q, sizeof(q), "''"));
	assert(audio_shell_quote(q, sizeof(q), "abcde") && !strcmp(q, "'abcde'"));
	/* the worst a parsed row can be still fits */
	static struct audio_app worst;
	memset(worst.key, '\'', sizeof(worst.key) - 1);
	memset(worst.output, '\'', sizeof(worst.output) - 1);
	assert(audio_cmd_app_output(line, sizeof(line), worst.key, worst.output));
}

int main(void) {
	test_real();
	test_truncated();
	test_odd();
	test_choices();
	test_status();
	test_commands();
	puts("audio-apps: ok");
	return 0;
}
