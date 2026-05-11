/**
 * 配置存储 - NVS 实现
 */

#include "config_store.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_base_mac.h"
#include "esp_wifi.h"

static const char* TAG = "CONFIG_STORE";
static const char* NVS_NAMESPACE = "esp32_voice";

esp_err_t config_init(void)
{
    ESP_LOGI(TAG, "Config store initializing...");
    
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 检查是否需要设置默认配置
    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, creating default config");
        config_reset();
    } else {
        nvs_close(handle);
    }
    
    ESP_LOGI(TAG, "Config store initialized");
    return ESP_OK;
}

esp_err_t config_get(app_config_t* cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 读取各配置项
    size_t len;
    
    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", cfg->wifi_ssid, &len);
    
    len = sizeof(cfg->wifi_password);
    nvs_get_str(handle, "wifi_pass", cfg->wifi_password, &len);
    
    len = sizeof(cfg->server_url);
    nvs_get_str(handle, "server_url", cfg->server_url, &len);
    
    len = sizeof(cfg->device_id);
    nvs_get_str(handle, "device_id", cfg->device_id, &len);
    
    len = sizeof(cfg->agent);
    nvs_get_str(handle, "agent", cfg->agent, &len);
    
    nvs_get_i32(handle, "volume", &cfg->volume);
    nvs_get_i32(handle, "auto_play", &cfg->auto_play_tts);
    nvs_get_i32(handle, "beep", &cfg->beep_feedback);
    
    nvs_close(handle);
    
    // 检查并填充默认值
    if (strlen(cfg->device_id) == 0) {
        strncpy(cfg->device_id, DEFAULT_DEVICE_ID, sizeof(cfg->device_id) - 1);
    }
    if (strlen(cfg->agent) == 0) {
        strncpy(cfg->agent, DEFAULT_AGENT, sizeof(cfg->agent) - 1);
    }
    if (strlen(cfg->server_url) == 0) {
        strncpy(cfg->server_url, DEFAULT_SERVER_URL, sizeof(cfg->server_url) - 1);
    }
    if (cfg->volume == 0) {
        cfg->volume = DEFAULT_VOLUME;
    }
    
    ESP_LOGI(TAG, "Config loaded: device_id=%s, agent=%s, server=%s",
             cfg->device_id, cfg->agent, cfg->server_url);
    
    return ESP_OK;
}

esp_err_t config_save(const app_config_t* cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 保存各配置项
    ret = nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, "wifi_pass", cfg->wifi_password);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, "server_url", cfg->server_url);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, "device_id", cfg->device_id);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, "agent", cfg->agent);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_i32(handle, "volume", cfg->volume);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_i32(handle, "auto_play", cfg->auto_play_tts ? 1 : 0);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_i32(handle, "beep", cfg->beep_feedback ? 1 : 0);
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_commit(handle);
    
cleanup:
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config saved successfully");
    } else {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t config_reset(void)
{
    app_config_t cfg = {0};
    
    // 设置默认值
    strncpy(cfg.device_id, DEFAULT_DEVICE_ID, sizeof(cfg.device_id) - 1);
    strncpy(cfg.agent, DEFAULT_AGENT, sizeof(cfg.agent) - 1);
    strncpy(cfg.server_url, DEFAULT_SERVER_URL, sizeof(cfg.server_url) - 1);
    strncpy(cfg.wifi_ssid, "", sizeof(cfg.wifi_ssid) - 1);
    strncpy(cfg.wifi_password, "", sizeof(cfg.wifi_password) - 1);
    cfg.volume = DEFAULT_VOLUME;
    cfg.auto_play_tts = DEFAULT_AUTO_PLAY_TTS;
    cfg.beep_feedback = DEFAULT_BEEP_FEEDBACK;
    
    return config_save(&cfg);
}

esp_err_t config_generate_device_id(char* id, size_t len)
{
    uint8_t mac[6];
    esp_err_t ret = esp_base_mac_addr_get(mac);
    if (ret != ESP_OK) {
        // 如果获取失败，使用随机 ID
        snprintf(id, len, "esp32s3_%08x", esp_random());
        return ESP_OK;
    }
    
    snprintf(id, len, "esp32s3_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}