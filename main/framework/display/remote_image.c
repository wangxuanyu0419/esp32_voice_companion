#include "remote_image.h"
#include "scene_avatar.h"
#include "config_store.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "REMOTE_IMAGE";

#define IMAGE_QUEUE_DEPTH 3
#define IMAGE_MAX_BYTES (512U * 1024U)

typedef struct {
    char *url;
    char *caption;
} image_request_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool rejected_type;
    bool rejected_size;
} fetch_ctx_t;

static QueueHandle_t s_queue;
static app_config_t s_cfg_snapshot;

static bool extract_authority(const char *url, const char *scheme,
                              char *out, size_t out_size)
{
    size_t scheme_len = strlen(scheme);
    if (!url || !scheme || strncasecmp(url, scheme, scheme_len) != 0) return false;
    const char *start = url + scheme_len;
    const char *end = strpbrk(start, "/?#");
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == 0 || len >= out_size || memchr(start, '@', len)) return false;
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)tolower((unsigned char)start[i]);
    }
    out[len] = '\0';
    return true;
}

static bool same_server_https_url(const char *url)
{
    app_config_t cfg = {0};
    config_get(&cfg);
    char url_host[128], server_host[128];
    return extract_authority(url, "https://", url_host, sizeof(url_host)) &&
           extract_authority(cfg.server_url, "wss://", server_host, sizeof(server_host)) &&
           strcmp(url_host, server_host) == 0;
}

static esp_err_t image_http_cb(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = evt ? (fetch_ctx_t *)evt->user_data : NULL;
    if (!ctx) return ESP_FAIL;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            if (!strstr(evt->header_value, "image/jpeg") &&
                !strstr(evt->header_value, "image/png")) {
                ctx->rejected_type = true;
            }
        } else if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            char *end = NULL;
            unsigned long n = strtoul(evt->header_value, &end, 10);
            if (end == evt->header_value || n > IMAGE_MAX_BYTES) {
                ctx->rejected_size = true;
            }
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (ctx->rejected_type || ctx->rejected_size ||
            ctx->len > IMAGE_MAX_BYTES - (size_t)evt->data_len) return ESP_FAIL;
        size_t needed = ctx->len + (size_t)evt->data_len;
        if (needed > ctx->cap) {
            size_t next = ctx->cap ? ctx->cap * 2 : 8192;
            while (next < needed && next < IMAGE_MAX_BYTES) next *= 2;
            if (next > IMAGE_MAX_BYTES) next = IMAGE_MAX_BYTES;
            uint8_t *grown = heap_caps_realloc(ctx->data, next,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) grown = realloc(ctx->data, next);
            if (!grown) return ESP_ERR_NO_MEM;
            ctx->data = grown;
            ctx->cap = next;
        }
        memcpy(ctx->data + ctx->len, evt->data, (size_t)evt->data_len);
        ctx->len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

static void add_same_origin_auth_headers(esp_http_client_handle_t client)
{
    config_get(&s_cfg_snapshot);
    if (*s_cfg_snapshot.ws_auth_token) {
        char bearer[160];
        snprintf(bearer, sizeof(bearer), "Bearer %s", s_cfg_snapshot.ws_auth_token);
        esp_http_client_set_header(client, "Authorization", bearer);
    }
    if (*s_cfg_snapshot.cf_client_id) {
        esp_http_client_set_header(client, "CF-Access-Client-Id",
                                   s_cfg_snapshot.cf_client_id);
    }
    if (*s_cfg_snapshot.cf_client_secret) {
        esp_http_client_set_header(client, "CF-Access-Client-Secret",
                                   s_cfg_snapshot.cf_client_secret);
    }
}

static void fetch_task(void *arg)
{
    (void)arg;
    image_request_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        fetch_ctx_t ctx = {0};
        esp_http_client_config_t cfg = {
            .url = req.url,
            .event_handler = image_http_cb,
            .user_data = &ctx,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000,
            .buffer_size = 4096,
            /* Bearer + Cloudflare Access headers can exceed IDF's 512-byte
             * default TX buffer; keep image requests bounded like audio. */
            .buffer_size_tx = 2048,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = ESP_FAIL;
        int status = 0;
        if (client) {
            add_same_origin_auth_headers(client);
            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
        }

        if (err == ESP_OK && status >= 200 && status < 300 && ctx.len > 0 &&
            !ctx.rejected_type && !ctx.rejected_size) {
            avatar_show_image_owned(ctx.data, ctx.len, req.caption);
            ctx.data = NULL;
        } else {
            ESP_LOGW(TAG, "Image fetch failed err=%s status=%d bytes=%u",
                     esp_err_to_name(err), status, (unsigned)ctx.len);
            avatar_show_assistant_text("Image download failed");
        }
        free(ctx.data);
        free(req.url);
        free(req.caption);
    }
}

esp_err_t remote_image_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(IMAGE_QUEUE_DEPTH, sizeof(image_request_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(fetch_task, "remote_image", 7168, NULL, 4,
                                NULL, 1) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t remote_image_fetch(const char *url, const char *caption)
{
    if (!s_queue || !url || !same_server_https_url(url)) {
        ESP_LOGW(TAG, "Rejected non-HTTPS or cross-origin image URL");
        return ESP_ERR_INVALID_ARG;
    }
    image_request_t req = {
        .url = strdup(url),
        .caption = strdup(caption ? caption : ""),
    };
    if (!req.url || !req.caption) {
        free(req.url);
        free(req.caption);
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(req.url);
        free(req.caption);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
