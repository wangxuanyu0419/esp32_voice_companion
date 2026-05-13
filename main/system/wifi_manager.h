#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ── 已知 WiFi 网络列表 ────────────────────────────────────────────────────── */
typedef struct {
    const char *ssid;
    const char *password;
} wifi_known_net_t;

extern const wifi_known_net_t g_wifi_known_nets[];
extern const int              g_wifi_known_nets_count;

/* ── 公共 API ──────────────────────────────────────────────────────────────── */
esp_err_t    wifi_manager_init(void);
bool         wifi_is_connected(void);
int          wifi_get_rssi(void);
const char  *wifi_get_ssid(void);
esp_err_t    wifi_start_provisioning(void);
esp_err_t    wifi_stop_provisioning(void);
esp_err_t    wifi_disconnect(void);
esp_err_t    wifi_connect(const char *ssid, const char *password);

/* 手动切换到已知网络列表中的某一项（idx = 0..g_wifi_known_nets_count-1）。
 * 会保存到 NVS，下次启动也会使用这个网络。 */
esp_err_t    wifi_manager_switch_network(int idx);

/* 返回当前正在尝试连接的网络索引 */
int          wifi_manager_get_current_net_idx(void);

typedef void (*wifi_event_cb_t)(bool connected);
void wifi_register_event_callback(wifi_event_cb_t cb);

#endif /* WIFI_MANAGER_H */
