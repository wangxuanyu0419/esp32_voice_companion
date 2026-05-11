/*
 * settings_app.c — Read-only device information screen.
 *
 * Screen layout (368×448):
 *   ┌────────────────────────────────┐
 *   │ ←  Settings                   │  44 px header
 *   ├────────────────────────────────┤
 *   │ WiFi      AG Jacob ✓           │
 *   │ Server    clawchat.uk          │
 *   │ WebSocket Connected ✓          │
 *   │ Device    esp32s3_001          │
 *   │ Volume    80%                  │
 *   ├────────────────────────────────┤
 *   │ Free heap   X.X MB             │
 *   │ Uptime      HH:MM:SS           │
 *   └────────────────────────────────┘
 */

#include "settings_app.h"
#include "app_registry.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "display_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SETTINGS";

/* -------------------------------------------------------------------------
 * UI state
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_wifi_val    = NULL;
static lv_obj_t *s_ws_val      = NULL;
static lv_obj_t *s_heap_val    = NULL;
static lv_obj_t *s_uptime_val  = NULL;
static lv_timer_t *s_refresh_timer = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static lv_obj_t *make_row(lv_obj_t *parent,
                           const char *key,
                           const char *val_init,
                           lv_obj_t  **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, DISPLAY_H_RES - 24, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *key_lbl = lv_label_create(row);
    lv_label_set_text(key_lbl, key);
    lv_obj_set_style_text_color(key_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(key_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, val_init);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_14, 0);

    if (val_out) *val_out = val_lbl;
    return row;
}

static void refresh_dynamic_rows(void)
{
    if (!s_screen) return;

    /* WiFi */
    lv_label_set_text(s_wifi_val,
        wifi_is_connected() ? "Connected  " LV_SYMBOL_OK
                            : "Disconnected");
    lv_obj_set_style_text_color(s_wifi_val,
        wifi_is_connected() ? lv_color_hex(0x44FF88)
                            : lv_color_hex(0xFF4444), 0);

    /* WebSocket */
    lv_label_set_text(s_ws_val,
        ws_client_is_connected() ? "Connected  " LV_SYMBOL_OK
                                 : "Disconnected");
    lv_obj_set_style_text_color(s_ws_val,
        ws_client_is_connected() ? lv_color_hex(0x44FF88)
                                 : lv_color_hex(0xFF4444), 0);

    /* Free heap */
    char buf[32];
    uint32_t free_bytes = esp_get_free_heap_size();
    snprintf(buf, sizeof(buf), "%.1f MB", free_bytes / 1048576.0f);
    lv_label_set_text(s_heap_val, buf);

    /* Uptime */
    uint64_t up_s = esp_timer_get_time() / 1000000ULL;
    snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
             up_s / 3600, (up_s % 3600) / 60, up_s % 60);
    lv_label_set_text(s_uptime_val, buf);
}

/* LVGL timer — refresh dynamic values every 2 s */
static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_dynamic_rows();
}

/* Back button → return to launcher */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    app_registry_launch("launcher");
}

/* -------------------------------------------------------------------------
 * Build settings screen
 * ------------------------------------------------------------------------- */
static void build_ui(void)
{
    app_config_t cfg = {0};
    config_get(&cfg);

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* --- Header --- */
    lv_obj_t *hdr = lv_obj_create(s_screen);
    lv_obj_set_size(hdr, DISPLAY_H_RES, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(hdr);
    lv_obj_set_size(back_btn, 40, 32);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x334466), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_pad_all(back_btn, 4, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "  Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    /* --- Content area --- */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_set_size(content, DISPLAY_H_RES, DISPLAY_V_RES - 44);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, 12, 0);
    lv_obj_set_style_pad_ver(content, 8, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 2, 0);

    /* Static rows */
    char buf[64];

    snprintf(buf, sizeof(buf), "%s",
             strlen(cfg.wifi_ssid) > 0 ? cfg.wifi_ssid : "—");
    make_row(content, LV_SYMBOL_WIFI "  WiFi", buf, NULL);

    /* Truncate server URL for display */
    const char *srv = cfg.server_url;
    const char *host = strstr(srv, "://");
    if (host) host += 3; else host = srv;
    char host_buf[40];
    strncpy(host_buf, host, sizeof(host_buf) - 1);
    host_buf[sizeof(host_buf)-1] = '\0';
    /* strip path */
    char *slash = strchr(host_buf, '/');
    if (slash) *slash = '\0';
    make_row(content, LV_SYMBOL_CALL "  Server", host_buf, NULL);

    make_row(content, LV_SYMBOL_WIFI "  WebSocket", "—", &s_ws_val);
    /* re-use wifi symbol for WS (no dedicated symbol) */
    make_row(content, LV_SYMBOL_SETTINGS "  Device", cfg.device_id, NULL);

    snprintf(buf, sizeof(buf), "%d%%", cfg.volume);
    make_row(content, LV_SYMBOL_AUDIO "  Volume", buf, NULL);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(content);
    lv_obj_set_size(sep, DISPLAY_H_RES - 24, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_set_style_pad_ver(sep, 4, 0);

    /* Dynamic rows */
    make_row(content, LV_SYMBOL_WIFI "  WiFi status", "—", &s_wifi_val);

    make_row(content, LV_SYMBOL_LIST "  Free heap", "—", &s_heap_val);
    make_row(content, LV_SYMBOL_REFRESH "  Uptime", "—", &s_uptime_val);

    refresh_dynamic_rows();
}

/* -------------------------------------------------------------------------
 * App lifecycle
 * ------------------------------------------------------------------------- */
static esp_err_t settings_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Settings enter");
    if (!s_screen) build_ui();

    refresh_dynamic_rows();

    if (!s_refresh_timer) {
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 2000, NULL);
    } else {
        lv_timer_resume(s_refresh_timer);
    }

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
    return ESP_OK;
}

static void settings_on_exit(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Settings exit");
    if (s_refresh_timer) lv_timer_pause(s_refresh_timer);
}

static void settings_on_event(app_t *self, const event_t *evt)
{
    (void)self;
    if (!s_screen) return;
    switch (evt->type) {
        case EVT_WIFI_CONNECTED:
        case EVT_WIFI_DISCONNECTED:
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            refresh_dynamic_rows();
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * Singleton
 * ------------------------------------------------------------------------- */
static app_t s_settings_app = {
    .id       = "settings",
    .name     = "Settings",
    .on_enter = settings_on_enter,
    .on_exit  = settings_on_exit,
    .on_event = settings_on_event,
    .priv     = NULL,
};

app_t *settings_app_get(void) { return &s_settings_app; }

esp_err_t settings_app_init(void)
{
    ESP_LOGI(TAG, "Settings app init");
    return ESP_OK;
}
