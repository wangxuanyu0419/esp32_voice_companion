/**
 * ESP32-S3 Voice Companion - 主程序入口
 *
 * 功能：类 ClawChat 语音交互设备
 * 触发：BOOT 按键 / 屏幕触摸头像
 * 输出：TTS 语音播放 + 屏幕状态显示
 *
 * Phase-1 新增：STT audio uplink
 * - 按住 BOOT 按键录音，松开发送 isLast=true
 * - PCM16 mono 16kHz 音频分 40ms chunk 通过 stt_audio WS 消息发送
 * - 接收 assistant_text / tts_text / live2d / status 并更新 UI
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "ws_protocol.h"
#include "audio_manager.h"
#include "vad_detector.h"
#include "avatar_ui.h"
#include "button_handler.h"
#include "config_store.h"
#include "led_indicator.h"
#include "app_state.h"

static const char* TAG = "APP_MAIN";

// ============================================================
// 应用状态
// ============================================================

static app_state_t g_app_state = APP_STATE_INIT;

// 事件队列（跨任务通信）
static QueueHandle_t g_event_queue = NULL;

// 状态转换锁
static SemaphoreHandle_t g_state_mutex = NULL;

// ============================================================
// Phase-1: 录音会话状态
// ============================================================

// 录音会话 volatile 标志：按钮按下时 true，松开时 false
// 由 button_press_start_cb 在 Core 1 设置，由 record_session_task 在 Core 0 读取
static volatile bool g_recording_active = false;

// 标记按钮已按下（record_session_task 等待此信号启动）
static volatile bool g_button_pressed_flag = false;

// ============================================================
// 状态转换
// ============================================================

static void app_set_state(app_state_t new_state)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    if (g_app_state != new_state) {
        ESP_LOGI(TAG, "State transition: %s → %s",
                 app_state_to_string(g_app_state),
                 app_state_to_string(new_state));
        g_app_state = new_state;

        // 通知 UI 更新
        avatar_set_state(new_state);

        // 通知 LED
        led_set_state(new_state);

        // 重置空闲计时器
        reset_idle_timer();
    }

    xSemaphoreGive(g_state_mutex);
}

static app_state_t app_get_state(void)
{
    return g_app_state;
}

// ============================================================
// Phase-1: 录音会话任务（Core 0）
//
// 按住 BOOT 键期间持续读取音频 chunk 并发送 stt_audio。
// 按钮按下时 press-start 回调设置 g_button_pressed_flag，
// 本任务检测到后开始录音并发送；按钮松开后发送 isLast=true 结束。
// ============================================================

static void record_session_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Record session task started (Core %d)", xPortGetCoreID());

    while (1) {
        // 等待按钮按下（press-start 回调设置标志）
        while (!g_button_pressed_flag) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        g_button_pressed_flag = false;

        // 确保不在 SPEAKING 中打断（允许在 IDLE/THINKING 任意状态开始）
        app_state_t cur = app_get_state();
        if (cur == APP_STATE_SPEAKING) {
            ESP_LOGW(TAG, "Currently SPEAKING, skipping record start");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 中断正在进行的会话（如果需要）
        if (cur == APP_STATE_LISTENING || cur == APP_STATE_THINKING) {
            ws_client_send_interrupt();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // 开始录音
        ESP_LOGI(TAG, "Starting audio capture (LISTENING)");
        g_recording_active = true;
        app_set_state(APP_STATE_LISTENING);
        audio_start_capture();

        // 循环读取 40ms chunk 并发送，直到按钮松开
        int16_t chunk_buf[AUDIO_CHUNK_SAMPLES];
        size_t samples_read = 0;

        while (g_recording_active) {
            // 读取下一个 chunk（最多等待 60ms）
            if (audio_manager_read_chunk(chunk_buf, &samples_read, 60)) {
                // 检查是否已标记结束（按钮松开）
                if (!g_recording_active) {
                    // 已收到停止信号，跳出发送循环
                    break;
                }

                // 发送 chunk（isLast=false，正常进行中）
                esp_err_t ret = ws_client_send_stt_audio(chunk_buf, samples_read, false);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "stt_audio send failed, continuing...");
                }
            } else {
                // 超时：检查是否已停止
                if (!g_recording_active) {
                    break;
                }
                // 否则继续等待
            }

            // 让出 CPU
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        // 停止捕获（会 flush 剩余 accumulator samples）
        ESP_LOGI(TAG, "Stopping audio capture");
        audio_stop_capture();

        // 最后再 drain 队列中所有剩余 chunk（包括 stop 时 flush 的）
        while (audio_manager_read_chunk(chunk_buf, &samples_read, 20)) {
            ws_client_send_stt_audio(chunk_buf, samples_read, true);  // isLast=true
            ESP_LOGD(TAG, "Drained remaining chunk, sent isLast=true");
        }

        // 切换到 THINKING，等待服务器响应
        ESP_LOGI(TAG, "Record done, entering THINKING");
        app_set_state(APP_STATE_THINKING);
    }

    vTaskDelete(NULL);
}

// ============================================================
// Phase-1: 按钮 press-start 回调（Core 1 — ISR 安全）
//
// 触发时机：BOOT 键按下瞬间。
// 这里只设置标志，record_session_task (Core 0) 检测到后开始录音。
// ============================================================

static void IRAM_ATTR button_press_start_cb(void* arg)
{
    (void)arg;
    // 设置按下标志（record_session_task 在 Core 0 轮询）
    g_button_pressed_flag = true;

    // 如果仍在录音中，说明是意外重入（忽略）
    if (g_recording_active) {
        return;
    }

    // 如果播放中，先停掉播放
    if (audio_is_playing()) {
        audio_stop_playback();
    }
}

static void button_release_cb(bool long_press, void* arg)
{
    (void)long_press;
    (void)arg;

    if (!g_recording_active) {
        return;
    }

    // 标记结束：record_session_task 将发送 isLast=true
    ESP_LOGI(TAG, "Button released, finalizing recording");
    g_recording_active = false;
}

// ============================================================
// Phase-1: WS 协议回调（从 ws_protocol 接收服务器消息）
// ============================================================

static void on_server_tts(void* arg, const char* text)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] tts_text: %s", text);

    app_set_state(APP_STATE_SPEAKING);
    avatar_show_message(text);
    audio_play_tts(text);
}

static void on_server_live2d(void* arg, const ws_live2d_event_t* event)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] live2d: action=%s name=%s duration=%dms",
             event->action, event->name, event->duration_ms);

    // emotion 事件：切换头像表情并自动恢复
    if (strcmp(event->action, "emotion") == 0) {
        avatar_emotion_t emo = emotion_id_to_avatar(event->name);
        if (event->duration_ms > 0) {
            avatar_set_emotion_with_duration(emo, event->duration_ms);
        } else {
            avatar_set_emotion(emo);
        }
    }
}

static void on_server_status(void* arg, const ws_status_event_t* status)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] status: listening=%d thinking=%d speaking=%d",
             status->listening, status->thinking, status->speaking);

    // 服务器状态优先级：SPEAKING > THINKING > LISTENING > IDLE
    if (status->speaking) {
        app_set_state(APP_STATE_SPEAKING);
    } else if (status->thinking) {
        app_set_state(APP_STATE_THINKING);
    } else if (status->listening) {
        app_set_state(APP_STATE_LISTENING);
    } else {
        app_set_state(APP_STATE_IDLE);
    }
}

static void on_server_text(void* arg, const char* text)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] assistant_text: %s", text);
    avatar_show_message(text);
}

// ============================================================
// 事件处理（保留用于非 WS 事件）
// ============================================================

typedef enum {
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_WS_CONNECTED,
    EVENT_WS_DISCONNECTED,
    EVENT_WS_MESSAGE,
    EVENT_TTS_END,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        char ws_msg[512];
    } data;
} app_event_t;

static void app_event_post(event_type_t type, const char* msg)
{
    app_event_t ev = {.type = type};
    if (msg) {
        snprintf(ev.data.ws_msg, sizeof(ev.data.ws_msg), "%s", msg);
    }
    xQueueSend(g_event_queue, &ev, 0);
}

static void app_event_handler(void* params)
{
    app_event_t event;

    while (1) {
        if (xQueueReceive(g_event_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
                case EVENT_WIFI_CONNECTED:
                    ESP_LOGI(TAG, "WiFi connected");
                    break;

                case EVENT_WIFI_DISCONNECTED:
                    ESP_LOGW(TAG, "WiFi disconnected");
                    if (app_get_state() != APP_STATE_SLEEP) {
                        app_set_state(APP_STATE_ERROR);
                    }
                    break;

                case EVENT_WS_CONNECTED:
                    ESP_LOGI(TAG, "WebSocket connected");
                    ws_client_send_hello();
                    break;

                case EVENT_WS_DISCONNECTED:
                    ESP_LOGW(TAG, "WebSocket disconnected");
                    if (app_get_state() != APP_STATE_SLEEP) {
                        app_set_state(APP_STATE_IDLE);
                    }
                    break;

                case EVENT_WS_MESSAGE:
                    // WS 消息已通过 ws_message_dispatcher → ws_protocol_parse 直接处理
                    break;

                case EVENT_TTS_END:
                    ESP_LOGI(TAG, "TTS playback finished");
                    app_set_state(APP_STATE_IDLE);
                    break;

                default:
                    break;
            }
        }
    }
}

// WS 消息到达时的回调（由 ws_client 在其事件中调用）
// 这里直接调用 ws_protocol_parse，然后通过协议回调处理
static void IRAM_ATTR ws_message_dispatcher(const char* type, const char* json_str, void* arg)
{
    (void)arg;
    // 直接解析（ws_protocol_parse 调用已注册的回调）
    ws_protocol_parse(json_str);
}

// ============================================================
// 应用初始化
// ============================================================

static esp_err_t app_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP32-S3 Voice Companion (Phase-1 STT Audio) ===");
    ESP_LOGI(TAG, "Chip: ESP32-S3");
    ESP_LOGI(TAG, "PSRAM: %dMB", esp_spiram_get_size() / 1024 / 1024);
    ESP_LOGI(TAG, "Flash: %dMB", spi_flash_get_chip_size() / 1024 / 1024);

    // 1. NVS 初始化
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. 配置加载
    ret = config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. WiFi 初始化
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. LED 初始化
    ret = led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(ret));
    }

    // 5. 屏幕 UI 初始化
    ret = avatar_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Avatar init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 6. 音频初始化
    ret = audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 7. 按键初始化
    ret = button_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 8. WebSocket 客户端初始化
    ret = ws_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS client init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ========================================================
    // Phase-1: 注册 WS 协议回调（接收服务器消息）
    // ========================================================
    ws_protocol_register_callbacks(
        on_server_tts,       // tts_text
        on_server_live2d,    // live2d emotion
        on_server_status,    // status
        on_server_text,      // assistant_text
        NULL
    );

    // 注册 WS 消息到达分发器
    ws_register_message_callback(ws_message_dispatcher, NULL);

    // ========================================================
    // Phase-1: 注册按键回调
    // ========================================================
    // press-start 回调：设置标志，record_session_task 收到后开始录音
    button_register_press_start_callback(button_press_start_cb, NULL);
    // 短按松开回调：发送 isLast=true，停止录音
    button_register_boot_callback(button_release_cb, NULL);

    return ESP_OK;
}

// ============================================================
// 主任务（Core 0）
// ============================================================

static void app_main_task(void* arg)
{
    (void)arg;

    // 等待 WiFi 连接
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "WiFi connected, connecting to server...");

    // 连接 WebSocket 服务器
    app_config_t cfg;
    config_get(&cfg);

    char ws_url[256];
    snprintf(ws_url, sizeof(ws_url), "%s/ws", cfg.server_url);

    ws_client_connect(ws_url);

    // 进入 IDLE 状态
    app_set_state(APP_STATE_IDLE);

    // 主循环
    while (1) {
        // 处理 LVGL 任务
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));

        // 检查深度睡眠
        if (should_enter_deep_sleep()) {
            ESP_LOGI(TAG, "Entering deep sleep...");
            app_set_state(APP_STATE_SLEEP);
            // 配置唤醒源
            esp_sleep_enable_gpio_wakeup();
            esp_deep_sleep_start();
        }
    }
}

// ============================================================
// 入口点
// ============================================================

void app_main(void)
{
    // 创建同步原语
    g_state_mutex = xSemaphoreCreateMutex();
    g_event_queue = xQueueCreate(16, sizeof(app_event_t));

    // 初始化应用
    esp_err_t ret = app_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "App init failed, rebooting...");
        esp_restart();
    }

    // 创建录音会话任务（Core 0，等待按钮按下并管理录音）
    xTaskCreatePinnedToCore(
        record_session_task,
        "record_session",
        4096,
        NULL,
        6,
        NULL,
        0  // Core 0
    );

    // 创建主任务（Core 0，WiFi + WS + UI loop）
    xTaskCreatePinnedToCore(
        app_main_task,
        "app_main",
        8192,
        NULL,
        5,
        NULL,
        0  // Core 0
    );

    // 创建事件处理任务（Core 1）
    xTaskCreatePinnedToCore(
        app_event_handler,
        "app_event",
        4096,
        NULL,
        4,
        NULL,
        1  // Core 1
    );

    ESP_LOGI(TAG, "App started, waiting for initialization...");
}
