#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * 配置存储 - NVS
 * 
 * 存储项目：
 * - WiFi SSID/Password
 * - 服务器 URL
 * - 设备 ID
 * - Agent 名称
 * - 音量
 */

// 应用配置结构
typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char server_url[128];     // 例如 "wss://clawchat.xuanyu.uk"
    char device_id[32];        // 设备唯一标识
    char agent[16];           // Agent: main / ella / daphne
    int volume;               // 音量 0-100
    bool auto_play_tts;        // 自动播放 TTS
    bool beep_feedback;        // 按键提示音
} app_config_t;

// 默认配置
#define DEFAULT_DEVICE_ID     "esp32s3_001"
#define DEFAULT_AGENT         "main"
#define DEFAULT_SERVER_URL    "wss://clawchat.xuanyu.uk"
#define DEFAULT_VOLUME        80
#define DEFAULT_AUTO_PLAY_TTS true
#define DEFAULT_BEEP_FEEDBACK  true

// 初始化配置存储
esp_err_t config_init(void);

// 获取配置
esp_err_t config_get(app_config_t* cfg);

// 保存配置
esp_err_t config_save(const app_config_t* cfg);

// 重置为默认配置
esp_err_t config_reset(void);

// 生成设备 ID（基于 MAC 地址）
esp_err_t config_generate_device_id(char* id, size_t len);

#endif // CONFIG_STORE_H