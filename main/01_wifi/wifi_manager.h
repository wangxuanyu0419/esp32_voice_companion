#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * WiFi 管理器
 * - 自动连接上次保存的 WiFi
 * - 支持 fallback 到 AP 模式配网
 * - 自动重连
 */

// 初始化 WiFi 管理器
esp_err_t wifi_manager_init(void);

// 检查 WiFi 是否已连接
bool wifi_is_connected(void);

// 获取信号强度 (dBm)
int wifi_get_rssi(void);

// 获取当前 SSID
const char* wifi_get_ssid(void);

// 启动配网模式（AP + SmartConfig/WebProvision）
esp_err_t wifi_start_provisioning(void);

// 停止配网模式
esp_err_t wifi_stop_provisioning(void);

// 断开 WiFi 连接
esp_err_t wifi_disconnect(void);

// 连接到指定 WiFi（手动）
esp_err_t wifi_connect(const char* ssid, const char* password);

// WiFi 事件回调注册
typedef void (*wifi_event_cb_t)(bool connected);
void wifi_register_event_callback(wifi_event_cb_t cb);

#endif // WIFI_MANAGER_H