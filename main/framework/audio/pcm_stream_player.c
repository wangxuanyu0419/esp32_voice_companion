#include "pcm_stream_player.h"
#include "audio_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PCM_STREAM";

/* A bounded queue keeps a fast server stream from consuming all PSRAM when
 * the I2S writer is momentarily busy.  Each queued data item owns its buffer. */
#define PCM_QUEUE_DEPTH 18
#define PCM_STREAM_ID_MAX 64
#define PCM_OUT_BLOCK_SAMPLES 256
#define PCM_QUEUE_WAIT_MS 1000

typedef enum {
    PCM_MSG_START,
    PCM_MSG_DATA,
    PCM_MSG_END,
    PCM_MSG_STOP,
} pcm_msg_type_t;

typedef struct {
    pcm_msg_type_t type;
    char stream_id[PCM_STREAM_ID_MAX];
    int sample_rate;
    int channels;
    uint32_t sequence;
    uint32_t checksum;
    uint8_t *data;
    size_t len;
} pcm_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task_handle;
static volatile bool s_accepting;
static volatile bool s_playing;
static volatile bool s_stream_aborted;
static const char * volatile s_stream_abort_reason;
static volatile size_t s_stream_received_bytes;
static volatile size_t s_stream_bytes;
static volatile size_t s_stream_dropped_bytes;
static volatile size_t s_stream_output_samples;
static volatile uint32_t s_stream_drops;
static volatile uint32_t s_stream_received_chunks;
static volatile uint32_t s_stream_enqueued_chunks;
static volatile uint32_t s_stream_odd_chunks;
static volatile uint32_t s_stream_crc_errors;
static volatile uint32_t s_stream_i2s_errors;
static volatile UBaseType_t s_stream_max_queue_depth;
static volatile size_t s_stream_max_chunk_bytes;
static volatile uint32_t s_stream_next_sequence;
static int64_t s_stream_start_us;
static volatile UBaseType_t s_min_stack_words = (UBaseType_t)-1;
static pcm_stream_complete_cb_t s_complete_cb;
static void *s_complete_arg;
static pcm_stream_start_cb_t s_start_cb;
static void *s_start_arg;
static bool s_start_notified;

/* Tracks whether THIS player currently holds the audio power gate, so START/
 * STOP/END can acquire/release exactly once regardless of ordering (a STOP
 * with no preceding START, or an END after a STOP already released, must not
 * double-release). */
static bool s_power_held;

static void notify_playback_started(void)
{
    if (s_start_notified) return;
    s_start_notified = true;
    if (s_start_cb) s_start_cb(s_start_arg);
}

static void update_stack_watermark(void)
{
    UBaseType_t free_words = uxTaskGetStackHighWaterMark(NULL);
    if (free_words < s_min_stack_words) s_min_stack_words = free_words;
}

static uint32_t pcm_checksum(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u; /* FNV-1a: cheap integrity check, no buffer */
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void mark_stream_aborted(const char *reason)
{
    if (!s_stream_aborted) {
        s_stream_aborted = true;
        s_stream_abort_reason = reason;
        ESP_LOGW(TAG, "PCM stream aborted: %s", reason ? reason : "unknown");
    }
}

static void free_message(pcm_msg_t *msg)
{
    if (msg && msg->data) free(msg->data);
    if (msg) msg->data = NULL;
}

static void drain_queue(void)
{
    if (!s_queue) return;
    pcm_msg_t msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) free_message(&msg);
}

