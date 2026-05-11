/**
 * WiFi 管理器 - 实现
 */

#include "wifi_manager.h"
#include "config_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi_default.h"
#include "freertos/event_groups.h"
#include "esp_dpp.h"

static const char* TAG = "WIFI_MANAGER";

// WiFi 事件
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT          BIT1

static bool s_is_connected = false;
static bool s_provisioning = false;
static wifi_event_cb_t s_event_cb = NULL;
static int s_retry_count = 0;
static const int MAX_RETRY = 5;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retry to connect to AP (attempt %d)", s_retry_count);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_is_connected = false;
        if (s_event_cb) s_event_cb(false);
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_event_cb) s_event_cb(true);
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "WiFi Manager initializing...");
    
    // 创建事件组
    s_wifi_event_group = xEventGroupCreate();
    
    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 创建默认 WiFi STA
    esp_netif_t* sta = esp_netif_create_default_wifi_sta();
    assert(sta);
    
    // WiFi 配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册事件处理
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                &wifi_event_handler, NULL));
    
    // 加载保存的 WiFi 配置
    app_config_t cfg_app;
    if (config_get(&cfg_app) == ESP_OK && strlen(cfg_app.wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Connecting to saved WiFi: %s", cfg_app.wifi_ssid);
        
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, cfg_app.wifi_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, cfg_app.wifi_password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGW(TAG, "No saved WiFi, use provisioning mode");
        wifi_start_provisioning();
    }
    
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_is_connected;
}

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -127;
}

const char* wifi_get_ssid(void)
{
    static char ssid[32] = {0};
    wifi_ap_record_t ap_info;
    if (esp_wifi_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
        return ssid;
    }
    return "";
}

esp_err_t wifi_start_provisioning(void)
{
    ESP_LOGI(TAG, "Starting provisioning mode...");
    s_provisioning = true;
    
    // 创建 AP 用于配置
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_VOICE",
            .ssid_len = strlen("ESP32_VOICE"),
            .password = "12345678",
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "AP started: ESP32_VOICE / 12345678");
    ESP_LOGI(TAG, "Connect to configure WiFi...");
    
    return ESP_OK;
}

esp_err_t wifi_stop_provisioning(void)
{
    ESP_LOGI(TAG, "Stopping provisioning mode...");
    s_provisioning = false;
    
    // 切换到 STA 模式
    wifi_config_t sta_config = {0};
    app_config_t cfg_app;
    if (config_get(&cfg_app) == ESP_OK && strlen(cfg_app.wifi_ssid) > 0) {
        strncpy((char*)sta_config.sta.ssid, cfg_app.wifi_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char*)sta_config.sta.password, cfg_app.wifi_password, sizeof(sta_config.sta.password));
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        esp_wifi_connect();
    }
    
    return ESP_OK;
}

esp_err_t wifi_disconnect(void)
{
    return esp_wifi_disconnect();
}

esp_err_t wifi_connect(const char* ssid, const char* password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    return esp_wifi_connect();
}

void wifi_register_event_callback(wifi_event_cb_t cb)
{
    s_event_cb = cb;
}