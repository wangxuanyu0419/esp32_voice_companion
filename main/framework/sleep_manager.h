#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

/*
 * sleep_manager.h — Screen dim / sleep based on LVGL inactivity timer.
 *
 * State machine:
 *   ACTIVE ──dim_timeout──▶ DIM ──sleep_timeout──▶ OFF
 *     ▲                      │                       │
 *     └──────── touch ────── ┘ ◀──────── touch ──────┘
 *
 * Timeouts are configurable at runtime and persisted to NVS via config_store.
 * 0 = disabled (never transition to that state).
 */

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialise the sleep manager.
 *
 * Reads dim_timeout_s and sleep_timeout_s from NVS config, then registers
 * an LVGL timer that polls the LVGL inactivity counter every second.
 *
 * Must be called after display_driver_init() and config_init().
 */
esp_err_t sleep_manager_init(void);

/**
 * @brief Update timeout thresholds at runtime (also takes effect immediately).
 *
 * @param dim_s    Seconds of inactivity before dimming the screen.  0 = never.
 * @param sleep_s  Seconds of inactivity before turning off the screen. 0 = never.
 */
void sleep_manager_set_timeouts(uint16_t dim_s, uint16_t sleep_s);

/**
 * @brief Signal activity from a non-touch source (e.g. physical button press).
 *
 * Wakes the display immediately if it was dimmed or off, and resets the
 * internal activity baseline so the inactivity clock restarts.
 */
void sleep_manager_activity(void);

#endif /* SLEEP_MANAGER_H */
