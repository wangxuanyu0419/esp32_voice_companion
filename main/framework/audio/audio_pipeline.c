/*
 * audio_pipeline.c — ES8311 + I2S audio capture and playback.
 * Moved from 03_audio/audio_manager.c; hardware init stubs remain for Phase B.
 */

#include "audio_pipeline.h"
#include "config_store.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "AUDIO_PIPELINE";

/* I2S GPIO (Waveshare AMOLED-1.8) */
#define I2S_BCK_IO  GPIO_NUM_15
#define I2S_WS_IO   GPIO_NUM_16
#define I2S_DO_IO   GPIO_NUM_17
#define I2S_DI_IO   GPIO_NUM_18
#define I2S_NUM_CH  I2S_NUM_0

#define DMA_BUFFER_SIZE  1024
#define DMA_BUFFER_COUNT 8

static bool             s_capturing  = false;
static bool             s_playing    = false;
static int              s_volume     = 80;
static bool             s_initialized = false;

static int16_t          s_audio_buffer[AUDIO_BUFFER_SIZE];
static size_t           s_buffer_pos  = 0;

static audio_capture_cb_t s_capture_cb     = NULL;
static void              *s_capture_cb_arg  = NULL;

static SemaphoreHandle_t  s_audio_mutex      = NULL;
static int16_t            s_dma_buffer[DMA_BUFFER_SIZE];

/* Chunk queue for 40 ms PCM chunks */
static QueueHandle_t      s_chunk_queue          = NULL;
static bool               s_chunk_queue_init      = false;
static int16_t            s_chunk_accum[AUDIO_CHUNK_SAMPLES];
static size_t             s_chunk_accum_count     = 0;

/* -------------------------------------------------------------------------
 * Chunk accumulator (called from capture task)
 * ------------------------------------------------------------------------- */
