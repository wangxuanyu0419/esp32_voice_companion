/*
 * tts_player.c — Streaming TTS via the ClawChat /stream?text=... endpoint.
 *
 * Flow:
 *   1. chat_app calls tts_player_play(text)
 *   2. Text is queued to the player task
 *   3. Player task makes HTTPS GET to https://clawchat.xuanyu.uk/stream?text=<encoded>
 *   4. Response body is decoded with esp_audio_codec and written to I2S
 *   5. On completion (or stop), fires on_complete callback → chat_app sends voice_turn_complete
 */

#include "tts_player.h"
#include "audio_pipeline.h"
#include "config_store.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <limits.h>

static const char *TAG = "TTS_PLAYER";

#define TTS_BASE_URL    "https://clawchat.xuanyu.uk/stream?text="
#define TTS_TEXT_MAX    512
#define TTS_BASE_MAX    128
#define TTS_URL_MAX     (TTS_BASE_MAX + TTS_TEXT_MAX * 3 + 1)
#define TTS_QUEUE_DEPTH 4

/* Audio-broadcast only: keep the encoded HTTP body ahead of the decoder so a
 * short server/network gap cannot starve the already verified I2S writer.
 * Eight queued 8-KiB blocks cap queue memory at 64 KiB; the producer holds
 * at most one additional 8-KiB staging block while coalescing HTTP reads. */
#define PREFETCH_QUEUE_DEPTH       8
#define PREFETCH_CHUNK_MAX         (8u * 1024u)
#define PREFETCH_PREFILL_BYTES     (24u * 1024u)
#define PREFETCH_PUSH_SLICE_MS     50
#define PREFETCH_RECV_WAIT_MS      20
#define PREFETCH_START_WAIT_MS     10000
#define PREFETCH_AFTER_START_MS    2000
#define PREFETCH_HTTP_STACK_WORDS  8192
#define PREFETCH_HTTP_PRIORITY     4
#define PREFETCH_HTTP_BUFFER_SIZE  PREFETCH_CHUNK_MAX

typedef enum {
    TTS_REQUEST_TEXT = 0,
    TTS_REQUEST_AUDIO_URL,
} tts_request_kind_t;

typedef struct {
    tts_request_kind_t kind;
    /* Queue only the caller-owned payload.  Audio-broadcast requests carry
     * the 36-byte stream id; URL construction and config access happen in
     * tts_player_task, never on the websocket receive stack. */
    char value[TTS_TEXT_MAX + 1];
} tts_request_t;

typedef enum {
    PREFETCH_MSG_START = 0,
    PREFETCH_MSG_DATA,
    PREFETCH_MSG_END,
} prefetch_msg_type_t;

typedef struct {
    prefetch_msg_type_t type;
    uint8_t *data;                 /* owned by this queue item */
    size_t len;
    esp_err_t error;
    int http_status;
    bool content_type_supported;
    esp_audio_simple_dec_type_t decoder_type;
} prefetch_msg_t;

/* Player state */
static QueueHandle_t      s_queue         = NULL;
static volatile bool      s_playing       = false;
static volatile bool      s_stop_request  = false;
static bool               s_initialized   = false;
static TaskHandle_t       s_task_handle   = NULL;

static tts_on_complete_cb_t s_complete_cb     = NULL;
static void                *s_complete_cb_arg = NULL;
static tts_on_start_cb_t    s_start_cb        = NULL;
static void                *s_start_cb_arg    = NULL;
static bool                 s_start_notified  = false;
static volatile size_t      s_rx_bytes        = 0;
static bool                 s_active_is_url   = false;

/* One audio-broadcast prefetch session at a time.  The existing tts_player
 * task remains the sole decoder/I2S consumer; a bounded producer task owns
 * only the HTTP client and copies each callback buffer into this queue. */
static QueueHandle_t        s_prefetch_queue = NULL;
static SemaphoreHandle_t    s_prefetch_start_sem = NULL;
static SemaphoreHandle_t    s_prefetch_prefill_sem = NULL;
static SemaphoreHandle_t    s_prefetch_done_sem = NULL;
static const char          *s_prefetch_url = NULL;
static volatile bool        s_prefetch_start_sent = false;
static volatile bool        s_prefetch_prefill_signalled = false;
static volatile bool        s_prefetch_producer_done = false;
static volatile size_t      s_prefetch_queued_bytes = 0;
static uint8_t             *s_prefetch_staging = NULL;
static size_t               s_prefetch_staging_len = 0;
static int64_t              s_prefetch_first_data_us = 0;
static volatile uint32_t    s_prefetch_underruns = 0;
static volatile uint32_t    s_prefetch_decode_errors = 0;
static volatile UBaseType_t s_prefetch_min_queue_depth = (UBaseType_t)-1;
static volatile UBaseType_t s_prefetch_max_queue_depth = 0;
static volatile UBaseType_t s_prefetch_http_min_stack = (UBaseType_t)-1;
static volatile bool        s_prefetch_content_type_supported = false;
static volatile esp_audio_simple_dec_type_t s_prefetch_decoder_type =
    ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;

/* Decoder ownership stays in tts_player_task.  HTTP callbacks run on that
 * task, so websocket_task and capture never touch decoder state or buffers. */
#define DECODE_OUT_INITIAL 8192u
#define DECODE_OUT_MAX     (64u * 1024u)
#define PLAY_OUT_SAMPLES   256u
static esp_audio_simple_dec_handle_t s_decoder = NULL;
static esp_audio_simple_dec_type_t s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
static uint8_t *s_decode_out = NULL;
static size_t s_decode_out_cap = 0;
static bool s_content_type_supported = false;
static bool s_audio_info_ready = false;
static esp_audio_simple_dec_info_t s_audio_info;
static uint32_t s_resample_phase = 0;
static uint8_t s_pcm_carry[8];
static size_t s_pcm_carry_len = 0;
static int16_t s_play_out[PLAY_OUT_SAMPLES];
static size_t s_play_out_count = 0;
static size_t s_decoded_bytes = 0;
static size_t s_pcm_samples = 0;
/* esp_audio_simple_dec_process() still receives the EOS marker with len=0,
 * but its parser rejects a NULL raw buffer before it can flush. */
static uint8_t s_decoder_eos_marker;

/* Diagnostic counters for the bounded HTTP->decoder->I2S path.  They are
 * reset per request and deliberately measure without changing the existing
 * single-consumer playback strategy. */
