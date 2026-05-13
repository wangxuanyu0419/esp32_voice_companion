#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2s_std.h"

/* Audio parameters */
#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_CHANNELS         1
#define AUDIO_BUFFER_SIZE      (16 * 1024)

/* Chunk parameters: 40 ms @ 16 kHz mono PCM16 = 640 samples */
#define AUDIO_CHUNK_SAMPLES    640
#define AUDIO_CHUNK_SIZE_BYTES (AUDIO_CHUNK_SAMPLES * sizeof(int16_t))

esp_err_t audio_init(void);
esp_err_t audio_start_capture(void);
esp_err_t audio_stop_capture(void);
bool      audio_is_capturing(void);

esp_err_t audio_play_tts(const char *text);

typedef enum {
    BEEP_BOOT_PRESS,
    BEEP_LISTENING_START,
    BEEP_CONNECTION_OK,
    BEEP_ERROR,
} audio_beep_type_t;

esp_err_t audio_play_beep(audio_beep_type_t type);
esp_err_t audio_stop_playback(void);
bool      audio_is_playing(void);
esp_err_t audio_set_volume(int volume);
int       audio_get_volume(void);

typedef void (*audio_capture_cb_t)(const int16_t *pcm_data, size_t len, void *arg);
void audio_register_capture_callback(audio_capture_cb_t cb, void *arg);

/** @brief Expose I2S TX channel handle for direct use by tts_player. */
i2s_chan_handle_t audio_get_tx_handle(void);

bool      audio_manager_read_chunk(int16_t *buf, size_t *count, int timeout_ms);
esp_err_t audio_manager_send_interrupt_before_capture(void);
void      audio_manager_flush_accumulator(void);

#endif /* AUDIO_PIPELINE_H */
