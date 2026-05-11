/*
 * openclaw_bridge.c — OpenClaw Bridge, Phase 1: Text Control Path Only
 *
 * 目的: 复用 xiaozhi 底层初始化，接入 OpenClaw server
 * 阶段目标: Wi-Fi / WSS 连通 → JSON 收发 → 屏幕显示 → 日志输出
 *
 * 约束:
 *   - 纯 C (no C++ exceptions)
 *   - static 静态对象，无大堆分配
 *   - 仅在 bridge task 调用，非 ISR
 *   - 不修改 xiaozhi 已有流程
 */

#include "openclaw_bridge.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <lwip/netdb.h>

#include "openclaw_ws_client.h"
#include "openclaw_display.h"
#include "openclaw_bridge_local.h"
#include "cJSON.h"

static const char* TAG = "OpenClawBridge";

/* ── Bridge task ── */
static TaskHandle_t s_bridge_task_handle = NULL;
static volatile bool s_bridge_running = false;

/* ── Current connection state ── */
static volatile int s_status = 0;  /* 0=idle, 1=connecting, 2=connected */

int openclaw_bridge_status(void) {
    return s_status;
}

/* ── Helpers ── */

static void IRAM_ATTR get_device_id(char* buf, size_t buflen) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, buflen, "esp32s3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── Build ClawChat JSON hello message ── */
static size_t build_hello(char* out, size_t max_len) {
    char device_id[32];
    get_device_id(device_id, sizeof(device_id));

    /* 单行 JSON，避免 snprintf 截断问题 */
    return snprintf(out, max_len,
        "{\"type\":\"hello\",\"device_id\":\"%s\",\"agent\":\"ella\"}",
        device_id);
}

/* ── Parse server JSON message ── */
static void parse_and_display(const char* json_str) {
    /* cJSON 全局解析（线程安全 — 单任务） */
    cJSON* root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse error: %.*s",
                 json_str ? (int)strlen(json_str) : 0,
                 json_str ? json_str : "(null)");
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        ESP_LOGW(TAG, "Missing type field");
        cJSON_Delete(root);
        return;
    }

    const char* msg_type = type->valuestring;
    ESP_LOGI(TAG, ">>> server msg: type=%s", msg_type);

    /* ── status ── */
    if (strcmp(msg_type, "status") == 0) {
        cJSON* state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsObject(state)) {
            cJSON* thinking = cJSON_GetObjectItem(state, "thinking");
            cJSON* speaking = cJSON_GetObjectItem(state, "speaking");
            cJSON* listening = cJSON_GetObjectItem(state, "listening");

            if (cJSON_IsTrue(thinking)) {
                openclaw_display_set_state("thinking");
            } else if (cJSON_IsTrue(speaking)) {
                openclaw_display_set_state("speaking");
            } else if (cJSON_IsTrue(listening)) {
                openclaw_display_set_state("listening");
            } else {
                openclaw_display_set_state("idle");
            }
        }
    }
    /* ── assistant_text ── */
    else if (strcmp(msg_type, "assistant_text") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            openclaw_display_show_text(text->valuestring);
            ESP_LOGI(TAG, " assistant_text: %s", text->valuestring);
        }
    }
    /* ── tts_text ── */
    else if (strcmp(msg_type, "tts_text") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            openclaw_display_set_state("speaking");
            ESP_LOGI(TAG, " tts_text: %s", text->valuestring);
        }
    }
    /* ── live2d / emotion ── */
    else if (strcmp(msg_type, "live2d") == 0) {
        cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion) && emotion->valuestring) {
            openclaw_display_set_emotion(emotion->valuestring);
            ESP_LOGI(TAG, " live2d emotion: %s", emotion->valuestring);
        }
    }
    /* ── error ── */
    else if (strcmp(msg_type, "error") == 0) {
        cJSON* msg = cJSON_GetObjectItem(root, "message");
        const char* err_str = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        openclaw_display_show_text(err_str);
        ESP_LOGE(TAG, " server error: %s", err_str);
    }
    /* ── ready / pong / other: ignore ── */
    else {
        ESP_LOGD(TAG, " unhandled msg type: %s", msg_type);
    }

    cJSON_Delete(root);
}