static int64_t s_stream_start_us = 0;
static int64_t s_first_pcm_us = 0;
static int64_t s_last_pcm_us = 0;
static int64_t s_http_first_data_us = 0;
static int64_t s_http_last_data_us = 0;
static int64_t s_http_max_gap_us = 0;
static uint32_t s_http_chunks = 0;
static uint32_t s_write_calls = 0;
static int64_t s_write_total_us = 0;
static int64_t s_write_max_us = 0;
static uint32_t s_write_errors = 0;

/* Keep the large queue item and URL scratch buffers out of the 8 KiB B task
 * stack.  The stable B task stack is intentionally unchanged; these objects
 * are used only by the single player task. */
static tts_request_t s_task_request;
static char s_http_url[TTS_URL_MAX];
static char s_url_encoded[TTS_TEXT_MAX * 3 + 1];
static char s_tts_base[TTS_BASE_MAX];

static esp_err_t build_audio_stream_url(const char *stream_id,
                                        char *out, size_t out_size);

/* WAV broadcast path: the server preserves the source file type.  Reuse the
 * stable B PCM/I2S writer for canonical PCM16/16 kHz/mono WAV streams. */
#define WAV_HEADER_MAX 512
static uint8_t s_wav_header[WAV_HEADER_MAX];
static size_t  s_wav_header_len;
static size_t  s_wav_data_offset;
static bool    s_stream_wav;
static bool    s_wav_ready;
static uint16_t s_wav_channels;
static uint8_t s_wav_pcm_pending[4];
static size_t  s_wav_pcm_pending_len;
static int16_t s_wav_pcm_block[256];
static size_t  s_wav_pcm_bytes;

static void notify_playback_started(void)
{
    if (s_start_notified) return;
    s_start_notified = true;
    if (s_start_cb) s_start_cb(s_start_cb_arg);
}

typedef enum {
    WAV_PARSE_WAIT = 0,
    WAV_PARSE_READY,
    WAV_PARSE_INVALID,
    WAV_PARSE_UNSUPPORTED,
} wav_parse_result_t;

static uint16_t wav_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t wav_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wav_reset_state(void)
{
    s_wav_header_len = 0;
    s_wav_data_offset = 0;
    s_stream_wav = false;
    s_wav_ready = false;
    s_wav_channels = 0;
    s_wav_pcm_pending_len = 0;
    s_wav_pcm_bytes = 0;
}

/* Parse only the small RIFF header.  The body is intentionally not buffered:
 * /audio-stream is a direct file stream and the existing B I2S writer can
 * pace playback as chunks arrive. */
static wav_parse_result_t wav_parse_header(size_t *data_offset)
{
    if (!data_offset) return WAV_PARSE_INVALID;
    if (s_wav_header_len < 12) return WAV_PARSE_WAIT;
    if (memcmp(s_wav_header, "RIFF", 4) != 0 ||
        memcmp(s_wav_header + 8, "WAVE", 4) != 0) {
        return WAV_PARSE_INVALID;
    }

    bool have_fmt = false;
    size_t pos = 12;
    while (pos + 8 <= s_wav_header_len) {
        const uint8_t *chunk = s_wav_header + pos;
        uint32_t chunk_size = wav_rd32(chunk + 4);
        size_t payload = pos + 8;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) return WAV_PARSE_INVALID;
            if (s_wav_header_len < payload + 16) return WAV_PARSE_WAIT;

            uint16_t format = wav_rd16(s_wav_header + payload);
            uint16_t channels = wav_rd16(s_wav_header + payload + 2);
            uint32_t rate = wav_rd32(s_wav_header + payload + 4);
            uint16_t bits = wav_rd16(s_wav_header + payload + 14);
            if (format != 1 || bits != 16 || rate != AUDIO_SAMPLE_RATE ||
                (channels != 1 && channels != 2)) {
                ESP_LOGW(TAG, "WAV unsupported: format=%u channels=%u rate=%u bits=%u",
                         (unsigned)format, (unsigned)channels,
                         (unsigned)rate, (unsigned)bits);
                return WAV_PARSE_UNSUPPORTED;
            }
            s_wav_channels = channels;
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            /* A normal RIFF file places fmt before data.  Refuse an unusual
             * ordering rather than buffering an unbounded data chunk while
             * searching for format metadata. */
            if (!have_fmt) return WAV_PARSE_UNSUPPORTED;
            *data_offset = payload;
            return WAV_PARSE_READY;
        }

        size_t padded = (size_t)chunk_size + (chunk_size & 1u);
        if (padded > SIZE_MAX - payload) return WAV_PARSE_INVALID;
        if (s_wav_header_len < payload + padded) return WAV_PARSE_WAIT;
        pos = payload + padded;
    }

    return (s_wav_header_len >= WAV_HEADER_MAX) ?
           WAV_PARSE_INVALID : WAV_PARSE_WAIT;
}

static esp_err_t wav_write_pcm(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_OK;
    const size_t frame_bytes = (size_t)s_wav_channels * sizeof(int16_t);
    if (frame_bytes != 2 && frame_bytes != 4) return ESP_ERR_NOT_SUPPORTED;

    s_wav_pcm_bytes += len;
    size_t pos = 0;
    size_t block_count = 0;
    while (pos < len || s_wav_pcm_pending_len == frame_bytes) {
        if (s_wav_pcm_pending_len < frame_bytes && pos < len) {
            size_t need = frame_bytes - s_wav_pcm_pending_len;
            size_t take = len - pos;
            if (take > need) take = need;
            memcpy(s_wav_pcm_pending + s_wav_pcm_pending_len, data + pos, take);
            s_wav_pcm_pending_len += take;
            pos += take;
        }
        if (s_wav_pcm_pending_len < frame_bytes) break;

        int16_t sample = (int16_t)wav_rd16(s_wav_pcm_pending);
        if (s_wav_channels == 2) {
            int16_t right = (int16_t)wav_rd16(s_wav_pcm_pending + 2);
            sample = (int16_t)(((int32_t)sample + (int32_t)right) / 2);
        }
        s_wav_pcm_pending_len = 0;
        s_wav_pcm_block[block_count++] = sample;

        if (block_count == sizeof(s_wav_pcm_block) / sizeof(s_wav_pcm_block[0])) {
            esp_err_t ret = audio_write_pcm(s_wav_pcm_block, block_count, 1000);
            if (ret != ESP_OK) return ret;
            notify_playback_started();
            block_count = 0;
        }
    }

    if (block_count > 0) {
        esp_err_t ret = audio_write_pcm(s_wav_pcm_block, block_count, 1000);
        if (ret == ESP_OK) notify_playback_started();
        return ret;
    }
    return ESP_OK;
}

