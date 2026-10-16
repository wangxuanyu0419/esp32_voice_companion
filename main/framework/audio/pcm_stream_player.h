#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*pcm_stream_complete_cb_t)(bool ok, void *arg);
typedef void (*pcm_stream_start_cb_t)(void *arg);

esp_err_t pcm_stream_player_init(void);
esp_err_t pcm_stream_player_start(const char *stream_id, int sample_rate, int channels);
esp_err_t pcm_stream_player_push(const uint8_t *data, size_t len);
esp_err_t pcm_stream_player_end(const char *stream_id);
void      pcm_stream_player_stop(void);
bool      pcm_stream_player_is_playing(void);
void      pcm_stream_player_register_complete_callback(pcm_stream_complete_cb_t cb,
                                                       void *arg);
/* Callback fires only after the first PCM block is accepted by I2S. */
void      pcm_stream_player_register_start_callback(pcm_stream_start_cb_t cb,
                                                    void *arg);
