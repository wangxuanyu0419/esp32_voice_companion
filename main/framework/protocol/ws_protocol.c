/*
 * ws_protocol.c — WebSocket transport + ClawChat protocol (merged).
 *
 * Consolidates the original 02_websocket/ws_client.c and
 * 02_websocket/ws_protocol.c into a single compilation unit that:
 *   1. Wraps esp_websocket_client for transport.
 *   2. Parses incoming JSON (tts_text, live2d, status, assistant_text, …).
 *   3. Implements protocol_t (Phase-A stub; full impl in Phase-B).
 *   4. Keeps the legacy ws_client_* API so existing call-sites compile.
 */

#include "ws_protocol.h"
#include "config_store.h"
#include "app_state.h"
#include "event_bus.h"
#include "esp_crt_bundle.h"
#include "emotion_map.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <mbedtls/base64.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "WS_PROTOCOL";

/* =========================================================================
 * Transport state
 * ========================================================================= */

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool                          s_connected  = false;
static bool                          s_ready      = false;
static esp_timer_handle_t            s_ping_timer = NULL;
static const int                     PING_INTERVAL_MS = 30000;

static ws_message_cb_t s_msg_cb     = NULL;
static void           *s_msg_cb_arg = NULL;

static char s_json_buf[1024];

/* =========================================================================
 * Protocol callbacks (inbound)
 * ========================================================================= */

static ws_on_tts_cb_t    s_tts_cb    = NULL;
static ws_on_live2d_cb_t s_live2d_cb = NULL;
static ws_on_status_cb_t s_status_cb = NULL;
static ws_on_text_cb_t   s_text_cb   = NULL;
static void             *s_cb_arg    = NULL;

/* Voice-flow callbacks */
static ws_on_voice_ready_cb_t s_voice_ready_cb = NULL;
static ws_on_stt_final_cb_t   s_stt_final_cb   = NULL;
static ws_on_turn_state_cb_t  s_turn_state_cb  = NULL;
static void                  *s_voice_cb_arg   = NULL;

/* Extended protocol events. Keep these separate from the legacy voice
 * callbacks so the stable B UI/audio path remains untouched. */
static ws_extended_callbacks_t s_ext_cb = {0};
static void                   *s_ext_cb_arg = NULL;

/* esp_websocket_client may split one text message across several data events.
 * Keep a bounded accumulator so the protocol parser never receives partial
 * JSON. Binary frames are forwarded directly to the PCM queue. */
#define WS_MAX_JSON_PAYLOAD (64 * 1024)
static uint8_t *s_rx_text_buf = NULL;
static size_t   s_rx_text_cap = 0;
static uint8_t  s_rx_opcode   = 0;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void send_json(const char *json_str)
{
    if (s_connected && s_ws_client) {
        esp_websocket_client_send_text(s_ws_client, json_str,
                                       strlen(json_str), portMAX_DELAY);
    }
}

static int base64_encode_buf(const uint8_t *src, size_t src_len,
                              char *dst, size_t dst_len)
{
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)dst, dst_len, &olen,
                                    src, src_len);
    return (ret == 0) ? (int)olen : -1;
}

static void reset_text_accumulator(void)
{
    free(s_rx_text_buf);
    s_rx_text_buf = NULL;
    s_rx_text_cap = 0;
    s_rx_opcode = 0;
}