static esp_err_t wav_handle_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_OK;
    if (s_wav_ready) return wav_write_pcm(data, len);

    size_t old_len = s_wav_header_len;
    size_t room = (old_len < WAV_HEADER_MAX) ? (WAV_HEADER_MAX - old_len) : 0;
    size_t copied = len < room ? len : room;
    if (copied > 0) {
        memcpy(s_wav_header + old_len, data, copied);
        s_wav_header_len += copied;
    }

    size_t data_offset = 0;
    wav_parse_result_t parsed = wav_parse_header(&data_offset);
    if (parsed == WAV_PARSE_WAIT) {
        return (copied < len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
    }
    if (parsed != WAV_PARSE_READY) return ESP_ERR_NOT_SUPPORTED;

    s_wav_data_offset = data_offset;
    s_wav_ready = true;

    /* The header buffer contains every byte received up to this point. */
    esp_err_t ret = ESP_OK;
    if (s_wav_header_len > s_wav_data_offset) {
        ret = wav_write_pcm(s_wav_header + s_wav_data_offset,
                            s_wav_header_len - s_wav_data_offset);
    }
    if (ret == ESP_OK && copied < len) {
        ret = wav_write_pcm(data + copied, len - copied);
    }
    return ret;
}

/* ── Bounded encoded-audio decoder path ──────────────────────────────────── */
static void decoder_reset_output_state(void)
{
    memset(&s_audio_info, 0, sizeof(s_audio_info));
    s_audio_info_ready = false;
    s_resample_phase = 0;
    s_pcm_carry_len = 0;
    s_play_out_count = 0;
    s_decoded_bytes = 0;
    s_pcm_samples = 0;
}

static void decoder_close(void)
{
    if (s_decoder) esp_audio_simple_dec_close(s_decoder);
    s_decoder = NULL;
    decoder_reset_output_state();
}

static esp_err_t decoder_flush_output(void)
{
    if (s_play_out_count == 0) return ESP_OK;
    size_t count = s_play_out_count;
    int64_t write_start_us = esp_timer_get_time();
    esp_err_t ret = audio_write_pcm(s_play_out, count, 1000);
    int64_t write_elapsed_us = esp_timer_get_time() - write_start_us;
    s_write_calls++;
    s_write_total_us += write_elapsed_us;
    if (write_elapsed_us > s_write_max_us) s_write_max_us = write_elapsed_us;
    if (ret == ESP_OK) {
        s_pcm_samples += count;
        if (s_first_pcm_us == 0) s_first_pcm_us = write_start_us;
        /* Record the end of the final successful I2S write as well as the
         * first write.  This separates actual playback duration from TLS,
         * HTTP, and decoder startup latency in the audio-broadcast log. */
        s_last_pcm_us = esp_timer_get_time();
        notify_playback_started();
    } else {
        s_write_errors++;
    }
    s_play_out_count = 0;
    return ret;
}

static int32_t decoder_read_signed_le(const uint8_t *p, int bytes)
{
    uint32_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint32_t)p[i] << (i * 8);
    if (bytes < 4 && (value & (1u << (bytes * 8 - 1)))) {
        value |= ~((1u << (bytes * 8)) - 1u);
    }
    return (int32_t)value;
}

static esp_err_t decoder_write_pcm(const uint8_t *data, size_t len)
{
    if (!s_audio_info_ready || !data || len == 0 || s_audio_info.sample_rate == 0 ||
        s_audio_info.channel < 1 || s_audio_info.channel > 2) {
        return ESP_OK;
    }

    if ((s_audio_info.bits_per_sample & 7u) != 0) return ESP_ERR_NOT_SUPPORTED;
    int bytes_per_sample = s_audio_info.bits_per_sample / 8;
    if (bytes_per_sample < 1 || bytes_per_sample > 4) return ESP_ERR_NOT_SUPPORTED;
    size_t frame_bytes = (size_t)bytes_per_sample * s_audio_info.channel;
    if (frame_bytes > sizeof(s_pcm_carry)) return ESP_ERR_NOT_SUPPORTED;

    for (size_t i = 0; i < len; i++) {
        if (s_pcm_carry_len >= sizeof(s_pcm_carry)) return ESP_ERR_INVALID_SIZE;
        s_pcm_carry[s_pcm_carry_len++] = data[i];
        if (s_pcm_carry_len < frame_bytes) continue;

        int32_t left = decoder_read_signed_le(s_pcm_carry, bytes_per_sample);
        int32_t mono = left;
        if (s_audio_info.channel == 2) {
            int32_t right = decoder_read_signed_le(s_pcm_carry + bytes_per_sample,
                                                   bytes_per_sample);
            mono = (left + right) / 2;
        }
        if (s_audio_info.bits_per_sample > 16) {
            mono >>= (s_audio_info.bits_per_sample - 16);
        } else if (s_audio_info.bits_per_sample < 16) {
            mono <<= (16 - s_audio_info.bits_per_sample);
        }
        if (mono > INT16_MAX) mono = INT16_MAX;
        if (mono < INT16_MIN) mono = INT16_MIN;
        s_pcm_carry_len = 0;

        /* Fixed B/I2S output is 16 kHz mono.  This phase accumulator keeps
         * decoded sample duration intact for 24/32/44.1/48 kHz sources. */
        s_resample_phase += AUDIO_SAMPLE_RATE;
        while (s_resample_phase >= s_audio_info.sample_rate) {
            s_resample_phase -= s_audio_info.sample_rate;
            s_play_out[s_play_out_count++] = (int16_t)mono;
            if (s_play_out_count == PLAY_OUT_SAMPLES) {
                esp_err_t ret = decoder_flush_output();
                if (ret != ESP_OK) return ret;
            }
        }
    }
    return ESP_OK;
}

static bool decoder_ensure_open(void)
{
    if (s_decoder) return true;
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = s_decoder_type,
        .use_frame_dec = false,
    };
    /* The M4A container may carry AAC without an ADTS header.  The vendor
     * simple-decoder reference config enables AAC-Plus handling explicitly;
     * without the container-specific config esp_m4a_dec_open() rejects the
     * stream before it can expose its AAC track.  Keep this object local to
     * the single consumer task and pass it only for M4A. */
    esp_m4a_dec_cfg_t m4a_cfg = {
        .track_idx = 0,
        .aac_plus_enable = true,
    };
    if (s_decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_M4A) {
        cfg.dec_cfg = &m4a_cfg;
        cfg.cfg_size = sizeof(m4a_cfg);
    }
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &s_decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decoder open failed type=0x%08x ret=%d",
                 (unsigned)s_decoder_type, (int)ret);
        s_decoder = NULL;
        return false;
    }
    return true;
}

