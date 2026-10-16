#ifndef SETTINGS_APP_H
#define SETTINGS_APP_H

/*
 * settings_app.h — Read-only system information screen.
 *
 * Shows: WiFi SSID / WS status / Device ID / Volume / Heap / Uptime.
 * The page is opened from the ClawChat gear button and returns to that same
 * root scene; it is not registered as a separate system app.
 */

#include "app_interface.h"
#include "esp_err.h"

app_t     *settings_app_get(void);
esp_err_t  settings_app_init(void);

/* Open/close the settings page without changing the active system app. */
void       settings_app_open(void);
void       settings_app_close(void);

/* The ClawChat owner restores its voice/UI state after the modal page closes. */
typedef void (*settings_close_cb_t)(void);
void       settings_app_set_close_callback(settings_close_cb_t cb);

#endif /* SETTINGS_APP_H */
