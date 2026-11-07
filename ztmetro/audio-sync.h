/* The volume banner's level against the active output's percent, kept apart
 * from the shell so the host can test it (tests/audio-sync.c). */
#ifndef ZT_AUDIO_SYNC_H
#define ZT_AUDIO_SYNC_H
#include <stdbool.h>
#include <stdint.h>

/* While the keys are moving, what the device reports back is behind them. */
#define AUDIO_SYNC_QUIET_MS 800

static inline int audio_percent_of_level(int level, int max) {
	return level * 100 / max;
}

static inline int audio_level_of_percent(int percent, int max) {
	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	return (percent * max + 50) / 100;
}

/* now and last_key are now_ms() values; last_key 0 is no key yet. */
static inline bool audio_sync_quiet(uint32_t now, uint32_t last_key) {
	return last_key && now - last_key < AUDIO_SYNC_QUIET_MS;
}
#endif
