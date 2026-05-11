#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

/*
 * display_driver.h — Hardware init for Waveshare ESP32-S3-Touch-AMOLED-1.8.
 *
 * Initialises:
 *   • TCA9554 IO expander  (display power / reset)
 *   • SH8601 QSPI panel    (368×448 AMOLED, SPI2_HOST)
 *   • FT5x06 touch         (I2C0, addr 0x38)
 *   • LVGL display driver + touch indev
 *   • LVGL tick timer      (2 ms)
 *
 * Call display_driver_init() once before any LVGL UI code.
 * Use display_driver_lock/unlock() when calling LVGL from a non-LVGL task.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Physical resolution of the SH8601 panel */
#define DISPLAY_H_RES  368
#define DISPLAY_V_RES  448

/**
 * @brief  Full hardware + LVGL initialisation.
 *
 * Must be called before lv_scr_act() or any lv_* API.
 * Internally calls lv_init().
 */
esp_err_t display_driver_init(void);

/**
 * @brief  Acquire the LVGL mutex before calling LVGL APIs from a task
 *         other than the one running lv_timer_handler().
 *
 * @param  timeout_ms  Maximum wait time; portMAX_DELAY for infinite.
 * @return true  if the mutex was acquired.
 */
bool display_driver_lock(uint32_t timeout_ms);

/**
 * @brief  Release the LVGL mutex obtained with display_driver_lock().
 */
void display_driver_unlock(void);

/**
 * @brief  Print I2C bus scan to log (useful for diagnostics from heartbeat task).
 */
void display_driver_i2c_scan(void);

#endif /* DISPLAY_DRIVER_H */
