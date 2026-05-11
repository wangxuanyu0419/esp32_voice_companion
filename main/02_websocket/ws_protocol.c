/**
 * WebSocket 协议解析
 * 
 * 处理 ClawChat 服务器消息：
 * - tts_text: TTS 文本，需要播放
 * - live2d: 虚拟形象指令（含 emotion 事件）
 * - status: 状态更新
 * - assistant_text: 文字回复
 * - ready: 连接就绪
 * - error: 错误
 * 
 * emotion 事件格式：
 * {"type": "live2d", "action": "emotion", "name": "happy_l2", "duration_ms": 2500}
 */

#include "ws_protocol.h"
#include "esp_log.h"
#include "app_state.h"
#include <mbedtls/base64.h>

// Base64 encode helper
static int base64_encode(const uint8_t* src, size_t src_len, char* dst, size_t dst_len)
{
    size_t olen;
    int ret = mbedtls_base64_encode((unsigned char*)dst, dst_len, &olen,
                                    src, src_len);
    if (ret != 0) {
        return -1;
    }
    return (int)olen;
}

static const char* TAG = "WS_PROTOCOL";

// 回调
static ws_on_tts_cb_t s_tts_cb = NULL;
static ws_on_live2d_cb_t s_live2d_cb = NULL;
static ws_on_status_cb_t s_status_cb = NULL;
static ws_on_text_cb_t s_text_cb = NULL;
static void* s_cb_arg = NULL;

// 解析 TTS 文本
static void parse_tts_text(const cJSON* json)
{
    const cJSON* text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring) {
        ESP_LOGI(TAG, "TTS text: %s", text->valuestring);
        if (s_tts_cb) {
            s_tts_cb(text->valuestring, s_cb_arg);
        }
    }
}

// 解析 Live2D 事件（包括 emotion）
static void parse_live2d(const cJSON* json)
{
    const cJSON* action = cJSON_GetObjectItem(json, "action");
    const cJSON* name = cJSON_GetObjectItem(json, "name");
    const cJSON* expression = cJSON_GetObjectItem(json, "expression");
    const cJSON* duration_ms = cJSON_GetObjectItem(json, "duration_ms");
    const cJSON* return_to = cJSON_GetObjectItem(json, "return_to");
    
    if (!name && !expression) {
        ESP_LOGW(TAG, "Live2D: no name/expression field");
        return;
    }
    
    const char* action_str = action ? action->valuestring : "";
    const char* name_str = name ? name->valuestring : (expression ? expression->valuestring : "");
    const char* return_to_str = return_to ? return_to->valuestring : "idle";
    int duration = duration_ms ? duration_ms->valueint : 0;
    
    ESP_LOGI(TAG, "Live2D: action=%s, name=%s, duration=%dms, return_to=%s",
             action_str, name_str, duration, return_to_str);
    
    // 构建 live2d 事件
    ws_live2d_event_t event = {
        .action = action_str,
        .name = name_str,
        .duration_ms = duration,
        .return_to = return_to_str,
    };
    
    // 回调通知
    if (s_live2d_cb) {
        s_live2d_cb(&event, s_cb_arg);
    }
    
    // 如果是 emotion 事件，直接更新头像表情
    if (strcmp(action_str, "emotion") == 0 && strlen(name_str) > 0) {
        avatar_emotion_t avatar_emo = emotion_id_to_avatar(name_str);
        const char* img_path = avatar_get_image_path(avatar_emo);
        
        ESP_LOGI(TAG, "Emotion mapped: %s → avatar=%d, path=%s", 
                 name_str, avatar_emo, img_path);
        
        // 调用 avatar_ui 更新表情
        // avatar_set_emotion_with_duration(avatar_emo, duration);
        
        // 如果有返回状态，设置定时器恢复
        if (duration > 0 && strcmp(return_to_str, "idle") != 0) {
            // TODO: 使用 esp_timer 延迟后恢复为 return_to 状态
        }
    }
}

// 解析状态更新
static void parse_status(const cJSON* json)
{
    const cJSON* state = cJSON_GetObjectItem(json, "state");
    if (!state) return;
    
    bool listening = cJSON_IsTrue(cJSON_GetObjectItem(state, "listening"));
    bool thinking = cJSON_IsTrue(cJSON_GetObjectItem(state, "thinking"));
    bool speaking = cJSON_IsTrue(cJSON_GetObjectItem(state, "speaking"));
    
    ESP_LOGI(TAG, "Status: listening=%d, thinking=%d, speaking=%d",
             listening, thinking, speaking);
    
    ws_status_event_t status = {
        .listening = listening,
        .thinking = thinking,
        .speaking = speaking,
    };
    
    if (s_status_cb) {
        s_status_cb(&status, s_cb_arg);
    }
    
    // 根据状态更新 app_state
    if (thinking) {
        app_state_set_current(APP_STATE_THINKING);
    } else if (speaking) {
        app_state_set_current(APP_STATE_SPEAKING);
    } else if (listening) {
        app_state_set_current(APP_STATE_LISTENING);
    }
}

