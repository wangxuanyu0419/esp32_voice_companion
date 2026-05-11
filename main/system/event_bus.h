#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * event_bus — Typed event queue replacing volatile g_recording_active /
 * g_button_pressed_flag.  ISR-safe post variant provided.
 *
 * Phase A: simple single-queue design; subscriber filtering is deferred.
 */

typedef enum {
    EVT_WIFI_CONNECTED    = 0,
    EVT_WIFI_DISCONNECTED,
    EVT_WS_CONNECTED,
    EVT_WS_DISCONNECTED,
    EVT_BUTTON_PRESS,
    EVT_BUTTON_RELEASE,
    EVT_BUTTON_LONG_PRESS,
    EVT_TOUCH_TAP,
    EVT_AUDIO_TTS_END,
    EVT_STATE_CHANGED,
    EVT_APP_SWITCH,
    EVT_TYPE_MAX,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        int32_t  ival;
        uint32_t uval;
        void    *ptr;
    } data;
} event_t;

/* Initialise the event bus (call once from app_main). */
esp_err_t event_bus_init(void);

/* Post an event from a task context. */
esp_err_t event_bus_post(const event_t *evt);

/* Post from an ISR. Returns pdTRUE if a higher-priority task was woken. */
int event_bus_post_from_isr(const event_t *evt);

/* Block until the next event arrives (or timeout_ms elapses).
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT on timeout. */
esp_err_t event_bus_receive(event_t *evt, uint32_t timeout_ms);

#endif /* EVENT_BUS_H */
