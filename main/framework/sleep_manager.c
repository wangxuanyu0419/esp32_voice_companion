/*
 * sleep_manager.c — Screen dim / sleep based on LVGL inactivity counter.
 *
 * State machine (transitions on LVGL inactivity time):
 *
 *   ACTIVE ──dim_timeout──▶ DIM ──sleep_timeout──▶ OFF
 *     ▲                      │                       │
 *     └──── any touch ────── ┘ ◀─────── any touch ───┘
 *
 * Activity detection:
 *   lv_disp_get_inactive_time() returns ms since the last LVGL input event.
 *   When a new touch arrives, this value resets (drops).  The 1-second timer
 *   compares the new value against the previous sample; a drop means activity.
 *
 * Brightness levels:
 *   ACTIVE  → 0xFF  (full)
 *   DIM     → 0x28  (~16% — visible but clearly dimmed)
 *   OFF     → 0x00  (blank) + DISPOFF command
 */

#include "sleep_manager.h"
#include "display_driver.h"
#include "config_store.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "SLEEP_MGR";

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define BRIGHTNESS_FULL  0xFF
#define BRIGHTNESS_DIM   0x28   /* ~16% */
#define BRIGHTNESS_OFF   0x00

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
typedef enum {
    SLEEP_ACTIVE = 0,
    SLEEP_DIM,
    SLEEP_OFF,
} sleep_state_t;

static sleep_state_t s_state            = SLEEP_ACTIVE;
static uint32_t      s_dim_timeout_ms   = 0;   /* 0 = never */
static uint32_t      s_sleep_timeout_ms = 0;   /* 0 = never */
static uint32_t      s_last_inactive_ms = 0;   /* previous sample for delta detection */
static lv_timer_t   *s_timer            = NULL;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */
static void apply_state(sleep_state_t new_state)
{
    if (new_state == s_state) return;
    s_state = new_state;

    switch (new_state) {
    case SLEEP_ACTIVE:
        ESP_LOGI(TAG, "Wake → ACTIVE");
        display_set_on(true);
        display_set_brightness(BRIGHTNESS_FULL);
        break;
    case SLEEP_DIM:
        ESP_LOGI(TAG, "Inactivity → DIM");
        display_set_brightness(BRIGHTNESS_DIM);
        break;
    case SLEEP_OFF:
        ESP_LOGI(TAG, "Inactivity → SCREEN OFF");
        display_set_brightness(BRIGHTNESS_OFF);
        display_set_on(false);
        break;
    }
}

/* -------------------------------------------------------------------------
 * LVGL timer callback — runs every 1 s from lv_timer_handler()
 * ------------------------------------------------------------------------- */
static void sleep_timer_cb(lv_timer_t *t)
{
    (void)t;

    uint32_t inactive_ms = lv_disp_get_inactive_time(lv_disp_get_default());

    /* A drop in inactive_time means a touch event occurred since last tick */
    bool activity = (inactive_ms < s_last_inactive_ms);
    s_last_inactive_ms = inactive_ms;

    /* Wake on any new touch */
    if (activity && s_state != SLEEP_ACTIVE) {
        apply_state(SLEEP_ACTIVE);
        return;
    }

    /* Forward transitions (only when currently not woken by touch) */
    switch (s_state) {
    case SLEEP_ACTIVE:
        if (s_sleep_timeout_ms > 0 && inactive_ms >= s_sleep_timeout_ms) {
            apply_state(SLEEP_OFF);
        } else if (s_dim_timeout_ms > 0 && inactive_ms >= s_dim_timeout_ms) {
            apply_state(SLEEP_DIM);
        }
        break;

    case SLEEP_DIM:
        if (s_sleep_timeout_ms > 0 && inactive_ms >= s_sleep_timeout_ms) {
            apply_state(SLEEP_OFF);
        }
        break;

    case SLEEP_OFF:
        /* stay off until touch detected (handled by activity check above) */
        break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
esp_err_t sleep_manager_init(void)
{
    app_config_t cfg = {0};
    config_get(&cfg);

    s_dim_timeout_ms   = (uint32_t)cfg.dim_timeout_s   * 1000;
    s_sleep_timeout_ms = (uint32_t)cfg.sleep_timeout_s * 1000;
    s_state            = SLEEP_ACTIVE;
    s_last_inactive_ms = 0;

    ESP_LOGI(TAG, "Sleep manager init: dim=%us sleep=%us",
             cfg.dim_timeout_s, cfg.sleep_timeout_s);

    /* 1-second LVGL timer — fires inside lv_timer_handler(), safe for LVGL calls */
    s_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create LVGL timer");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void sleep_manager_set_timeouts(uint16_t dim_s, uint16_t sleep_s)
{
    ESP_LOGI(TAG, "Timeouts updated: dim=%us sleep=%us", dim_s, sleep_s);
    s_dim_timeout_ms   = (uint32_t)dim_s   * 1000;
    s_sleep_timeout_ms = (uint32_t)sleep_s * 1000;

    /* If we're currently dim/off but the new setting disables that state, wake */
    if (s_state == SLEEP_DIM  && s_dim_timeout_ms   == 0) apply_state(SLEEP_ACTIVE);
    if (s_state == SLEEP_OFF  && s_sleep_timeout_ms == 0) apply_state(SLEEP_ACTIVE);
}

void sleep_manager_activity(void)
{
    /* Force a large "last" value so the next timer tick sees it as activity */
    s_last_inactive_ms = UINT32_MAX;
    if (s_state != SLEEP_ACTIVE) {
        apply_state(SLEEP_ACTIVE);
    }
}
