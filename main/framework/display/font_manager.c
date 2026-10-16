/*
 * font_manager.c — SPIFFS + LVGL font loader.
 *
 * The primary assets are legacy CBIN files from the previously validated
 * xiaozhi-fonts bundle.  cbin_font.c adapts them to the LVGL 8 callback ABI;
 * the older LVGL BIN files remain as a compatibility fallback.
 *
 * Path mapping for the fallback LVGL BIN files remains LVGL drive 'S'.
 */

#include "font_manager.h"
#include "cbin_font.h"
#include "ui_fonts.h"          /* fallback flash fonts */
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include <stdio.h>
#include <sys/stat.h>
#include <inttypes.h>

static const char *TAG = "FONT_MGR";

/* Public pointers — set by font_manager_init(), fallback to flash fonts */
lv_font_t *g_font_cn_20 = NULL;
lv_font_t *g_font_cn_16 = NULL;

/* CBIN wrappers own their PSRAM buffers for the lifetime of the application. */
static cbin_font_t *s_cbin_font_20 = NULL;
static cbin_font_t *s_cbin_font_16 = NULL;

/* ── SPIFFS mount ─────────────────────────────────────────────────────────── */
static esp_err_t spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,     /* use first "spiffs" partition */
        .max_files              = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPIFFS already mounted");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u KB used / %u KB total",
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

/* ── Load one font, return fallback on failure ────────────────────────────── */
static lv_font_t *load_legacy_font(const char *spiffs_path,
                                   const lv_font_t *fallback)
{
    /* Quick existence check before handing path to LVGL */
    struct stat st;
    if (stat(spiffs_path, &st) != 0) {
        ESP_LOGW(TAG, "Font not found: %s — using flash fallback", spiffs_path);
        return (lv_font_t *)fallback;
    }
    ESP_LOGI(TAG, "Loading %s (%ld KB) into PSRAM...", spiffs_path,
             (long)(st.st_size / 1024));

    /* Build the LVGL path: prepend drive letter 'S' */
    char lv_path[64];
    snprintf(lv_path, sizeof(lv_path), "S:%s", spiffs_path);

    lv_font_t *f = lv_font_load(lv_path);
    if (!f) {
        ESP_LOGW(TAG, "lv_font_load failed for %s — using flash fallback", lv_path);
        return (lv_font_t *)fallback;
    }
    ESP_LOGI(TAG, "Font loaded OK: %s", lv_path);
    return f;
}

/* Load one official legacy CBIN file into a single PSRAM-backed buffer. */
static cbin_font_t *load_cbin_file(const char *spiffs_path)
{
    struct stat st;
    if (stat(spiffs_path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "CBIN font not found: %s", spiffs_path);
        return NULL;
    }

    /* The common 16/20 profiles are below 2 MB; reject corrupt files. */
    if ((uint64_t)st.st_size > (uint64_t)(4 * 1024 * 1024)) {
        ESP_LOGE(TAG, "CBIN font is unexpectedly large: %s (%ld bytes)",
                 spiffs_path, (long)st.st_size);
        return NULL;
    }

    FILE *fp = fopen(spiffs_path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Cannot open CBIN font: %s", spiffs_path);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    uint8_t *data = lv_mem_alloc(size);
    if (!data) {
        ESP_LOGE(TAG, "CBIN allocation failed: %s (%u KB)", spiffs_path,
                 (unsigned)(size / 1024));
        fclose(fp);
        return NULL;
    }

    size_t got = fread(data, 1, size, fp);
    fclose(fp);
    if (got != size) {
        ESP_LOGE(TAG, "CBIN read truncated: %s (%u/%u bytes)", spiffs_path,
                 (unsigned)got, (unsigned)size);
        lv_mem_free(data);
        return NULL;
    }

    cbin_font_t *font = cbin_font_create(data, size);
    if (!font) {
        ESP_LOGE(TAG, "CBIN validation failed: %s", spiffs_path);
        lv_mem_free(data);
        return NULL;
    }
    return font;
}

/* ── Public API ───────────────────────────────────────────────────────────── */
esp_err_t font_manager_init(void)
{
    esp_err_t ret = spiffs_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falling back to compact flash fonts (no SPIFFS)");
        g_font_cn_20 = (lv_font_t *)UI_FONT_TEXT;
        g_font_cn_16 = (lv_font_t *)UI_FONT_TEXT_SM;
        return ret;
    }

    /* Loading is synchronous; retain timing evidence because the two CBIN
     * buffers are intentionally kept alive for the application's lifetime. */
    int64_t font_load_start_us = esp_timer_get_time();

    /* Prefer the broad common CJK assets.  Keep the old files as a safe
     * fallback for a staged SPIFFS update or a device with an older image. */
    s_cbin_font_20 = load_cbin_file("/spiffs/font_noto_sans_common_20_4.bin");
    s_cbin_font_16 = load_cbin_file("/spiffs/font_noto_sans_common_16_4.bin");

    if (s_cbin_font_20) {
        g_font_cn_20 = (lv_font_t *)cbin_font_get(s_cbin_font_20);
        g_font_cn_20->fallback = UI_FONT_TEXT;
    }
    else {
        g_font_cn_20 = load_legacy_font("/spiffs/puhui_20.bin", UI_FONT_TEXT);
    }

    if (s_cbin_font_16) {
        g_font_cn_16 = (lv_font_t *)cbin_font_get(s_cbin_font_16);
        g_font_cn_16->fallback = UI_FONT_TEXT_SM;
    }
    else {
        g_font_cn_16 = load_legacy_font("/spiffs/puhui_16.bin", UI_FONT_TEXT_SM);
    }

    ESP_LOGI(TAG, "Font load finished (%" PRId64 " ms)",
             (esp_timer_get_time() - font_load_start_us) / 1000);

    bool all_ok = (g_font_cn_20 != UI_FONT_TEXT) &&
                  (g_font_cn_16 != UI_FONT_TEXT_SM);
    return all_ok ? ESP_OK : ESP_FAIL;
}
