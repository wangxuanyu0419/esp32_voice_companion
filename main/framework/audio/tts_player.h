#ifndef TTS_PLAYER_H
#define TTS_PLAYER_H

/*
 * tts_player.h — HTTP MP3 stream TTS player.
 *
 * Usage:
 *   tts_player_init()  — call once during startup
 *   tts_player_play(text) — streams audio for `text`; fires completion cb when done
 *   tts_player_stop()  — interrupt current playback
 */

#include <stdbool.h>
#include "esp_err.h"

typedef void (*tts_on_complete_cb_t)(void *arg);

esp_err_t tts_player_init(void);

/* Submit text for TTS playback.  Non-blocking; the player task does the work. */
esp_err_t tts_player_play(const char *text);

/* Stop current playback immediately.  Safe to call from any task. */
void tts_player_stop(void);

bool tts_player_is_playing(void);

/* Callback fired on the player task when playback finishes. */
void tts_player_register_complete_callback(tts_on_complete_cb_t cb, void *arg);

#endif /* TTS_PLAYER_H */
