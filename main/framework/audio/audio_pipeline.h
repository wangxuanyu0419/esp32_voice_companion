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

/*
 * Reference-counted audio hardware power gate.  While the refcount is 0 the
 * ES8311 codec is in standby (CLK1/SYS0D/SYS12/SYS13 written to their
 * power-down values), the I2S TX/RX channels are disabled, and the speaker
 * PA (GPIO46) is held low.  Every code path that actually moves audio
 * through the codec — mic capture, TTS playback, WS PCM-stream playback, the
 * media player's raw-I2S WAV playback, and the test tone — must call
 * audio_power_acquire() before touching the codec/I2S and
 * audio_power_release() exactly once when done.
 *
 * Acquiring while the gate is already held by another consumer is cheap
 * (refcount increment only, no re-init, no glitch).  The FIRST acquire pays
 * a ~20 ms settle latency (10 ms for I2S/MCLK to stabilise, 10 ms for the
 * codec's analog/DAC stage to settle before the output driver/PA come back
 * up).  release() is safe to call even if the refcount is already 0 (it
 * logs a warning and does nothing rather than underflowing or powering
 * anything off under a still-active consumer).  `reason` is a short static
 * string used only for the ESP_LOGI acquire/release trace.
 */
esp_err_t audio_power_acquire(const char *reason);
esp_err_t audio_power_release(const char *reason);

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

/* Write mono PCM16 @ 16 kHz to the I2S TX (duplicated to stereo).  Blocks until
 * the DMA accepts the data, pacing playback to real time. */
esp_err_t audio_write_pcm(const int16_t *mono, size_t samples, int timeout_ms);

/* Play a sine test tone — proves the speaker path works. */
esp_err_t audio_play_test_tone(int freq_hz, int duration_ms);
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