static bool decoder_process_chunk(const uint8_t *data, size_t len, bool eos)
{
    /* A zero-length EOS after a rejected/empty response must not create a new
     * decoder instance just to flush it. */
    if (!s_decoder && len == 0 && eos) return true;
    if (!decoder_ensure_open()) return false;

    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)((data != NULL || len != 0 || !eos)
                              ? data : &s_decoder_eos_marker),
        .len = (uint32_t)len,
        .eos = eos,
    };
    do {
        esp_audio_simple_dec_out_t out = {
            .buffer = s_decode_out,
            .len = (uint32_t)s_decode_out_cap,
        };
        esp_audio_err_t ret = esp_audio_simple_dec_process(s_decoder, &raw, &out);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > s_decode_out_cap) {
            if (out.needed_size > DECODE_OUT_MAX) {
                ESP_LOGW(TAG, "Decoder output request too large: %u", (unsigned)out.needed_size);
                return false;
            }
            uint8_t *larger = heap_caps_realloc(s_decode_out, out.needed_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!larger) larger = realloc(s_decode_out, out.needed_size);
            if (!larger) return false;
            s_decode_out = larger;
            s_decode_out_cap = out.needed_size;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Decode failed ret=%d", (int)ret);
            return false;
        }
        if (out.decoded_size > 0) {
            if (!s_audio_info_ready) {
                if (esp_audio_simple_dec_get_info(s_decoder, &s_audio_info) != ESP_AUDIO_ERR_OK) {
                    return false;
                }
                if (s_audio_info.sample_rate == 0 || s_audio_info.channel < 1 ||
                    s_audio_info.channel > 2 || (s_audio_info.bits_per_sample & 7u) != 0) {
                    ESP_LOGW(TAG, "Unsupported decoded format: %lu Hz %u-bit %u ch",
                             (unsigned long)s_audio_info.sample_rate,
                             (unsigned)s_audio_info.bits_per_sample,
                             (unsigned)s_audio_info.channel);
                    return false;
                }
                s_audio_info_ready = true;
                ESP_LOGI(TAG, "Decoded audio %lu Hz %u-bit %u ch -> %u Hz mono",
                         (unsigned long)s_audio_info.sample_rate,
                         (unsigned)s_audio_info.bits_per_sample,
                         (unsigned)s_audio_info.channel, (unsigned)AUDIO_SAMPLE_RATE);
            }
            s_decoded_bytes += out.decoded_size;
            if (decoder_write_pcm(out.buffer, out.decoded_size) != ESP_OK) return false;
        }
        if (raw.consumed > raw.len) return false;
        if (raw.consumed == 0) break;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    } while (raw.len > 0 && !s_stop_request);
    return true;
}

/* ── URL percent-encoder ──────────────────────────────────────────────────── */
static void url_encode(const char *src, char *dst, size_t dst_max)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;

    for (const char *s = src; *s && di + 4 < dst_max; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

static void add_configured_auth_headers(esp_http_client_handle_t client)
{
    static app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config_get(&cfg) != ESP_OK) return;

    if (cfg.ws_auth_token[0] != '\0') {
        char bearer[sizeof(cfg.ws_auth_token) + 8];
        snprintf(bearer, sizeof(bearer), "Bearer %s", cfg.ws_auth_token);
        esp_http_client_set_header(client, "Authorization", bearer);
    }
    if (cfg.cf_client_id[0] != '\0') {
        esp_http_client_set_header(client, "CF-Access-Client-Id", cfg.cf_client_id);
    }
    if (cfg.cf_client_secret[0] != '\0') {
        esp_http_client_set_header(client, "CF-Access-Client-Secret",
                                   cfg.cf_client_secret);
    }
}

/* ── Bounded encoded-audio prefetch ──────────────────────────────────────── */
static void prefetch_update_queue_depth(void)
{
    if (!s_prefetch_queue) return;
    UBaseType_t depth = uxQueueMessagesWaiting(s_prefetch_queue);
    if (depth < s_prefetch_min_queue_depth) s_prefetch_min_queue_depth = depth;
    if (depth > s_prefetch_max_queue_depth) s_prefetch_max_queue_depth = depth;
}

static void prefetch_signal_prefill(bool force)
{
    if (s_prefetch_prefill_signalled) return;
    if (!force && s_prefetch_queued_bytes < PREFETCH_PREFILL_BYTES) return;
    s_prefetch_prefill_signalled = true;
    ESP_LOGI(TAG, "Audio prefetch prefill ready cause=%s bytes=%u q=%u",
             force ? "eof" : "target",
             (unsigned)s_prefetch_queued_bytes,
             (unsigned)(s_prefetch_queue
                            ? uxQueueMessagesWaiting(s_prefetch_queue) : 0));
    if (s_prefetch_prefill_sem) xSemaphoreGive(s_prefetch_prefill_sem);
}

static void prefetch_free_message(prefetch_msg_t *msg)
{
    if (!msg || !msg->data) return;
    heap_caps_free(msg->data);
    msg->data = NULL;
}

/* Data producers wait in short bounded slices so stop_request can cancel a
 * full queue.  The terminal message is sent with cancelable=false and the
 * decoder consumer keeps draining until it receives it. */
static bool prefetch_send_message(prefetch_msg_t *msg, bool cancelable)
{
    if (!s_prefetch_queue || !msg) return false;
    for (;;) {
        if (xQueueSend(s_prefetch_queue, msg,
                       pdMS_TO_TICKS(PREFETCH_PUSH_SLICE_MS)) == pdTRUE) {
            if (msg->type == PREFETCH_MSG_DATA) {
                s_prefetch_queued_bytes += msg->len;
                prefetch_signal_prefill(false);
            } else if (msg->type == PREFETCH_MSG_START && s_prefetch_start_sem) {
                xSemaphoreGive(s_prefetch_start_sem);
            }
            prefetch_update_queue_depth();
            return true;
        }
        if (cancelable && s_stop_request) return false;
    }
}

static void prefetch_drain_queue(void)
{
    if (!s_prefetch_queue) return;
    prefetch_msg_t msg;
    while (xQueueReceive(s_prefetch_queue, &msg, 0) == pdTRUE) {
        if (msg.type == PREFETCH_MSG_DATA) {
            if (s_prefetch_queued_bytes >= msg.len) s_prefetch_queued_bytes -= msg.len;
            else s_prefetch_queued_bytes = 0;
        }
        prefetch_free_message(&msg);
    }
    prefetch_update_queue_depth();
}