static void handle_text_data(const esp_websocket_event_data_t *data)
{
    if (!data || !data->data_ptr || data->data_len <= 0) return;

    size_t total = data->payload_len > 0 ? (size_t)data->payload_len
                                         : (size_t)data->data_len;
    size_t offset = data->payload_offset > 0 ? (size_t)data->payload_offset : 0;
    if (total == 0 || total > WS_MAX_JSON_PAYLOAD ||
        offset > total || (size_t)data->data_len > total - offset) {
        ESP_LOGW(TAG, "Dropping invalid WS text payload total=%u offset=%u len=%d",
                 (unsigned)total, (unsigned)offset, data->data_len);
        reset_text_accumulator();
        return;
    }

    if (offset == 0) {
        reset_text_accumulator();
        s_rx_text_buf = heap_caps_malloc(total + 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rx_text_buf) s_rx_text_buf = malloc(total + 1);
        if (!s_rx_text_buf) {
            ESP_LOGE(TAG, "No memory for %u-byte WS JSON", (unsigned)total);
            return;
        }
        s_rx_text_cap = total;
    }
    if (!s_rx_text_buf || s_rx_text_cap != total) return;

    memcpy(s_rx_text_buf + offset, data->data_ptr, (size_t)data->data_len);
    if (offset + (size_t)data->data_len >= total && data->fin) {
        s_rx_text_buf[total] = '\0';
        if (s_msg_cb) s_msg_cb("json", (const char *)s_rx_text_buf, s_msg_cb_arg);
        ws_protocol_parse((const char *)s_rx_text_buf);
        reset_text_accumulator();
    }
}

/* =========================================================================
 * WebSocket event handler
 * ========================================================================= */

static void ws_event_handler(void *handler_args, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "WebSocket connected");
            s_connected = true;
            s_ready = false;
            esp_timer_start_periodic(s_ping_timer,
                                     (uint64_t)PING_INTERVAL_MS * 1000);
            event_t ev = { .type = EVT_WS_CONNECTED };
            event_bus_post(&ev);
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED: {
            ESP_LOGI(TAG, "WebSocket disconnected");
            s_connected = false;
            s_ready = false;
            reset_text_accumulator();
            esp_timer_stop(s_ping_timer);
            event_t ev2 = { .type = EVT_WS_DISCONNECTED };
            event_bus_post(&ev2);
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (data && data->data_len > 0) {
                if (data->payload_offset == 0 && data->op_code != 0) {
                    s_rx_opcode = data->op_code;
                }
                uint8_t opcode = data->op_code == 0 ? s_rx_opcode : data->op_code;
                if (opcode == 0x02) {
                    /* data_ptr is valid only during this callback. The PCM
                     * callback copies/enqueues synchronously. */
                    if (s_ext_cb.on_tts_audio_chunk) {
                        s_ext_cb.on_tts_audio_chunk((const uint8_t *)data->data_ptr,
                                                    (size_t)data->data_len,
                                                    s_ext_cb_arg);
                    }
                } else if (opcode == 0x01) {
                    handle_text_data(data);
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

static void ping_timer_cb(void *arg)
{
    if (s_connected) ws_client_send_ping();
}

/* =========================================================================
 * JSON parsing (inbound)
 * ========================================================================= */

static void parse_tts_text(const cJSON *json)
{
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring && s_tts_cb) {
        s_tts_cb(text->valuestring, s_cb_arg);
    }
}

static void parse_live2d(const cJSON *json)
{
    const cJSON *action     = cJSON_GetObjectItem(json, "action");
    const cJSON *name       = cJSON_GetObjectItem(json, "name");
    const cJSON *expression = cJSON_GetObjectItem(json, "expression");
    const cJSON *dur        = cJSON_GetObjectItem(json, "duration_ms");
    const cJSON *ret_to     = cJSON_GetObjectItem(json, "return_to");

    if (!name && !expression) return;

    const char *action_str  = action     ? action->valuestring     : "";
    const char *name_str    = name       ? name->valuestring
                                         : (expression ? expression->valuestring : "");
    const char *ret_str     = ret_to     ? ret_to->valuestring     : "idle";
    int         duration    = dur        ? dur->valueint            : 0;

    ws_live2d_event_t event = {
        .action      = action_str,
        .name        = name_str,
        .duration_ms = duration,
        .return_to   = ret_str,
    };

    if (s_live2d_cb) s_live2d_cb(&event, s_cb_arg);

    if (strcmp(action_str, "emotion") == 0 && strlen(name_str) > 0) {
        /* Resolve emotion for logging only; avatar update is via callback. */
        avatar_emotion_t emo = emotion_id_to_avatar(name_str);
        ESP_LOGI(TAG, "Emotion: %s → avatar=%d", name_str, emo);
    }
}

static void parse_status(const cJSON *json)
{
    const cJSON *state = cJSON_GetObjectItem(json, "state");
    if (!state) return;

    bool listening = cJSON_IsTrue(cJSON_GetObjectItem(state, "listening"));
    bool thinking  = cJSON_IsTrue(cJSON_GetObjectItem(state, "thinking"));
    bool speaking  = cJSON_IsTrue(cJSON_GetObjectItem(state, "speaking"));

    ws_status_event_t ev = {
        .listening = listening,
        .thinking  = thinking,
        .speaking  = speaking,
    };
    if (s_status_cb) s_status_cb(&ev, s_cb_arg);

    /* Sync app state directly as well. */
    if (thinking)       app_state_set_current(APP_STATE_THINKING);
    else if (speaking)  app_state_set_current(APP_STATE_SPEAKING);
    else if (listening) app_state_set_current(APP_STATE_LISTENING);
}

static void parse_assistant_text(const cJSON *json)
{
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring && s_text_cb) {
        s_text_cb(text->valuestring, s_cb_arg);
    }
}

static void parse_stt_final(const cJSON *json)
{
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring && s_stt_final_cb) {
        s_stt_final_cb(text->valuestring, s_voice_cb_arg);
    }
}

