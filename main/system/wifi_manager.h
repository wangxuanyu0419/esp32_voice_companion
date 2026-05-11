#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t    wifi_manager_init(void);
bool         wifi_is_connected(void);
int          wifi_get_rssi(void);
const char  *wifi_get_ssid(void);
esp_err_t    wifi_start_provisioning(void);
esp_err_t    wifi_stop_provisioning(void);
esp_err_t    wifi_disconnect(void);
esp_err_t    wifi_connect(const char *ssid, const char *password);

typedef void (*wifi_event_cb_t)(bool connected);
void wifi_register_event_callback(wifi_event_cb_t cb);

#endif /* WIFI_MANAGER_H */