static void chunk_accumulator_push(const int16_t *samples, size_t count)
{
    if (!s_chunk_queue_init || !s_capturing) return;

    for (size_t i = 0; i < count; i++) {
        s_chunk_accum[s_chunk_accum_count++] = samples[i];
        if (s_chunk_accum_count >= AUDIO_CHUNK_SAMPLES) {
            int16_t chunk[AUDIO_CHUNK_SAMPLES];
            memcpy(chunk, s_chunk_accum, sizeof(chunk));
            s_chunk_accum_count = 0;
            BaseType_t woken = pdFALSE;
            if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
                int16_t dummy[AUDIO_CHUNK_SAMPLES];
                xQueueReceiveFromISR(s_chunk_queue, dummy, &woken);
                xQueueSendFromISR(s_chunk_queue, chunk, &woken);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Hardware init stubs (Phase B will replace these)
 * ------------------------------------------------------------------------- */
static esp_err_t i2s_init_stub(void)
{
    /* TODO Phase B: configure I2S via i2s_new_channel / i2s_channel_init_std_mode */
    ESP_LOGI(TAG, "I2S init (stub)");
    return ESP_OK;
}

static esp_err_t es8311_init_stub(void)
{
    /* TODO Phase B: ES8311 I2C register config */
    ESP_LOGI(TAG, "ES8311 init (stub)");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Capture / playback tasks
 * ------------------------------------------------------------------------- */
static void audio_capture_task(void *arg)
{
    ESP_LOGI(TAG, "Capture task started (Core %d)", xPortGetCoreID());
    size_t bytes_read = 0;

    while (s_capturing) {
        /* Stub: no real I2S read; just delay to simulate 40 ms chunks. */
        vTaskDelay(pdMS_TO_TICKS(40));
        bytes_read = 0;

        if (bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int16_t);
            if (s_capture_cb) s_capture_cb(s_dma_buffer, samples, s_capture_cb_arg);
            chunk_accumulator_push(s_dma_buffer, samples);
            memcpy(s_audio_buffer + s_buffer_pos, s_dma_buffer,
                   (bytes_read < (AUDIO_BUFFER_SIZE - s_buffer_pos) * sizeof(int16_t))
                   ? bytes_read
                   : (AUDIO_BUFFER_SIZE - s_buffer_pos) * sizeof(int16_t));
            s_buffer_pos = (s_buffer_pos + samples) % AUDIO_BUFFER_SIZE;
        }
    }

    ESP_LOGI(TAG, "Capture task stopped");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
esp_err_t audio_init(void)
{
    if (s_initialized) return ESP_OK;

    s_audio_mutex = xSemaphoreCreateMutex();

    s_chunk_queue = xQueueCreate(8, AUDIO_CHUNK_SIZE_BYTES);
    if (s_chunk_queue) {
        s_chunk_queue_init = true;
        ESP_LOGI(TAG, "Chunk queue created");
    } else {
        ESP_LOGE(TAG, "Chunk queue creation failed");
    }

    ESP_ERROR_CHECK(i2s_init_stub());
    ESP_ERROR_CHECK(es8311_init_stub());

    s_initialized = true;
    ESP_LOGI(TAG, "Audio pipeline ready");
    return ESP_OK;
}

esp_err_t audio_start_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (s_capturing) { xSemaphoreGive(s_audio_mutex); return ESP_OK; }

    s_chunk_accum_count = 0;
    if (s_chunk_queue) {
        int16_t dummy[AUDIO_CHUNK_SAMPLES];
        while (xQueueReceive(s_chunk_queue, dummy, 0) == pdTRUE) {}
    }

    s_capturing  = true;
    s_buffer_pos = 0;
    xTaskCreate(audio_capture_task, "audio_cap", 4096, NULL, 10, NULL);
    xSemaphoreGive(s_audio_mutex);

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_stop_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

    if (s_capturing && s_chunk_accum_count > 0) {
        while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES)
            s_chunk_accum[s_chunk_accum_count++] = 0;
        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        memcpy(chunk, s_chunk_accum, sizeof(chunk));
        s_chunk_accum_count = 0;
        BaseType_t woken = pdFALSE;
        if (s_chunk_queue) {
            if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
                int16_t dummy[AUDIO_CHUNK_SAMPLES];
                xQueueReceiveFromISR(s_chunk_queue, dummy, &woken);
                xQueueSendFromISR(s_chunk_queue, chunk, &woken);
            }
        }
    }

    s_capturing = false;
    xSemaphoreGive(s_audio_mutex);
    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

bool audio_is_capturing(void) { return s_capturing; }

esp_err_t audio_play_tts(const char *text)
{
    ESP_LOGI(TAG, "TTS play (stub): %s", text);
    s_playing = true;
    vTaskDelay(pdMS_TO_TICKS(500));
    s_playing = false;
    return ESP_OK;
}

esp_err_t audio_play_beep(audio_beep_type_t type)
{
    ESP_LOGI(TAG, "Beep (stub): %d", type);
    return ESP_OK;
}

esp_err_t audio_stop_playback(void)  { s_playing = false; return ESP_OK; }
bool      audio_is_playing(void)     { return s_playing; }

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    return ESP_OK;
}

int audio_get_volume(void) { return s_volume; }

void audio_register_capture_callback(audio_capture_cb_t cb, void *arg)
{
    s_capture_cb     = cb;
    s_capture_cb_arg = arg;
}

bool audio_manager_read_chunk(int16_t *buf, size_t *count, int timeout_ms)
{
    if (!buf || !count) return false;
    *count = 0;
    if (!s_capturing || !s_chunk_queue_init) return false;
    if (xQueueReceive(s_chunk_queue, buf, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *count = AUDIO_CHUNK_SAMPLES;
        return true;
    }
    return false;
}

esp_err_t audio_manager_send_interrupt_before_capture(void)
{
    return ws_client_send_interrupt();
}

void audio_manager_flush_accumulator(void)
{
    if (!s_chunk_queue_init || s_chunk_accum_count == 0) return;
    while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES)
        s_chunk_accum[s_chunk_accum_count++] = 0;
    int16_t chunk[AUDIO_CHUNK_SAMPLES];
    memcpy(chunk, s_chunk_accum, sizeof(chunk));
    s_chunk_accum_count = 0;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
        int16_t dummy[AUDIO_CHUNK_SAMPLES];
        xQueueReceiveFromISR(s_chunk_queue, dummy, &woken);
        xQueueSendFromISR(s_chunk_queue, chunk, &woken);
    }
}