static void parse_voice_turn_state(const cJSON *json)
{
    const cJSON *state = cJSON_GetObjectItem(json, "state");
    if (!state || !state->valuestring) return;

    const char *s = state->valuestring;
    ESP_LOGI(TAG, "voice_turn_state: %s", s);

    /* Delegate app-state changes to the chat_app callback so the chat-state
     * machine and the display stay in sync. */
    if (s_turn_state_cb) s_turn_state_cb(s, s_voice_cb_arg);
}

static void parse_tts_sentence(const cJSON *json)
{
    /* Treat like tts_text — call the TTS callback with the sentence text */
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring && s_tts_cb) {
        s_tts_cb(text->valuestring, s_cb_arg);
    }
}

static const char *json_string(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static void parse_broadcast(const cJSON *json)
{
    const char *text = json_string(json, "content");
    if (s_ext_cb.on_broadcast && *text) {
        ESP_LOGI(TAG, "Broadcast received (%u chars)", (unsigned)strlen(text));
        s_ext_cb.on_broadcast(text, s_ext_cb_arg);
    }
    const char *url = json_string(json, "image_url");
    if (!*url) url = json_string(json, "url");
    if (s_ext_cb.on_image && *url) s_ext_cb.on_image(url, text, s_ext_cb_arg);
}

static void parse_agent_ack(const cJSON *json)
{
    const char *text = json_string(json, "text");
    if (s_ext_cb.on_agent_ack && *text) {
        s_ext_cb.on_agent_ack(text, s_ext_cb_arg);
    }
}

static void parse_audio_broadcast(const cJSON *json)
{
    const char *stream_id = json_string(json, "stream_id");
    const char *message = json_string(json, "message");
    if (s_ext_cb.on_audio_broadcast && *stream_id) {
        ESP_LOGI(TAG, "Audio broadcast received: stream_id=%s", stream_id);
        s_ext_cb.on_audio_broadcast(stream_id, message, s_ext_cb_arg);
    }
}

static void parse_tts_audio_start(const cJSON *json)
{
    if (!s_ext_cb.on_tts_audio_start) return;
    const cJSON *rate = cJSON_GetObjectItem(json, "sampleRate");
    const cJSON *channels = cJSON_GetObjectItem(json, "channels");
    s_ext_cb.on_tts_audio_start(json_string(json, "streamId"),
                                cJSON_IsNumber(rate) ? rate->valueint : 24000,
                                cJSON_IsNumber(channels) ? channels->valueint : 1,
                                json_string(json, "encoding"), s_ext_cb_arg);
}

static void parse_tts_audio_end(const cJSON *json)
{
    if (s_ext_cb.on_tts_audio_end) {
        s_ext_cb.on_tts_audio_end(json_string(json, "streamId"), s_ext_cb_arg);
    }
}

static void parse_image(const cJSON *json)
{
    const char *url = json_string(json, "url");
    if (!*url) url = json_string(json, "image_url");
    const char *caption = json_string(json, "caption");
    if (!*caption) caption = json_string(json, "text");
    if (s_ext_cb.on_image && *url) s_ext_cb.on_image(url, caption, s_ext_cb_arg);
}

void ws_protocol_parse(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed");
        return;
    }

    const cJSON *type_item = cJSON_GetObjectItem(json, "type");
    if (!type_item || !type_item->valuestring) {
        cJSON_Delete(json);
        return;
    }

    const char *t = type_item->valuestring;

    if      (strcmp(t, "tts_text")           == 0) parse_tts_text(json);
    else if (strcmp(t, "tts_sentence")       == 0) parse_tts_sentence(json);
    else if (strcmp(t, "tts_sentence_end")   == 0) { /* all sentences received */ }
    else if (strcmp(t, "live2d")             == 0) parse_live2d(json);
    else if (strcmp(t, "status")             == 0) parse_status(json);
    else if (strcmp(t, "assistant_text")     == 0) parse_assistant_text(json);
    else if (strcmp(t, "stt_final")          == 0) parse_stt_final(json);
    else if (strcmp(t, "voice_turn_state")   == 0) parse_voice_turn_state(json);
    else if (strcmp(t, "broadcast")          == 0) parse_broadcast(json);
    else if (strcmp(t, "agent_ack")          == 0) parse_agent_ack(json);
    else if (strcmp(t, "audio_broadcast")    == 0) parse_audio_broadcast(json);
    else if (strcmp(t, "tts_audio_start")    == 0) parse_tts_audio_start(json);
    else if (strcmp(t, "tts_audio_end")      == 0) parse_tts_audio_end(json);
    else if (strcmp(t, "image")              == 0 ||
             strcmp(t, "image_message")      == 0) parse_image(json);
    else if (strcmp(t, "voice_session_ready")== 0) {
        ESP_LOGI(TAG, "Voice session ready");
        if (s_voice_ready_cb) s_voice_ready_cb(s_voice_cb_arg);
    }
    else if (strcmp(t, "ready")              == 0) {
        ESP_LOGI(TAG, "Server ready");
        s_ready = true;
        app_state_set_current(APP_STATE_IDLE);
    }
    else if (strcmp(t, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(json, "message");
        ESP_LOGE(TAG, "Server error: %s", msg ? msg->valuestring : "?");
        if (s_ext_cb.on_error) {
            s_ext_cb.on_error(msg && msg->valuestring ? msg->valuestring : "Server error",
                              s_ext_cb_arg);
        }
        app_state_set_current(APP_STATE_ERROR);
    }
    else if (strcmp(t, "pong") == 0) { /* heartbeat acknowledgement */ }
    else if (strcmp(t, "ping") == 0) {
        send_json("{\"type\":\"pong\"}");
    }
    else if (strcmp(t, "debug_log") == 0) { /* ignore */ }
    else {
        ESP_LOGW(TAG, "Unknown message type: %s", t);
    }

    cJSON_Delete(json);
}

void ws_protocol_register_callbacks(ws_on_tts_cb_t tts_cb,
                                    ws_on_live2d_cb_t live2d_cb,
                                    ws_on_status_cb_t status_cb,
                                    ws_on_text_cb_t text_cb,
                                    void *arg)
{
    s_tts_cb    = tts_cb;
    s_live2d_cb = live2d_cb;
    s_status_cb = status_cb;
    s_text_cb   = text_cb;
    s_cb_arg    = arg;
}

void ws_protocol_register_voice_callbacks(ws_on_voice_ready_cb_t voice_ready_cb,
                                          ws_on_stt_final_cb_t stt_final_cb,
                                          ws_on_turn_state_cb_t turn_state_cb,
                                          void *arg)
{
    s_voice_ready_cb = voice_ready_cb;
    s_stt_final_cb   = stt_final_cb;
    s_turn_state_cb  = turn_state_cb;
    s_voice_cb_arg   = arg;
}

void ws_protocol_register_extended_callbacks(const ws_extended_callbacks_t *callbacks,
                                              void *arg)
{
    if (callbacks) s_ext_cb = *callbacks;
    else memset(&s_ext_cb, 0, sizeof(s_ext_cb));
    s_ext_cb_arg = arg;
}

int ws_protocol_build_stt_audio(const int16_t *pcm_data, size_t sample_count,
                                bool is_last, char *out_buf, size_t buf_size)
{
    if (!pcm_data || !out_buf || buf_size < 100) return -1;

    size_t pcm_bytes = sample_count * sizeof(int16_t);
    char b64_buf[2048];
    int b64_len = base64_encode_buf((const uint8_t *)pcm_data, pcm_bytes,
                                    b64_buf, sizeof(b64_buf));
    if (b64_len < 0) {
        ESP_LOGE(TAG, "Base64 encode failed");
        return -1;
    }
    b64_buf[b64_len] = '\0';

    int written = snprintf(out_buf, buf_size,
                           "{\"type\":\"stt_audio\",\"audio\":\"%s\",\"isLast\":%s}",
                           b64_buf, is_last ? "true" : "false");
    if (written < 0 || (size_t)written >= buf_size) {
        ESP_LOGE(TAG, "stt_audio JSON overflow");
        return -1;
    }
    return written;
}

/* =========================================================================
 * Legacy ws_client_* API
 * ========================================================================= */

esp_err_t ws_client_init(void)
{
    esp_timer_create_args_t ta = {.callback = ping_timer_cb, .name = "ws_ping"};
    return esp_timer_create(&ta, &s_ping_timer);
}

esp_err_t ws_client_connect(const char *url)
{
    ESP_LOGI(TAG, "Connecting WS: %s", url);
    s_ready = false;

    if (s_ws_client) {
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    /* Build auth header string from config */
    static char s_headers[600];
    app_config_t cfg = {0};
    config_get(&cfg);
    int hlen = 0;
    if (strlen(cfg.ws_auth_token) > 0)
        hlen += snprintf(s_headers + hlen, sizeof(s_headers) - hlen,
                         "Authorization: Bearer %s\r\n", cfg.ws_auth_token);
    if (strlen(cfg.cf_client_id) > 0)
        hlen += snprintf(s_headers + hlen, sizeof(s_headers) - hlen,
                         "CF-Access-Client-Id: %s\r\n", cfg.cf_client_id);
    if (strlen(cfg.cf_client_secret) > 0)
        hlen += snprintf(s_headers + hlen, sizeof(s_headers) - hlen,
                         "CF-Access-Client-Secret: %s\r\n", cfg.cf_client_secret);

    esp_websocket_client_config_t ws_cfg = {
        .uri                  = url,
        .headers              = hlen > 0 ? s_headers : NULL,
        .transport            = WEBSOCKET_TRANSPORT_OVER_SSL,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms   = 30000,
        .task_stack           = 6144,
        /* Cloudflare sends many response headers (CF-Ray, nel, report-to, etc.)
         * that exceed the default 1024-byte buffer. 4096 is sufficient. */
        .buffer_size          = 4096,
        /* Use the built-in Mozilla CA bundle (includes Cloudflare's CA). */
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .use_global_ca_store         = false,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) return ESP_FAIL;

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    return esp_websocket_client_start(s_ws_client);
}

void ws_client_disconnect(void)
{
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        s_connected = false;
    }
}

bool ws_client_is_connected(void) { return s_connected; }
bool ws_client_is_ready(void) { return s_connected && s_ready; }

esp_err_t ws_client_send_hello(void)
{
    app_config_t cfg;
    config_get(&cfg);
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"hello\",\"device_id\":\"%s\",\"agent\":\"%s\","
             "\"persona\":\"voice\",\"app_version\":3}",
             cfg.device_id, cfg.agent);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_text(const char *text)
{
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"user_text\",\"text\":\"%s\"}", text);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_stt_partial(const char *text)
{
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"stt_partial\",\"text\":\"%s\"}", text);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_stt_final(const char *text)
{
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"stt_final\",\"text\":\"%s\"}", text);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_interrupt(void)
{
    snprintf(s_json_buf, sizeof(s_json_buf), "{\"type\":\"interrupt\"}");
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_ping(void)
{
    snprintf(s_json_buf, sizeof(s_json_buf), "{\"type\":\"ping\"}");
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_stt_audio(const int16_t *pcm_data,
                                   size_t sample_count, bool is_last)
{
    if (!s_connected) return ESP_FAIL;

    char json_buf[2048];
    int len = ws_protocol_build_stt_audio(pcm_data, sample_count, is_last,
                                          json_buf, sizeof(json_buf));
    if (len < 0) return ESP_FAIL;

    int ret = esp_websocket_client_send_text(s_ws_client, json_buf, len,
                                             pdMS_TO_TICKS(100));
    return (ret < 0) ? ESP_FAIL : ESP_OK;
}

void ws_register_message_callback(ws_message_cb_t cb, void *arg)
{
    s_msg_cb     = cb;
    s_msg_cb_arg = arg;
}

/* =========================================================================
 * Voice session + binary audio (ClawChat protocol)
 * ========================================================================= */

esp_err_t ws_client_send_voice_session_start(void)
{
    app_config_t cfg;
    config_get(&cfg);
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"voice_session_start\",\"device_id\":\"%s\",\"agent\":\"%s\"}",
             cfg.device_id, cfg.agent);
    send_json(s_json_buf);
    ESP_LOGI(TAG, "voice_session_start sent");
    return ESP_OK;
}

esp_err_t ws_client_send_voice_session_stop(void)
{
    app_config_t cfg;
    config_get(&cfg);
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"voice_session_stop\",\"device_id\":\"%s\"}",
             cfg.device_id);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_voice_session_idle(const char *reason)
{
    app_config_t cfg;
    config_get(&cfg);
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"voice_session_idle\",\"device_id\":\"%s\",\"reason\":\"%s\"}",
             cfg.device_id, reason ? reason : "idle");
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_voice_turn_complete(void)
{
    app_config_t cfg;
    config_get(&cfg);
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"voice_turn_complete\",\"device_id\":\"%s\"}",
             cfg.device_id);
    send_json(s_json_buf);
    ESP_LOGI(TAG, "voice_turn_complete sent");
    return ESP_OK;
}

/* 32-byte audio frame header (little-endian integers, packed) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic[4];       /* 0xCC 0x56 0x43 0x01 */
    uint32_t chunk_idx;      /* LE */
    uint32_t sample_rate;    /* LE, 16000 */
    uint8_t  flags;          /* bit0 = isLast */
    uint8_t  channels;       /* 1 */
    uint8_t  bits_per_sample;/* 16 */
    uint8_t  reserved;       /* 0 */
    uint8_t  turn_id[16];    /* 16-byte opaque ID */
} audio_frame_hdr_t;         /* 32 bytes total */
#pragma pack(pop)