// 解析助手文字回复
static void parse_assistant_text(const cJSON* json)
{
    const cJSON* text = cJSON_GetObjectItem(json, "text");
    if (text && text->valuestring) {
        ESP_LOGI(TAG, "Assistant text: %s", text->valuestring);
        if (s_text_cb) {
            s_text_cb(text->valuestring, s_cb_arg);
        }
    }
}

// 主解析函数
void ws_protocol_parse(const char* json_str)
{
    cJSON* json = cJSON_Parse(json_str);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed: %s", json_str);
        return;
    }
    
    const cJSON* type = cJSON_GetObjectItem(json, "type");
    if (!type || !type->valuestring) {
        ESP_LOGE(TAG, "No 'type' field in JSON");
        cJSON_Delete(json);
        return;
    }
    
    const char* type_str = type->valuestring;
    ESP_LOGD(TAG, "Parsing message type: %s", type_str);
    
    if (strcmp(type_str, "tts_text") == 0) {
        parse_tts_text(json);
    }
    else if (strcmp(type_str, "live2d") == 0) {
        parse_live2d(json);
    }
    else if (strcmp(type_str, "status") == 0) {
        parse_status(json);
    }
    else if (strcmp(type_str, "assistant_text") == 0) {
        parse_assistant_text(json);
    }
    else if (strcmp(type_str, "ready") == 0) {
        ESP_LOGI(TAG, "WebSocket ready!");
        app_state_set_current(APP_STATE_IDLE);
    }
    else if (strcmp(type_str, "error") == 0) {
        const cJSON* msg = cJSON_GetObjectItem(json, "message");
        ESP_LOGE(TAG, "Server error: %s", msg ? msg->valuestring : "unknown");
        app_state_set_current(APP_STATE_ERROR);
    }
    else if (strcmp(type_str, "pong") == 0) {
        ESP_LOGD(TAG, "Pong received");
    }
    else if (strcmp(type_str, "debug_log") == 0) {
        // 忽略调试日志
    }
    else {
        ESP_LOGW(TAG, "Unknown message type: %s", type_str);
    }
    
    cJSON_Delete(json);
}

// 注册回调
void ws_protocol_register_callbacks(ws_on_tts_cb_t tts_cb,
                                    ws_on_live2d_cb_t live2d_cb,
                                    ws_on_status_cb_t status_cb,
                                    ws_on_text_cb_t text_cb,
                                    void* arg)
{
    s_tts_cb = tts_cb;
    s_live2d_cb = live2d_cb;
    s_status_cb = status_cb;
    s_text_cb = text_cb;
    s_cb_arg = arg;
}

// ============================================================
// Phase-1 STT audio: stt_audio message builder
// ============================================================

int ws_protocol_build_stt_audio(const int16_t* pcm_data, size_t sample_count,
                               bool is_last, char* out_buf, size_t buf_size)
{
    if (!pcm_data || !out_buf || buf_size < 100) {
        return -1;
    }

    // Base64 encode PCM16 samples
    // sample_count * sizeof(int16_t) bytes of PCM16
    size_t pcm_bytes = sample_count * sizeof(int16_t);
    size_t b64_len = ((pcm_bytes + 2) / 3) * 4 + 1;  // base64 output estimate

    if (buf_size < b64_len + 64) {
        ESP_LOGE(TAG, "Buffer too small for base64: need %zu, got %zu", b64_len + 64, buf_size);
        return -1;
    }

    char b64_buf[2048];
    int b64_len_out = base64_encode((const uint8_t*)pcm_data, pcm_bytes, b64_buf, sizeof(b64_buf));
    if (b64_len_out < 0) {
        ESP_LOGE(TAG, "Base64 encode failed");
        return -1;
    }
    b64_buf[b64_len_out] = '\0';

    // Build JSON: {"type":"stt_audio","audio":"<b64>","isLast":true|false}
    int written = snprintf(out_buf, buf_size,
                           "{\"type\":\"stt_audio\",\"audio\":\"%s\",\"isLast\":%s}",
                           b64_buf, is_last ? "true" : "false");

    if (written < 0 || (size_t)written >= buf_size) {
        ESP_LOGE(TAG, "stt_audio JSON build overflow");
        return -1;
    }

    ESP_LOGD(TAG, "stt_audio built: %zu samples, %d bytes b64, isLast=%d",
             sample_count, b64_len_out, is_last);

    return written;
}