/*
 * tts_player.c — Streaming TTS via the ClawChat /stream?text=... endpoint.
 *
 * Flow:
 *   1. chat_app calls tts_player_play(text)
 *   2. Text is queued to the player task
 *   3. Player task makes HTTPS GET to https://clawchat.xuanyu.uk/stream?text=<encoded>
 *   4. Response body (MP3) is consumed (stub; decoded+played in Phase B when I2S confirmed)
 *   5. On completion (or stop), fires on_complete callback → chat_app sends voice_turn_complete
 */

#include "tts_player.h"
#include "config_store.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "TTS_PLAYER";

#define TTS_BASE_URL    "https://clawchat.xuanyu.uk/stream?text="
#define TTS_TEXT_MAX    512
#define TTS_BASE_MAX    128
#define TTS_URL_MAX     (TTS_BASE_MAX + TTS_TEXT_MAX * 3 + 1)
#define TTS_QUEUE_DEPTH 4

/* Player state */
static QueueHandle_t      s_queue         = NULL;
static volatile bool      s_playing       = false;
static volatile bool      s_stop_request  = false;
static bool               s_initialized   = false;
static TaskHandle_t       s_task_handle   = NULL;

static tts_on_complete_cb_t s_complete_cb     = NULL;
static void                *s_complete_cb_arg = NULL;

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

/* ── HTTP event handler ───────────────────────────────────────────────────── */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            /* TODO Phase B: pipe evt->data (MP3 bytes) into libhelix decoder,
             * write decoded PCM16 to I2S TX channel.
             * For now, data is silently consumed — timing is preserved because
             * we still block until the HTTP body is fully received. */
            if (s_stop_request) {
                /* Signal esp_http_client to abort by returning an error */
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

/* ── Player task ──────────────────────────────────────────────────────────── */
static void tts_player_task(void *arg)
{
    char text_buf[TTS_TEXT_MAX + 1];

    ESP_LOGI(TAG, "TTS player task started");

    while (1) {
        /* Wait for text to play */
        if (xQueueReceive(s_queue, text_buf, portMAX_DELAY) != pdTRUE) continue;

        if (s_stop_request) {
            s_stop_request = false;
            continue;
        }

        /* Build URL */
        char encoded[TTS_TEXT_MAX * 3 + 1];
        url_encode(text_buf, encoded, sizeof(encoded));

        /* TTS base URL is derived from the configured server host so the
         * dropdown in Settings switches TTS too. Falls back to compile-time. */
        char base[TTS_BASE_MAX];
        if (config_get_tts_base_url(base, sizeof(base)) < 0) {
            strlcpy(base, TTS_BASE_URL, sizeof(base));
        }

        char url[TTS_URL_MAX];
        snprintf(url, sizeof(url), "%s%s", base, encoded);
        ESP_LOGI(TAG, "TTS fetch: %.80s...", url);

        s_playing      = true;
        s_stop_request = false;

        esp_http_client_config_t http_cfg = {
            .url                = url,
            .event_handler      = http_event_cb,
            .crt_bundle_attach  = esp_crt_bundle_attach,
            .timeout_ms         = 30000,
            .buffer_size        = 4096,
            .buffer_size_tx     = 512,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "HTTP client init failed");
            s_playing = false;
            if (s_complete_cb) s_complete_cb(s_complete_cb_arg);
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status    = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TTS stream done (HTTP %d)", status);
        } else {
            ESP_LOGW(TAG, "TTS stream error: %s (HTTP %d)",
                     esp_err_to_name(err), status);
        }

        s_playing      = false;
        s_stop_request = false;

        /* Notify caller (chat_app will send voice_turn_complete) */
        if (s_complete_cb) s_complete_cb(s_complete_cb_arg);
    }

    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t tts_player_init(void)
{
    if (s_initialized) return ESP_OK;

    s_queue = xQueueCreate(TTS_QUEUE_DEPTH, TTS_TEXT_MAX + 1);
    if (!s_queue) {
        ESP_LOGE(TAG, "Queue creation failed");
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
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TTS player ready");
    return ESP_OK;
}

esp_err_t tts_player_play(const char *text)
{
    if (!s_initialized || !text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;

    char buf[TTS_TEXT_MAX + 1];
    strlcpy(buf, text, sizeof(buf));

    if (xQueueSend(s_queue, buf, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full — dropping text");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void tts_player_stop(void)
{
    s_stop_request = true;
    /* Drain queue so pending items don't play after stop */
    if (s_queue) {
        char dummy[TTS_TEXT_MAX + 1];
        while (xQueueReceive(s_queue, dummy, 0) == pdTRUE) {}
    }
}

bool tts_player_is_playing(void) { return s_playing; }

void tts_player_register_complete_callback(tts_on_complete_cb_t cb, void *arg)
{
    s_complete_cb     = cb;
    s_complete_cb_arg = arg;
}
