#ifndef SETTINGS_APP_H
#define SETTINGS_APP_H

/*
 * settings_app.h — Read-only system information screen.
 *
 * Shows: WiFi SSID / WS status / Device ID / Volume / Heap / Uptime.
 * Back button (top-left) or BOOT long-press returns to Launcher.
 */

#include "app_interface.h"
#include "esp_err.h"

app_t     *settings_app_get(void);
esp_err_t  settings_app_init(void);

#endif /* SETTINGS_APP_H */
