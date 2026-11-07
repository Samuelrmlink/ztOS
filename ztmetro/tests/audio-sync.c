#include <assert.h>
#include <stdio.h>
#include "../audio-sync.h"

int main(void) {
	/* a key step survives the trip through the device and back */
	for (int max = 15; max <= 30; max += 15) {
		for (int level = 0; level <= max; level++) {
			assert(audio_level_of_percent(audio_percent_of_level(level, max), max) == level);
		}
	}
	/* what the speaker's buttons report lands on the scale */
	assert(audio_level_of_percent(0, 30) == 0);
	assert(audio_level_of_percent(25, 30) == 8);
	assert(audio_level_of_percent(100, 30) == 30);
	assert(audio_level_of_percent(140, 30) == 30);
	assert(audio_level_of_percent(-3, 30) == 0);
	/* the echo of a key is ignored for a moment, and a clock wrap is no matter */
	assert(!audio_sync_quiet(5000, 0));
	assert(audio_sync_quiet(5000, 4500));
	assert(!audio_sync_quiet(5000, 4000));
	assert(audio_sync_quiet(100, UINT32_MAX - 100));
	puts("audio-sync: ok");
	return 0;
}