static void player_task(void *arg)
{
    (void)arg;
    update_stack_watermark();
    ESP_LOGI(TAG, "PCM task ready stack_free=%u heap=%u",
             (unsigned)s_min_stack_words,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    pcm_msg_t msg;
    char active_id[PCM_STREAM_ID_MAX] = {0};
    int input_rate = AUDIO_SAMPLE_RATE;
    int input_channels = 1;
    uint32_t phase = 0;
    uint8_t frame_bytes[4] = {0};
    size_t frame_fill = 0;
    int16_t output[PCM_OUT_BLOCK_SAMPLES];
    size_t output_count = 0;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.type == PCM_MSG_STOP) {
            free_message(&msg);
            drain_queue();
            active_id[0] = '\0';
            frame_fill = output_count = 0;
            phase = 0;
            s_playing = false;
            if (s_power_held) {
                audio_power_release("pcm_stream");
                s_power_held = false;
            }
            update_stack_watermark();
            continue;
        }

        if (msg.type == PCM_MSG_START) {
            /* Stop the capture path before taking ownership of the I2S output.
             * The underlying B audio driver remains the only I2S/codec owner. */
            audio_stop_capture();
            /* Power the codec/I2S/PA on for this stream (no-op refcount bump
             * if capture or another player already holds the gate). */
            if (!s_power_held) {
                audio_power_acquire("pcm_stream");
                s_power_held = true;
            }
            strlcpy(active_id, msg.stream_id, sizeof(active_id));
            input_rate = msg.sample_rate;
            input_channels = msg.channels;
            phase = 0;
            frame_fill = output_count = 0;
            s_stream_start_us = esp_timer_get_time();
            s_start_notified = false;
            s_playing = true;
            ESP_LOGI(TAG, "PCM start id=%s rate=%d channels=%d",
                     active_id, input_rate, input_channels);
            free_message(&msg);
            continue;
        }

        if (msg.type == PCM_MSG_DATA) {
            if (s_stream_aborted) {
                free_message(&msg);
                continue;
            }
            if (!s_playing || input_rate < 8000 || input_rate > 48000 ||
                input_channels < 1 || input_channels > 2) {
                free_message(&msg);
                continue;
            }

            if (pcm_checksum(msg.data, msg.len) != msg.checksum) {
                s_stream_crc_errors++;
                ESP_LOGW(TAG, "PCM chunk checksum mismatch seq=%u len=%u",
                         (unsigned)msg.sequence, (unsigned)msg.len);
                mark_stream_aborted("chunk checksum");
                free_message(&msg);
                continue;
            }

            const size_t bytes_per_frame = (size_t)input_channels * sizeof(int16_t);
            for (size_t i = 0; i < msg.len; i++) {
                if (s_stream_aborted) break;
                frame_bytes[frame_fill++] = msg.data[i];
                if (frame_fill < bytes_per_frame) continue;

                int16_t left = (int16_t)((uint16_t)frame_bytes[0] |
                                         ((uint16_t)frame_bytes[1] << 8));
                int32_t mono = left;
                if (input_channels == 2) {
                    int16_t right = (int16_t)((uint16_t)frame_bytes[2] |
                                              ((uint16_t)frame_bytes[3] << 8));
                    mono = ((int32_t)left + (int32_t)right) / 2;
                }
                frame_fill = 0;

                /* Lightweight nearest-neighbour resampling into the B output
                 * rate.  This is intentionally kept out of the I2S driver. */
                phase += AUDIO_SAMPLE_RATE;
                while (phase >= (uint32_t)input_rate) {
                    phase -= (uint32_t)input_rate;
                    output[output_count++] = (int16_t)mono;
                    s_stream_output_samples++;
                    if (output_count == PCM_OUT_BLOCK_SAMPLES) {
                        esp_err_t write_ret = audio_write_pcm(output, output_count, 1000);
                        if (write_ret != ESP_OK) {
                            s_stream_i2s_errors++;
                            mark_stream_aborted("I2S write");
                            ESP_LOGW(TAG, "I2S write failed");
                        } else notify_playback_started();
                        output_count = 0;
                    }
                }
            }
            s_stream_bytes += msg.len;
            free_message(&msg);
            update_stack_watermark();
            continue;
        }

        if (msg.type == PCM_MSG_END) {
            bool matching = s_playing &&
                (msg.stream_id[0] == '\0' || strcmp(active_id, msg.stream_id) == 0);
            if (matching && output_count > 0) {
                esp_err_t write_ret = audio_write_pcm(output, output_count, 1000);
                if (write_ret != ESP_OK) {
                    s_stream_i2s_errors++;
                    mark_stream_aborted("I2S final write");
                    ESP_LOGW(TAG, "I2S final write failed");
                } else notify_playback_started();
            }
            output_count = frame_fill = 0;
            phase = 0;
            s_playing = false;
            s_accepting = false;
            if (s_power_held) {
                audio_power_release("pcm_stream");
                s_power_held = false;
            }
            update_stack_watermark();
            const size_t bytes_per_frame = (size_t)(input_channels > 0 ? input_channels : 1)
                                           * sizeof(int16_t);
            const uint32_t input_frames = bytes_per_frame
                ? (uint32_t)(s_stream_bytes / bytes_per_frame) : 0;
            const uint32_t received_frames = bytes_per_frame
                ? (uint32_t)(s_stream_received_bytes / bytes_per_frame) : 0;
            const uint32_t input_ms = input_rate > 0
                ? (uint32_t)(((uint64_t)input_frames * 1000U) / (uint32_t)input_rate) : 0;
            const uint32_t received_ms = input_rate > 0
                ? (uint32_t)(((uint64_t)received_frames * 1000U) / (uint32_t)input_rate) : 0;
            const uint32_t output_ms = (uint32_t)(((uint64_t)s_stream_output_samples * 1000U)
                                                   / AUDIO_SAMPLE_RATE);
            const uint32_t elapsed_ms = s_stream_start_us > 0
                ? (uint32_t)((esp_timer_get_time() - s_stream_start_us) / 1000) : 0;
            ESP_LOGI(TAG, "PCM end id=%s aborted=%d reason=%s rate=%d->%d rx_bytes=%u enq_bytes=%u drop_bytes=%u rx_ms=%u enq_ms=%u in_frames=%u out_samples=%u in_ms=%u out_ms=%u elapsed_ms=%u rx_chunks=%u enq_chunks=%u drop_chunks=%u odd_chunks=%u max_chunk=%u max_q=%u crc_errors=%u i2s_errors=%u stack_free=%u heap=%u",
                     active_id,
                     s_stream_aborted ? 1 : 0,
                     s_stream_abort_reason ? s_stream_abort_reason : "none",
                     input_rate, AUDIO_SAMPLE_RATE,
                     (unsigned)s_stream_received_bytes, (unsigned)s_stream_bytes,
                     (unsigned)s_stream_dropped_bytes,
                     (unsigned)received_ms, (unsigned)input_ms,
                     (unsigned)input_frames, (unsigned)s_stream_output_samples,
                     (unsigned)input_ms, (unsigned)output_ms, (unsigned)elapsed_ms,
                     (unsigned)s_stream_received_chunks, (unsigned)s_stream_enqueued_chunks,
                     (unsigned)s_stream_drops, (unsigned)s_stream_odd_chunks,
                     (unsigned)s_stream_max_chunk_bytes, (unsigned)s_stream_max_queue_depth,
                     (unsigned)s_stream_crc_errors, (unsigned)s_stream_i2s_errors,
                     (unsigned)s_min_stack_words,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            active_id[0] = '\0';
            free_message(&msg);
            if (s_complete_cb) s_complete_cb(matching && !s_stream_aborted, s_complete_arg);
        }
    }
}