/* Static send buffer: 32-byte header + one 40 ms chunk (640 samples × 2 B).
 * Avoids a malloc/free per ~40 ms frame (25×/s) during recording. */
#define WS_TX_PCM_MAX   1024   /* max samples per chunk (covers 640) */
static uint8_t s_bin_tx_buf[sizeof(audio_frame_hdr_t) + WS_TX_PCM_MAX * sizeof(int16_t)];

esp_err_t ws_client_send_binary_chunk(const int16_t *pcm, size_t sample_count,
                                      uint32_t chunk_idx,
                                      const uint8_t turn_id[16],
                                      bool is_last)
{
    if (!s_connected || !s_ws_client) return ESP_FAIL;
    if (sample_count > WS_TX_PCM_MAX)  return ESP_ERR_INVALID_SIZE;

    size_t pcm_bytes = sample_count * sizeof(int16_t);
    size_t total     = sizeof(audio_frame_hdr_t) + pcm_bytes;

    uint8_t *buf = s_bin_tx_buf;

    audio_frame_hdr_t *hdr = (audio_frame_hdr_t *)buf;
    hdr->magic[0]        = 0xCC;
    hdr->magic[1]        = 0x56;
    hdr->magic[2]        = 0x43;
    hdr->magic[3]        = 0x01;
    hdr->chunk_idx       = chunk_idx;    /* already LE on Xtensa */
    hdr->sample_rate     = 16000;
    hdr->flags           = is_last ? 1 : 0;
    hdr->channels        = 1;
    hdr->bits_per_sample = 16;
    hdr->reserved        = 0;
    memcpy(hdr->turn_id, turn_id, 16);

    if (pcm_bytes > 0 && pcm) {
        memcpy(buf + sizeof(audio_frame_hdr_t), pcm, pcm_bytes);
    }

    int ret = esp_websocket_client_send_bin(s_ws_client, (const char *)buf,
                                            (int)total, pdMS_TO_TICKS(200));

    if (is_last) {
        ESP_LOGI(TAG, "Binary chunk #%" PRIu32 " (isLast, %u bytes PCM)", chunk_idx, (unsigned)pcm_bytes);
    }
    return (ret < 0) ? ESP_FAIL : ESP_OK;
}

