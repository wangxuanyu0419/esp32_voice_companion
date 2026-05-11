/**
 * WebSocket 客户端 - 实现
 */

#include "ws_client.h"
#include "config_store.h"
#include "ws_protocol.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "WS_CLIENT";

// WebSocket 客户端句柄
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_connected = false;

// 心跳定时器
static esp_timer_handle_t s_ping_timer = NULL;
static const int PING_INTERVAL_MS = 30000;

// 消息回调
static ws_message_cb_t s_msg_cb = NULL;
static void* s_msg_cb_arg = NULL;

// JSON 构建辅助
static char s_json_buf[1024];

static void send_json(const char* json_str)
{
    if (s_connected && s_ws_client) {
        esp_websocket_client_send_text(s_ws_client, json_str, strlen(json_str), portMAX_DELAY);
    }
}

static void ws_event_handler(void* handler_args, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            s_connected = true;
            // 启动心跳定时器
            esp_timer_start_periodic(s_ping_timer, PING_INTERVAL_MS * 1000);
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            s_connected = false;
            esp_timer_stop(s_ping_timer);
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                // 解析消息类型
                // 格式: {"type": "xxx", ...}
                char* start = strchr(data->data_ptr, '"');
                if (start) {
                    start++;
                    char* type_end = strchr(start, '"');
                    if (type_end) {
                        size_t type_len = type_end - start;
                        char type[32] = {0};
                        memcpy(type, start, type_len);
                        
                        ESP_LOGI(TAG, "WS message: type=%s, len=%d", type, data->data_len);
                        
                        if (s_msg_cb) {
                            // 提取 data 部分
                            const char* json_start = data->data_ptr;
                            s_msg_cb(type, json_start, s_msg_cb_arg);
                        }
                    }
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

static void ping_timer_callback(void* arg)
{
    if (s_connected) {
        ws_client_send_ping();
    }
}

esp_err_t ws_client_init(void)
{
    ESP_LOGI(TAG, "WS Client initializing...");
    
    // 创建心跳定时器
    esp_timer_create_args_t timer_args = {
        .callback = ping_timer_callback,
        .name = "ws_ping"
    };
    esp_timer_create(&timer_args, &s_ping_timer);
    
    return ESP_OK;
}

esp_err_t ws_client_connect(const char* url)
{
    ESP_LOGI(TAG, "Connecting to: %s", url);
    
    if (s_ws_client) {
        esp_websocket_client_destroy(s_ws_client);
    }
    
    // 获取设备配置
    app_config_t cfg;
    config_get(&cfg);
    
    // WebSocket 配置
    esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 30000,
        .task_stack = 4096,
    };
    
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    
    // 注册事件处理
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    
    // 启动连接
    esp_err_t ret = esp_websocket_client_start(s_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS client start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

void ws_client_disconnect(void)
{
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        s_connected = false;
    }
}

bool ws_client_is_connected(void)
{
    return s_connected;
}

esp_err_t ws_client_send_hello(void)
{
    app_config_t cfg;
    config_get(&cfg);
    
    snprintf(s_json_buf, sizeof(s_json_buf),
        "{\"type\":\"hello\",\"device_id\":\"%s\",\"agent\":\"%s\",\"app_version\":2}",
        cfg.device_id, cfg.agent);
    
    ESP_LOGI(TAG, "Sending hello: %s", s_json_buf);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_text(const char* text)
{
    snprintf(s_json_buf, sizeof(s_json_buf),
        "{\"type\":\"user_text\",\"text\":\"%s\"}", text);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_stt_partial(const char* text)
{
    snprintf(s_json_buf, sizeof(s_json_buf),
        "{\"type\":\"stt_partial\",\"text\":\"%s\"}", text);
    send_json(s_json_buf);
    return ESP_OK;
}

esp_err_t ws_client_send_stt_final(const char* text)
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

esp_err_t ws_client_send_stt_audio(const int16_t* pcm_data, size_t sample_count, bool is_last)
{
    if (!ws_client_is_connected()) {
        return ESP_FAIL;
    }

    // Build stt_audio JSON using ws_protocol helper
    char json_buf[2048];
    int len = ws_protocol_build_stt_audio(pcm_data, sample_count, is_last,
                                           json_buf, sizeof(json_buf));
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to build stt_audio message");
        return ESP_FAIL;
    }

    // Use raw binary send (same as send_json but with explicit length)
    if (s_ws_client) {
        int ret = esp_websocket_client_send_text(s_ws_client, json_buf, len, pdMS_TO_TICKS(100));
        if (ret < 0) {
            ESP_LOGE(TAG, "stt_audio send failed: %d", ret);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "stt_audio sent: %d bytes, isLast=%d", ret, is_last);
    }
    return ESP_OK;
}

void ws_register_message_callback(ws_message_cb_t cb, void* arg)
{
    s_msg_cb = cb;
    s_msg_cb_arg = arg;
}