esp_err_t pcm_stream_player_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(PCM_QUEUE_DEPTH, sizeof(pcm_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(player_task, "pcm_stream", 6144, NULL, 7,
                                &s_task_handle, 1) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t queue_control(pcm_msg_type_t type, const char *stream_id,
                               int sample_rate, int channels)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    pcm_msg_t msg = {
        .type = type,
        .sample_rate = sample_rate,
        .channels = channels,
    };
    if (stream_id) strlcpy(msg.stream_id, stream_id, sizeof(msg.stream_id));
    return xQueueSend(s_queue, &msg, pdMS_TO_TICKS(PCM_QUEUE_WAIT_MS)) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pcm_stream_player_start(const char *stream_id, int sample_rate, int channels)
{
    if (!stream_id || !*stream_id || sample_rate < 8000 || sample_rate > 48000 ||
        channels < 1 || channels > 2) return ESP_ERR_INVALID_ARG;
    /* Supersede queued data from the previous stream without placing STOP in
     * front of the new START; STOP drains the queue and could discard START. */
    s_accepting = false;
    drain_queue();
    s_stream_aborted = false;
    s_stream_abort_reason = NULL;
    s_stream_received_bytes = 0;
    s_stream_bytes = 0;
    s_stream_dropped_bytes = 0;
    s_stream_output_samples = 0;
    s_stream_drops = 0;
    s_stream_received_chunks = 0;
    s_stream_enqueued_chunks = 0;
    s_stream_odd_chunks = 0;
    s_stream_crc_errors = 0;
    s_stream_i2s_errors = 0;
    s_stream_max_queue_depth = 0;
    s_stream_max_chunk_bytes = 0;
    s_stream_next_sequence = 0;
    esp_err_t ret = queue_control(PCM_MSG_START, stream_id, sample_rate, channels);
    if (ret == ESP_OK) s_accepting = true;
    return ret;
}

esp_err_t pcm_stream_player_push(const uint8_t *data, size_t len)
{
    if (!s_queue || !s_accepting || s_stream_aborted || !data || len == 0)
        return ESP_ERR_INVALID_STATE;
    s_stream_received_bytes += len;
    s_stream_received_chunks++;
    if (len & 1U) s_stream_odd_chunks++;
    if (len > s_stream_max_chunk_bytes) s_stream_max_chunk_bytes = len;

    pcm_msg_t msg = {
        .type = PCM_MSG_DATA,
        .sequence = ++s_stream_next_sequence,
        .checksum = pcm_checksum(data, len),
        .len = len,
    };
    msg.data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg.data) msg.data = malloc(len);
    if (!msg.data) {
        s_stream_dropped_bytes += len;
        mark_stream_aborted("PCM buffer allocation");
        return ESP_ERR_NO_MEM;
    }
    memcpy(msg.data, data, len);
    /* Apply bounded back-pressure while the single PCM consumer drains at
     * the hardware rate.  This avoids dropping odd-sized byte ranges, which
     * would otherwise shift PCM16 framing and produce white noise. */
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(PCM_QUEUE_WAIT_MS)) != pdTRUE) {
        free(msg.data);
        s_stream_dropped_bytes += len;
        s_stream_drops++;
        mark_stream_aborted("PCM queue timeout");
        ESP_LOGW(TAG, "PCM queue full; dropping %u bytes", (unsigned)len);
        return ESP_ERR_TIMEOUT;
    }
    s_stream_enqueued_chunks++;
    UBaseType_t queue_depth = uxQueueMessagesWaiting(s_queue);
    if (queue_depth > s_stream_max_queue_depth) s_stream_max_queue_depth = queue_depth;
    return ESP_OK;
}

esp_err_t pcm_stream_player_end(const char *stream_id)
{
    if (!s_queue || !s_accepting) return ESP_ERR_INVALID_STATE;
    return queue_control(PCM_MSG_END, stream_id, 0, 0);
}

void pcm_stream_player_stop(void)
{
    s_accepting = false;
    if (!s_queue) return;
    pcm_msg_t msg = {.type = PCM_MSG_STOP};
    if (xQueueSendToFront(s_queue, &msg, 0) != pdTRUE) {
        drain_queue();
        xQueueSendToFront(s_queue, &msg, 0);
    }
}

bool pcm_stream_player_is_playing(void) { return s_playing; }

void pcm_stream_player_register_complete_callback(pcm_stream_complete_cb_t cb,
                                                   void *arg)
{
    s_complete_cb = cb;
    s_complete_arg = arg;
}

void pcm_stream_player_register_start_callback(pcm_stream_start_cb_t cb,
                                                void *arg)
{
    s_start_cb = cb;
    s_start_arg = arg;
}