static void prefetch_reset_state(void)
{
    prefetch_drain_queue();
    if (s_prefetch_start_sem) xSemaphoreTake(s_prefetch_start_sem, 0);
    if (s_prefetch_prefill_sem) xSemaphoreTake(s_prefetch_prefill_sem, 0);
    if (s_prefetch_done_sem) xSemaphoreTake(s_prefetch_done_sem, 0);
    s_prefetch_url = NULL;
    s_prefetch_start_sent = false;
    s_prefetch_prefill_signalled = false;
    s_prefetch_producer_done = false;
    s_prefetch_queued_bytes = 0;
    s_prefetch_staging = NULL;
    s_prefetch_staging_len = 0;
    s_prefetch_first_data_us = 0;
    s_prefetch_underruns = 0;
    s_prefetch_decode_errors = 0;
    s_prefetch_min_queue_depth = (UBaseType_t)-1;
    s_prefetch_max_queue_depth = 0;
    s_prefetch_http_min_stack = (UBaseType_t)-1;
    s_prefetch_content_type_supported = false;
    s_prefetch_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

static bool prefetch_set_content_type(const char *type)
{
    if (!type) return false;
    s_prefetch_content_type_supported = true;
    if (strstr(type, "audio/wav") || strstr(type, "audio/x-wav") ||
        strstr(type, "audio/wave")) {
        s_prefetch_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    } else if (strstr(type, "audio/mpeg") || strstr(type, "audio/mp3") ||
               strstr(type, "mpeg") || strstr(type, "mp3")) {
        s_prefetch_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    } else if (strstr(type, "audio/aac") || strstr(type, "aac")) {
        s_prefetch_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    } else if (strstr(type, "audio/mp4") || strstr(type, "audio/m4a") ||
               strstr(type, "mp4") || strstr(type, "m4a")) {
        s_prefetch_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    } else {
        s_prefetch_content_type_supported = false;
    }
    ESP_LOGI(TAG, "Audio prefetch Content-Type: %s (decoder=0x%08x)",
             type, (unsigned)s_prefetch_decoder_type);
    return s_prefetch_content_type_supported;
}

static void prefetch_log_failure_memory(const char *reason)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    UBaseType_t consumer_stack = s_task_handle ? uxTaskGetStackHighWaterMark(s_task_handle) : 0;
    UBaseType_t producer_stack = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGW(TAG, "Audio prefetch failure memory reason=%s internal_free=%u "
             "internal_largest=%u psram_free=%u psram_largest=%u "
             "q=%u queued_bytes=%u producer_stack=%u consumer_stack=%u",
             reason ? reason : "unknown",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)(s_prefetch_queue ? uxQueueMessagesWaiting(s_prefetch_queue) : 0),
             (unsigned)s_prefetch_queued_bytes,
             (unsigned)producer_stack,
             (unsigned)consumer_stack);
}

/* ── HTTP event handler ───────────────────────────────────────────────────── */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (s_stop_request) return ESP_FAIL;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && evt->header_value &&
                strcasecmp(evt->header_key, "Content-Type") == 0) {
                const char *type = evt->header_value;
                s_content_type_supported = true;
                if (strstr(type, "audio/wav") || strstr(type, "audio/x-wav") ||
                    strstr(type, "audio/wave")) {
                    s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
                } else if (strstr(type, "audio/mpeg") || strstr(type, "audio/mp3") ||
                           strstr(type, "mpeg") || strstr(type, "mp3")) {
                    s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                } else if (strstr(type, "audio/aac") || strstr(type, "aac")) {
                    s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
                } else if (strstr(type, "audio/mp4") || strstr(type, "audio/m4a") ||
                           strstr(type, "mp4") || strstr(type, "m4a")) {
                    s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
                } else {
                    s_content_type_supported = false;
                    ESP_LOGW(TAG, "Unsupported audio Content-Type: %s", type);
                    return ESP_ERR_NOT_SUPPORTED;
                }
                ESP_LOGI(TAG, "Audio stream Content-Type: %s (decoder=0x%08x)",
                         type, (unsigned)s_decoder_type);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                int64_t now_us = esp_timer_get_time();
                if (s_http_first_data_us == 0) {
                    s_http_first_data_us = now_us;
                } else if (s_http_last_data_us > 0) {
                    int64_t gap_us = now_us - s_http_last_data_us;
                    if (gap_us > s_http_max_gap_us) s_http_max_gap_us = gap_us;
                }
                s_http_last_data_us = now_us;
                s_http_chunks++;
                s_rx_bytes += (size_t)evt->data_len;
            }
            if (evt->data_len > 0 &&
                (!s_content_type_supported ||
                 !decoder_process_chunk((const uint8_t *)evt->data,
                                        (size_t)evt->data_len, false))) {
                return ESP_FAIL;
            }
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "HTTP error");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool prefetch_flush_staging(void)
{
    if (!s_prefetch_staging || s_prefetch_staging_len == 0) return true;

    prefetch_msg_t data = {
        .type = PREFETCH_MSG_DATA,
        .data = s_prefetch_staging,
        .len = s_prefetch_staging_len,
    };
    if (!prefetch_send_message(&data, true)) {
        prefetch_free_message(&data);
        s_prefetch_staging = NULL;
        s_prefetch_staging_len = 0;
        return false;
    }
    s_prefetch_staging = NULL;
    s_prefetch_staging_len = 0;
    return true;
}

static bool prefetch_ensure_staging(void)
{
    if (s_prefetch_staging) return true;
    s_prefetch_staging = heap_caps_malloc(PREFETCH_CHUNK_MAX,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prefetch_staging) {
        s_prefetch_staging = heap_caps_malloc(PREFETCH_CHUNK_MAX,
                                              MALLOC_CAP_8BIT);
    }
    return s_prefetch_staging != NULL;
}

/* HTTP callbacks may be much smaller than the client buffer. Coalesce them
 * into bounded owned blocks so queue capacity is measured in useful audio
 * bytes, not tiny HTTP read fragments. A final partial block is flushed at
 * EOF before the terminal marker; stop remains cancelable while queue-full. */
