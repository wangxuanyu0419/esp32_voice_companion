#pragma once
/*
 * font_manager.h — Load comprehensive CJK fonts from SPIFFS into PSRAM.
 *
 * Two fonts are loaded at startup:
 *   g_font_cn_20  — 20 pt, 3780 Chinese chars + ASCII (for chat bubbles)
 *   g_font_cn_16  — 16 pt, same charset (for smaller labels)
 *
 * On load failure, both pointers fall back to the compact flash fonts so the
 * UI never crashes.
 */
#include "lvgl.h"
#include "esp_err.h"

/* Loaded comprehensive fonts — valid after font_manager_init() */
extern lv_font_t *g_font_cn_20;
extern lv_font_t *g_font_cn_16;

/**
 * @brief  Mount SPIFFS and load the two binary font files into PSRAM.
 *         Call once, after display_driver_init() and before avatar_init().
 * @return ESP_OK on success; ESP_FAIL if fonts could not be loaded
 *         (falls back to flash fonts, returns error for logging).
 */
esp_err_t font_manager_init(void);