/* ── Bridge main loop ── */
static void bridge_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "Bridge task started");

    char rx_buf[4096];
    int rx_len = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 3;

    while (s_bridge_running) {
        s_status = 1;  /* connecting */

        /* 状态显示 */
        openclaw_display_set_state("connecting");

        ESP_LOGI(TAG, "Connecting to wss://%s/ws ...", CLAWCHAT_WS_HOST);
        int fd = openclaw_ws_connect(CLAWCHAT_WS_HOST, CLAWCHAT_WS_PORT, CLAWCHAT_WS_SECURE);
        if (fd < 0) {
            ESP_LOGE(TAG, "TCP connect failed, retry in 5s");
            s_status = 0;
            openclaw_display_set_state("idle");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* WebSocket handshake */
        char hello_buf[256];
        size_t hello_len = build_hello(hello_buf, sizeof(hello_buf));

        /* Build and send WS upgrade + hello together */
        char ws_req[512];
        int req_len = snprintf(ws_req, sizeof(ws_req),
            "GET /ws HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Content-Type: text/plain\r\n"
            "X-Device-Id: esp32s3-bridge\r\n"
            "Authorization: Bearer %s\r\n"
            "\r\n"
            "%.*s",
            CLAWCHAT_WS_HOST, CLAWCHAT_WS_PORT, CLAWCHAT_APP_TOKEN,
            (int)hello_len, hello_buf);

        int sent = openclaw_ws_write(fd, ws_req, req_len);
        if (sent < 0) {
            ESP_LOGE(TAG, "send failed");
            openclaw_ws_close(fd);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* Read until we get "ready" or timeout */
        bool got_ready = false;
        int to_start = 5000;  /* ms */
        int64_t deadline = esp_timer_get_time() / 1000 + to_start;

        while (s_bridge_running && !got_ready) {
            int remain = (int)(deadline - (esp_timer_get_time() / 1000));
            if (remain <= 0) break;

            struct timeval tv = { .tv_sec = remain / 1000, .tv_usec = (remain % 1000) * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char buf[2048];
            int n = openclaw_ws_read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';

                /* WebSocket frame? Decode inline (opcode + mask) */
                if ((unsigned char)buf[0] == 0x81) {
                    int payload_len = buf[1] & 0x7F;
                    int hdr_len = 2;
                    if (payload_len == 126) {
                        payload_len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
                        hdr_len = 4;
                    } else if (payload_len == 127) {
                        hdr_len = 10;
                        payload_len = 0;
                    }
                    if (n >= hdr_len + payload_len) {
                        char* data = &buf[hdr_len];
                        data[payload_len] = '\0';

                        /* 找 ready */
                        if (strstr(data, "\"ready\"") != NULL) {
                            ESP_LOGI(TAG, "Server ready!");
                            got_ready = true;
                            retry_count = 0;
                        } else {
                            /* 其他 JSON */
                            parse_and_display(data);
                        }
                    }
                }
                /* HTTP response (not WS framed yet) */
                else if (strstr(buf, "{\"type") != NULL) {
                    char* start = strstr(buf, "{\"type");
                    char* end = strstr(buf, "}\"");
                    if (start && end) {
                        end[1] = '\0';
                        parse_and_display(start);
                    }
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;  /* closed or error */
            }
        }

        openclaw_ws_close(fd);

        if (!got_ready) {
            ESP_LOGW(TAG, "No ready, retrying...");
            retry_count++;
            if (retry_count >= MAX_RETRIES) {
                openclaw_display_show_text("Server unreachable");
                retry_count = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* ── Connected: main receive loop ── */
        s_status = 2;  /* connected */
        openclaw_display_set_state("idle");
        ESP_LOGI(TAG, "Bridge connected!");

        while (s_bridge_running) {
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char buf[4096];
            int n = openclaw_ws_read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';

                /* WS frame */
                if ((unsigned char)buf[0] == 0x81) {
                    int payload_len = buf[1] & 0x7F;
                    int hdr_len = 2;
                    if (payload_len == 126) {
                        payload_len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
                        hdr_len = 4;
                    }
                    if (n >= hdr_len + payload_len) {
                        char* data = &buf[hdr_len];
                        data[payload_len] = '\0';
                        parse_and_display(data);
                    }
                }
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* keep-alive ping — OK */
                continue;
            } else {
                break;  /* disconnect */
            }
        }

        /* Disconnected */
        s_status = 0;
        ESP_LOGW(TAG, "Connection lost, reconnecting...");
        openclaw_display_set_state("idle");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    s_status = 0;
    ESP_LOGI(TAG, "Bridge task exiting");
    s_bridge_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ── */

void openclaw_bridge_start(void) {
    if (s_bridge_task_handle != NULL) {
        ESP_LOGW(TAG, "Already running");
        return;
    }

    s_bridge_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        &bridge_task,
        "OpenClawBridge",
        8192,          /* stack size */
        NULL,          /* param */
        5,             /* priority */
        &s_bridge_task_handle,
        1              /* core 1 (app_main runs on core 1) */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        s_bridge_running = false;
    } else {
        ESP_LOGI(TAG, "Bridge task created on core 1");
    }
}

void openclaw_bridge_stop(void) {
    if (!s_bridge_running) return;
    s_bridge_running = false;
    if (s_bridge_task_handle != NULL) {
        vTaskDelete(s_bridge_task_handle);
        s_bridge_task_handle = NULL;
    }
}