static esp_err_t prefetch_http_event_cb(esp_http_client_event_t *evt)
{
    if (s_stop_request) return ESP_FAIL;
    if (!evt) return ESP_ERR_INVALID_ARG;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        evt->header_value && strcasecmp(evt->header_key, "Content-Type") == 0) {
        if (!prefetch_set_content_type(evt->header_value)) {
            ESP_LOGW(TAG, "Audio prefetch rejected Content-Type: %s",
                     evt->header_value);
            return ESP_ERR_NOT_SUPPORTED;
        }
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_prefetch_first_data_us == 0) s_prefetch_first_data_us = now_us;
    if (s_http_first_data_us == 0) {
        s_http_first_data_us = now_us;
    } else if (s_http_last_data_us > 0) {
        int64_t gap_us = now_us - s_http_last_data_us;
        if (gap_us > s_http_max_gap_us) s_http_max_gap_us = gap_us;
    }
    s_http_last_data_us = now_us;
    s_http_chunks++;
    s_rx_bytes += (size_t)evt->data_len;

    if ((size_t)evt->data_len > PREFETCH_CHUNK_MAX) {
        ESP_LOGW(TAG, "Audio prefetch chunk too large: %d", evt->data_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_prefetch_start_sent) {
        prefetch_msg_t start = {
            .type = PREFETCH_MSG_START,
            .content_type_supported = s_prefetch_content_type_supported,
            .decoder_type = s_prefetch_decoder_type,
        };
        if (!prefetch_send_message(&start, true)) return ESP_FAIL;
        s_prefetch_start_sent = true;
    }

    const uint8_t *src = (const uint8_t *)evt->data;
    size_t remaining = (size_t)evt->data_len;
    while (remaining > 0) {
        if (!prefetch_ensure_staging()) {
            ESP_LOGW(TAG, "Audio prefetch staging allocation failed");
            return ESP_ERR_NO_MEM;
        }
        size_t room = PREFETCH_CHUNK_MAX - s_prefetch_staging_len;
        size_t copy_len = remaining < room ? remaining : room;
        memcpy(s_prefetch_staging + s_prefetch_staging_len, src, copy_len);
        s_prefetch_staging_len += copy_len;
        src += copy_len;
        remaining -= copy_len;
        if (s_prefetch_staging_len == PREFETCH_CHUNK_MAX &&
            !prefetch_flush_staging()) return ESP_FAIL;
    }
    return ESP_OK;
}

static void prefetch_http_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_FAIL;
    int status = 0;
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < s_prefetch_http_min_stack) s_prefetch_http_min_stack = stack_free;

    esp_http_client_config_t http_cfg = {
        .url               = s_prefetch_url,
        .event_handler     = prefetch_http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        /* Keep the TLS/HTTP internal allocation bounded; queue ownership
         * remains eight 8-KiB slots, while normal callbacks are <=4 KiB. */
        .buffer_size       = PREFETCH_HTTP_BUFFER_SIZE,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client) {
        add_configured_auth_headers(client);
        esp_http_client_set_header(client, "Range", "bytes=0-");
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    } else {
        ESP_LOGE(TAG, "Audio prefetch HTTP client init failed");
    }

    if (err == ESP_OK && s_prefetch_staging_len > 0 &&
        !prefetch_flush_staging()) {
        err = s_stop_request ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    if (s_prefetch_staging) {
        heap_caps_free(s_prefetch_staging);
        s_prefetch_staging = NULL;
        s_prefetch_staging_len = 0;
    }

    if (s_stop_request) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_FAIL;
    if (err == ESP_OK && !s_prefetch_content_type_supported) err = ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        prefetch_log_failure_memory(esp_err_to_name(err));
    }

    prefetch_msg_t end = {
        .type = PREFETCH_MSG_END,
        .error = err,
        .http_status = status,
        .content_type_supported = s_prefetch_content_type_supported,
        .decoder_type = s_prefetch_decoder_type,
    };
    /* Do not cancel the terminal item: the decoder owns cleanup and must see
     * the end marker even when the user interrupted or HTTP failed. */
    (void)prefetch_send_message(&end, false);
    prefetch_signal_prefill(true);
    s_prefetch_producer_done = true;

    stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < s_prefetch_http_min_stack) s_prefetch_http_min_stack = stack_free;
    ESP_LOGI(TAG, "Audio prefetch producer done HTTP=%d err=%s stack_free=%u",
             status, esp_err_to_name(err), (unsigned)stack_free);
    if (s_prefetch_done_sem) xSemaphoreGive(s_prefetch_done_sem);
    vTaskDelete(NULL);
}

