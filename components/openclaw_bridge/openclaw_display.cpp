/*
 * openclaw_display.cpp — Phase 2 stub (ESP_LOG only)
 *
 * Real HW display integration requires further board.h header resolution.
 * Keep as ESP_LOG stubs for now; bridge text path still works via logs.
 */
#include "openclaw_display.h"
#include <esp_log.h>

static const char* TAG = "OC_Display";

extern "C" {

void openclaw_display_set_state(const char* state) {
    ESP_LOGI(TAG, "[DISPLAY] state=%s", state ? state : "null");
}

void openclaw_display_show_text(const char* text) {
    ESP_LOGI(TAG, "[DISPLAY] text=%s", text ? text : "null");
}

void openclaw_display_set_emotion(const char* emotion) {
    ESP_LOGI(TAG, "[DISPLAY] emotion=%s", emotion ? emotion : "null");
}

void openclaw_display_notify(const char* msg, int duration_ms) {
    ESP_LOGI(TAG, "[DISPLAY] notify=%s dur=%d", msg ? msg : "null", duration_ms);
}

}
