/*
 * wifi_manager.c — WiFi 管理器，支持已知网络列表自动轮换。
 *
 * 行为：
 *   启动时：从 NVS 读取上次使用的网络索引，连接对应网络。
 *   断线后：同一网络重试 RETRIES_PER_NET 次（3次），失败后自动切换到列表中
 *          下一个网络，依次轮换，循环尝试。
 *   手动切换：wifi_manager_switch_network(idx) 立即断开并重连指定网络，
 *            同时保存索引到 NVS。
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MANAGER";

/* ── 已知网络列表（按优先级排列） ────────────────────────────────────────── */
const wifi_known_net_t g_wifi_known_nets[] = {
    { "AG Jacob",            "allBLACK#000000" },
    { "AG_Jacob_Lab_2.4GHz", "allBLACK#000000" },
    { "AG_Jacob_Lab_5GHz",   "allBLACK#000000" },
    { "TP-Link_D7D8",        "31707619"         },
    { "TP-Link_D7D8_5G",     "31707619"         },
    { "Xuanyu's Redmi",      "19970525"         },
};
const int g_wifi_known_nets_count =
    (int)(sizeof(g_wifi_known_nets) / sizeof(g_wifi_known_nets[0]));

/* ── 参数 ─────────────────────────────────────────────────────────────────── */
#define RETRIES_PER_NET   3          /* 同一网络最多重试次数，超过后换下一个 */
#define NVS_NAMESPACE     "esp32_voice"
#define NVS_KEY_NET_IDX   "wifi_net_idx"

/* ── 内部状态 ─────────────────────────────────────────────────────────────── */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static bool           s_is_connected  = false;
static bool           s_provisioning  = false;
static wifi_event_cb_t s_event_cb     = NULL;
static int            s_retry_count   = 0;
static int            s_cur_net_idx   = 0;   /* 当前尝试的网络索引 */

/* ── NVS 持久化 ────────────────────────────────────────────────────────────── */
static int load_net_idx_from_nvs(void)
{
    nvs_handle_t h;
    int32_t idx = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_NET_IDX, &idx);
        nvs_close(h);
    }
    if (idx < 0 || idx >= g_wifi_known_nets_count) idx = 0;
    return (int)idx;
}

static void save_net_idx_to_nvs(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_NET_IDX, (int32_t)idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── 应用 WiFi 配置并发起连接 ─────────────────────────────────────────────── */
static void apply_and_connect(int idx)
{
    if (idx < 0 || idx >= g_wifi_known_nets_count) idx = 0;
    const wifi_known_net_t *net = &g_wifi_known_nets[idx];
    ESP_LOGI(TAG, "Connecting to [%d] %s ...", idx, net->ssid);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     net->ssid,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, net->password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode  = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable     = true;
    wifi_cfg.sta.pmf_cfg.required    = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();
}

/* ── WiFi 事件处理 ────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* 首次启动，连接初始网络 */
        apply_and_connect(s_cur_net_idx);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_event_cb) s_event_cb(false);

        s_retry_count++;
        if (s_retry_count >= RETRIES_PER_NET) {
            /* 当前网络多次失败 → 换下一个 */
            s_retry_count = 0;
            s_cur_net_idx = (s_cur_net_idx + 1) % g_wifi_known_nets_count;
            ESP_LOGI(TAG, "Network [%d] failed, trying [%d]: %s",
                     (s_cur_net_idx - 1 + g_wifi_known_nets_count) % g_wifi_known_nets_count,
                     s_cur_net_idx,
                     g_wifi_known_nets[s_cur_net_idx].ssid);
        } else {
            ESP_LOGI(TAG, "Retry %d/%d for [%d] %s",
                     s_retry_count, RETRIES_PER_NET,
                     s_cur_net_idx, g_wifi_known_nets[s_cur_net_idx].ssid);
        }
        apply_and_connect(s_cur_net_idx);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP " IPSTR " (network [%d] %s)",
                 IP2STR(&event->ip_info.ip),
                 s_cur_net_idx, g_wifi_known_nets[s_cur_net_idx].ssid);
        s_retry_count  = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_event_cb) s_event_cb(true);
        /* 保存成功连接的网络索引，下次启动优先使用 */
        save_net_idx_to_nvs(s_cur_net_idx);
    }
}

/* ── 公共 API ──────────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "WiFi Manager 初始化... (已知网络 %d 个)", g_wifi_known_nets_count);

    /* 读取上次使用的网络索引 */
    s_cur_net_idx = load_net_idx_from_nvs();
    s_retry_count = 0;
    ESP_LOGI(TAG, "启动网络: [%d] %s", s_cur_net_idx, g_wifi_known_nets[s_cur_net_idx].ssid);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    assert(sta);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* 配置会在 WIFI_EVENT_STA_START 时通过 apply_and_connect() 设置 */
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_is_connected;
}

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -127;
}

const char *wifi_get_ssid(void)
{
    static char ssid[33] = {0};
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
        return ssid;
    }
    /* 未连接时返回正在尝试的网络名 */
    return g_wifi_known_nets[s_cur_net_idx].ssid;
}

/* 手动切换到指定网络索引，立即断开并重连 */
esp_err_t wifi_manager_switch_network(int idx)
{
    if (idx < 0 || idx >= g_wifi_known_nets_count) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "手动切换到 [%d] %s", idx, g_wifi_known_nets[idx].ssid);
    s_cur_net_idx = idx;
    s_retry_count = 0;
    save_net_idx_to_nvs(idx);
    /* 断开后 DISCONNECTED 事件会再次触发 apply_and_connect，但我们直接调用更快 */
    esp_wifi_disconnect();
    apply_and_connect(idx);
    return ESP_OK;
}

int wifi_manager_get_current_net_idx(void)
{
    return s_cur_net_idx;
}

/* ── 兼容旧接口（provisioning 暂不使用） ─────────────────────────────────── */
esp_err_t wifi_start_provisioning(void)
{
    ESP_LOGW(TAG, "wifi_start_provisioning: 已禁用，使用已知网络列表");
    return ESP_OK;
}

esp_err_t wifi_stop_provisioning(void)
{
    s_provisioning = false;
    return ESP_OK;
}

esp_err_t wifi_disconnect(void)
{
    return esp_wifi_disconnect();
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    return esp_wifi_connect();
}

void wifi_register_event_callback(wifi_event_cb_t cb)
{
    s_event_cb = cb;
}