static esp_err_t play_audio_url_prefetched(const char *url, int *status_out)
{
    if (!url || !s_prefetch_queue || !s_prefetch_start_sem ||
        !s_prefetch_prefill_sem || !s_prefetch_done_sem) return ESP_ERR_INVALID_STATE;

    prefetch_reset_state();
    s_prefetch_url = url;
    if (xTaskCreatePinnedToCore(prefetch_http_task, "audio_http",
                                PREFETCH_HTTP_STACK_WORDS, NULL,
                                PREFETCH_HTTP_PRIORITY, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Audio prefetch producer task creation failed");
        s_prefetch_url = NULL;
        return ESP_ERR_NO_MEM;
    }

    bool decode_ok = true;
    bool started = false;
    bool terminal = false;
    int status = 0;

    if (xSemaphoreTake(s_prefetch_start_sem,
                       pdMS_TO_TICKS(PREFETCH_START_WAIT_MS)) != pdTRUE) {
        if (s_stop_request) decode_ok = false;
        ESP_LOGW(TAG, "Audio prefetch start timeout bytes=%u q=%u",
                 (unsigned)s_prefetch_queued_bytes,
                 (unsigned)uxQueueMessagesWaiting(s_prefetch_queue));
    } else if (xSemaphoreTake(s_prefetch_prefill_sem,
                              pdMS_TO_TICKS(PREFETCH_AFTER_START_MS)) != pdTRUE) {
        if (s_stop_request) decode_ok = false;
        int64_t prefill_elapsed_ms = s_prefetch_first_data_us > 0
            ? (esp_timer_get_time() - s_prefetch_first_data_us) / 1000 : 0;
        ESP_LOGW(TAG, "Audio prefetch prefill timeout bytes=%u q=%u elapsed_ms=%lld",
                 (unsigned)s_prefetch_queued_bytes,
                 (unsigned)uxQueueMessagesWaiting(s_prefetch_queue),
                 (long long)prefill_elapsed_ms);
    }

    while (!terminal) {
        prefetch_msg_t msg = {0};
        if (xQueueReceive(s_prefetch_queue, &msg,
                          pdMS_TO_TICKS(PREFETCH_RECV_WAIT_MS)) != pdTRUE) {
            if (started && !s_prefetch_producer_done && !s_stop_request) {
                s_prefetch_underruns++;
            }
            continue;
        }
        prefetch_update_queue_depth();

        if (msg.type == PREFETCH_MSG_START) {
            s_decoder_type = msg.decoder_type;
            s_content_type_supported = msg.content_type_supported;
            started = true;
            if (!s_content_type_supported) decode_ok = false;
        } else if (msg.type == PREFETCH_MSG_DATA) {
            if (s_prefetch_queued_bytes >= msg.len) s_prefetch_queued_bytes -= msg.len;
            else s_prefetch_queued_bytes = 0;
            if (decode_ok && started && !s_stop_request) {
                if (!decoder_process_chunk(msg.data, msg.len, false)) {
                    decode_ok = false;
                    s_prefetch_decode_errors++;
                    ESP_LOGW(TAG, "Audio prefetch decoder rejected data");
                }
            }
            prefetch_free_message(&msg);
        } else if (msg.type == PREFETCH_MSG_END) {
            status = msg.http_status;
            if (msg.error != ESP_OK || !msg.content_type_supported) decode_ok = false;
            terminal = true;
        } else {
            prefetch_free_message(&msg);
        }
    }

    /* The producer sends END before signaling this semaphore.  Waiting here
     * makes queue reuse safe even if the producer is between send and delete. */
    (void)xSemaphoreTake(s_prefetch_done_sem, pdMS_TO_TICKS(1000));
    prefetch_drain_queue();
    if (status_out) *status_out = status;

    if (s_stop_request) return ESP_ERR_INVALID_STATE;
    if (!decode_ok || status < 200 || status >= 300) return ESP_FAIL;
    return ESP_OK;
}

/* ── Player task ──────────────────────────────────────────────────────────── */
static void tts_player_task(void *arg)
{
    ESP_LOGI(TAG, "TTS player task started");

    while (1) {
        /* Wait for either text TTS or a server-issued audio URL. */
        if (xQueueReceive(s_queue, &s_task_request, portMAX_DELAY) != pdTRUE) continue;

        if (s_stop_request) {
            s_stop_request = false;
            continue;
        }

        char *url = s_http_url;
        if (s_task_request.kind == TTS_REQUEST_AUDIO_URL) {
            /* Resolve the configured server and build the HTTPS URL only on
             * this dedicated consumer task.  The websocket callback must
             * remain a small enqueue operation; config_get() includes a
             * large app_config_t and used to overflow websocket_task. */
            if (build_audio_stream_url(s_task_request.value,
                                       s_http_url, sizeof(s_http_url)) != ESP_OK) {
                ESP_LOGW(TAG, "Audio stream URL rejected for stream id");
                s_playing = false;
                s_active_is_url = false;
                if (s_complete_cb) s_complete_cb(s_complete_cb_arg);
                continue;
            }
            ESP_LOGI(TAG, "Audio broadcast fetch: %.100s", url);
        } else {
            /* TTS base URL is derived from the configured server host so the
             * dropdown in Settings switches TTS too. */
            url_encode(s_task_request.value, s_url_encoded, sizeof(s_url_encoded));
            if (config_get_tts_base_url(s_tts_base, sizeof(s_tts_base)) < 0) {
                strlcpy(s_tts_base, TTS_BASE_URL, sizeof(s_tts_base));
            }
            snprintf(url, sizeof(s_http_url), "%s%s", s_tts_base, s_url_encoded);
            ESP_LOGI(TAG, "TTS fetch: %.80s...", url);
        }

        /* Power the codec/I2S/PA on for this request; released once the
         * request finishes below.  No-op refcount bump if capture or another
         * player already holds the gate. */
        audio_power_acquire("tts_playback");

        s_playing      = true;
        s_stop_request = false;
        s_active_is_url = (s_task_request.kind == TTS_REQUEST_AUDIO_URL);
        s_rx_bytes      = 0;
        s_start_notified = false;
        s_stream_start_us = esp_timer_get_time();
        s_first_pcm_us = 0;
        s_last_pcm_us = 0;
        s_http_first_data_us = 0;
        s_http_last_data_us = 0;
        s_http_max_gap_us = 0;
        s_http_chunks = 0;
        s_write_calls = 0;
        s_write_total_us = 0;
        s_write_max_us = 0;
        s_write_errors = 0;
        wav_reset_state();
        decoder_close();
        s_decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        s_content_type_supported = false;

        if (s_active_is_url) {
            /* Do not capture while the shared I2S bus is being used for TX. */
            audio_stop_capture();
        }

        esp_err_t err = ESP_FAIL;
        int status = 0;
        if (s_active_is_url) {
            /* Audio-broadcast uses the bounded encoded prefetch producer;
             * text TTS remains on the previously validated direct path. */
            err = play_audio_url_prefetched(url, &status);
        } else {
            esp_http_client_config_t http_cfg = {
                .url                = url,
                .event_handler      = http_event_cb,
                .crt_bundle_attach  = esp_crt_bundle_attach,
                .timeout_ms         = 30000,
                .buffer_size        = 4096,
                /* Request headers include Bearer + both Cloudflare Access
                 * headers.  512 bytes is too small for the header generator;
                 * keep this bounded but leave enough room for the request. */
                .buffer_size_tx     = 2048,
            };

            esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            if (client) {
                add_configured_auth_headers(client);
                err = esp_http_client_perform(client);
                status = esp_http_client_get_status_code(client);
                esp_http_client_cleanup(client);
            } else {
                ESP_LOGE(TAG, "HTTP client init failed");
            }
        }

        if (err == ESP_OK && status >= 200 && status < 300 &&
            s_content_type_supported) {
            if (!decoder_process_chunk(NULL, 0, true) ||
                decoder_flush_output() != ESP_OK) {
                err = ESP_FAIL;
            }
        }

        if (err == ESP_OK && status >= 200 && status < 300) {
            ESP_LOGI(TAG, "%s stream done (HTTP %d, %u bytes)",
                     s_active_is_url ? "Audio broadcast" : "TTS",
                     status, (unsigned)s_rx_bytes);
            if (s_pcm_samples == 0) {
                ESP_LOGW(TAG, "Audio stream completed without PCM output");
            } else {
                ESP_LOGI(TAG, "Audio decoded: encoded=%u decoded=%u pcm_samples=%u",
                         (unsigned)s_rx_bytes, (unsigned)s_decoded_bytes,
                         (unsigned)s_pcm_samples);
            }
        } else {
            ESP_LOGW(TAG, "%s stream error: %s (HTTP %d, %u bytes)",
                     s_active_is_url ? "Audio broadcast" : "TTS",
                     esp_err_to_name(err), status, (unsigned)s_rx_bytes);
        }

        if (s_active_is_url && s_stream_start_us > 0) {
            int64_t wall_us = esp_timer_get_time() - s_stream_start_us;
            uint32_t media_ms = (uint32_t)(((uint64_t)s_pcm_samples * 1000U) /
                                           AUDIO_SAMPLE_RATE);
            uint32_t first_pcm_ms = s_first_pcm_us > 0
                ? (uint32_t)((s_first_pcm_us - s_stream_start_us) / 1000) : 0;
            uint32_t playback_ms = (s_first_pcm_us > 0 && s_last_pcm_us >= s_first_pcm_us)
                ? (uint32_t)((s_last_pcm_us - s_first_pcm_us) / 1000) : 0;
            uint32_t first_data_ms = s_http_first_data_us > 0
                ? (uint32_t)((s_http_first_data_us - s_stream_start_us) / 1000) : 0;
            ESP_LOGI(TAG, "Audio timing: media_ms=%u wall_ms=%u first_data_ms=%u "
                     "first_pcm_ms=%u playback_ms=%u http_chunks=%u max_http_gap_ms=%u "
                     "i2s_writes=%u i2s_write_ms=%u i2s_max_write_ms=%u i2s_errors=%u "
                     "prefetch_min_q=%u prefetch_max_q=%u prefetch_underruns=%u "
                     "prefetch_decode_errors=%u prefetch_http_stack=%u tts_stack=%u",
                     (unsigned)media_ms,
                     (unsigned)(wall_us / 1000),
                     (unsigned)first_data_ms,
                     (unsigned)first_pcm_ms,
                     (unsigned)playback_ms,
                     (unsigned)s_http_chunks,
                     (unsigned)(s_http_max_gap_us / 1000),
                     (unsigned)s_write_calls,
                     (unsigned)(s_write_total_us / 1000),
                     (unsigned)(s_write_max_us / 1000),
                     (unsigned)s_write_errors,
                     (unsigned)(s_prefetch_min_queue_depth == (UBaseType_t)-1
                                    ? 0 : s_prefetch_min_queue_depth),
                     (unsigned)s_prefetch_max_queue_depth,
                     (unsigned)s_prefetch_underruns,
                     (unsigned)s_prefetch_decode_errors,
                     (unsigned)(s_prefetch_http_min_stack == (UBaseType_t)-1
                                    ? 0 : s_prefetch_http_min_stack),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        if (s_pcm_carry_len != 0) {
            ESP_LOGW(TAG, "Audio stream ended with partial PCM frame (%u bytes)",
                     (unsigned)s_pcm_carry_len);
        }
        decoder_close();

        s_playing      = false;
        s_stop_request = false;
        s_active_is_url = false;

        audio_power_release("tts_playback");

        /* Notify caller (chat_app will send voice_turn_complete) */
        if (s_complete_cb) s_complete_cb(s_complete_cb_arg);
    }

    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t tts_player_init(void)
{
    if (s_initialized) return ESP_OK;

    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Audio decoder registration failed");
        return ESP_FAIL;
    }
    s_decode_out_cap = DECODE_OUT_INITIAL;
    s_decode_out = heap_caps_malloc(s_decode_out_cap,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_decode_out) s_decode_out = malloc(s_decode_out_cap);
    if (!s_decode_out) {
        ESP_LOGE(TAG, "Decoder output buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(TTS_QUEUE_DEPTH, sizeof(tts_request_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_prefetch_queue = xQueueCreate(PREFETCH_QUEUE_DEPTH, sizeof(prefetch_msg_t));
    s_prefetch_start_sem = xSemaphoreCreateBinary();
    s_prefetch_prefill_sem = xSemaphoreCreateBinary();
    s_prefetch_done_sem = xSemaphoreCreateBinary();
    if (!s_prefetch_queue || !s_prefetch_start_sem ||
        !s_prefetch_prefill_sem || !s_prefetch_done_sem) {
        ESP_LOGE(TAG, "Audio prefetch queue/semaphore creation failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_player_task, "tts_player", 8192, NULL,
        5,          /* priority */
        &s_task_handle,
        1           /* Core 1 (net / audio core) */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        free(s_decode_out);
        s_decode_out = NULL;
        s_decode_out_cap = 0;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TTS player ready");
    return ESP_OK;
}

esp_err_t tts_player_play(const char *text)
{
    if (!s_initialized || !text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;

    tts_request_t req = {.kind = TTS_REQUEST_TEXT};
    strlcpy(req.value, text, TTS_TEXT_MAX + 1);

    if (xQueueSend(s_queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full — dropping text");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool valid_stream_id(const char *stream_id)
{
    if (!stream_id || strlen(stream_id) != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (stream_id[i] != '-') return false;
        } else if (!isxdigit((unsigned char)stream_id[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t build_audio_stream_url(const char *stream_id,
                                        char *out, size_t out_size)
{
    if (!valid_stream_id(stream_id) || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t cfg = {0};
    if (config_get(&cfg) != ESP_OK) return ESP_FAIL;

    const char *scheme = strstr(cfg.server_url, "://");
    if (!scheme || strncmp(cfg.server_url, "wss://", 6) != 0) {
        ESP_LOGW(TAG, "Audio stream requires configured wss:// server");
        return ESP_ERR_INVALID_ARG;
    }
    const char *host = scheme + 3;
    const char *end = strpbrk(host, "/:?#");
    size_t host_len = end ? (size_t)(end - host) : strlen(host);
    if (host_len == 0 || host_len >= 96 || memchr(host, '@', host_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_size, "https://%.*s/audio-stream/%s",
                     (int)host_len, host, stream_id);
    return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t tts_player_play_audio_stream(const char *stream_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!valid_stream_id(stream_id)) return ESP_ERR_INVALID_ARG;

    tts_request_t req = {.kind = TTS_REQUEST_AUDIO_URL};
    strlcpy(req.value, stream_id, sizeof(req.value));
    if (xQueueSend(s_queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full — dropping stream %s", stream_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void tts_player_stop(void)
{
    /* Only arm the stop flag when something is actually in flight.  The task
     * checks s_stop_request right after dequeuing (see tts_player_task), so
     * an unconditional set here would stick if the task was idle and then
     * silently swallow the *next* unrelated request queued after this call
     * returns (e.g. a stray tts_player_stop() from stop_playback_for_interrupt()
     * while a PCM broadcast, not TTS, was actually playing).  There is a small
     * race: s_playing is only set true after dequeue (~line 1084), so a stop
     * landing between dequeue and that assignment won't cancel the request
     * that was just pulled off the queue.  That's an acceptable trade-off —
     * worst case is one extra short reply playing out, versus silently
     * dropping a whole future reply. */
    if (s_playing) {
        s_stop_request = true;
    }
    /* Always drain the queue so pending items don't play after stop. */
    if (s_queue) {
        tts_request_t dummy;
        while (xQueueReceive(s_queue, &dummy, 0) == pdTRUE) {}
    }
}

bool tts_player_is_playing(void) { return s_playing; }

void tts_player_register_complete_callback(tts_on_complete_cb_t cb, void *arg)
{
    s_complete_cb     = cb;
    s_complete_cb_arg = arg;
}

void tts_player_register_start_callback(tts_on_start_cb_t cb, void *arg)
{
    s_start_cb = cb;
    s_start_cb_arg = arg;
}
