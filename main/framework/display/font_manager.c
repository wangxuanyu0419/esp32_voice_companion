/*
 * font_manager.c — SPIFFS + LVGL binary font loader.
 *
 * Fonts are stored in the SPIFFS partition (9.9 MB) as binary files generated
 * by lv_font_conv --format bin.  They are loaded at startup by lv_font_load()
 * which allocates the entire font bitmap data in heap (→ PSRAM on ESP32-S3
 * because CONFIG_SPIRAM_USE_MALLOC=y redirects allocations > 16 KB there).
 *
 * Path mapping:  LVGL drive 'S' + LV_FS_POSIX_PATH "" →
 *   lv_font_load("S:/spiffs/puhui_20.bin")  →  fopen("/spiffs/puhui_20.bin")
 */

#include "font_manager.h"
#include "ui_fonts.h"          /* fallback flash fonts */
#include "esp_log.h"
#include "esp_spiffs.h"
#include <sys/stat.h>

static const char *TAG = "FONT_MGR";

/* Public pointers — set by font_manager_init(), fallback to flash fonts */
lv_font_t *g_font_cn_20 = NULL;
lv_font_t *g_font_cn_16 = NULL;

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
static lv_font_t *load_font(const char *spiffs_path, const lv_font_t *fallback)
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

    g_font_cn_20 = load_font("/spiffs/puhui_20.bin", UI_FONT_TEXT);
    g_font_cn_16 = load_font("/spiffs/puhui_16.bin", UI_FONT_TEXT_SM);

    bool all_ok = (g_font_cn_20 != UI_FONT_TEXT) &&
                  (g_font_cn_16 != UI_FONT_TEXT_SM);
    return all_ok ? ESP_OK : ESP_FAIL;
}