/* =========================================================================
 * protocol_t implementation
 * ========================================================================= */

static esp_err_t proto_connect(protocol_t *self, const char *url,
                               const char *token)
{
    (void)self;
    (void)token;
    return ws_client_connect(url);
}

static void proto_disconnect(protocol_t *self) { (void)self; ws_client_disconnect(); }
static bool proto_is_connected(protocol_t *self) { (void)self; return ws_client_is_connected(); }

static esp_err_t proto_send_hello(protocol_t *self, const char *device_id,
                                  const char *agent)
{
    (void)self;
    snprintf(s_json_buf, sizeof(s_json_buf),
             "{\"type\":\"hello\",\"device_id\":\"%s\",\"agent\":\"%s\","
             "\"persona\":\"voice\",\"app_version\":3}",
             device_id, agent);
    send_json(s_json_buf);
    return ESP_OK;
}

static esp_err_t proto_send_audio(protocol_t *self, const int16_t *samples,
                                  size_t count, bool is_last)
{
    (void)self;
    return ws_client_send_stt_audio(samples, count, is_last);
}

static esp_err_t proto_send_text(protocol_t *self, const char *text)
{
    (void)self;
    return ws_client_send_text(text);
}

static esp_err_t proto_send_interrupt(protocol_t *self)
{
    (void)self;
    return ws_client_send_interrupt();
}

protocol_t *protocol_ws_create(void)
{
    protocol_t *p = calloc(1, sizeof(protocol_t));
    if (!p) return NULL;

    p->connect        = proto_connect;
    p->disconnect     = proto_disconnect;
    p->is_connected   = proto_is_connected;
    p->send_hello     = proto_send_hello;
    p->send_audio     = proto_send_audio;
    p->send_text      = proto_send_text;
    p->send_interrupt = proto_send_interrupt;

    return p;
}

void protocol_destroy(protocol_t *proto)
{
    free(proto);
}
