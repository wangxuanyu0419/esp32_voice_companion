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
#include "sleep_manager.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_status_bar.h"
#include "launcher_app.h"
#include "scene_chat.h"
#include "scene_avatar.h"
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
static lv_obj_t *s_screen        = NULL;
static lv_obj_t *s_status_bar    = NULL;
static lv_obj_t *s_wifi_val      = NULL;
static lv_obj_t *s_ws_val        = NULL;
static lv_obj_t *s_heap_val      = NULL;
static lv_obj_t *s_uptime_val    = NULL;
static lv_obj_t *s_dim_dd        = NULL;   /* dim-timeout dropdown */
static lv_obj_t *s_sleep_dd      = NULL;   /* sleep-timeout dropdown */
static lv_timer_t *s_refresh_timer = NULL;

/* -------------------------------------------------------------------------
 * Timeout option tables
 * ------------------------------------------------------------------------- */
static const uint16_t k_timeout_vals[] = { 15, 30, 60, 120, 300, 0 };
#define TIMEOUT_OPTS_COUNT  6
static const char *k_timeout_opts = "15 s\n30 s\n1 min\n2 min\n5 min\nNever";

static uint8_t timeout_to_idx(uint16_t secs)
{
    for (int i = 0; i < TIMEOUT_OPTS_COUNT - 1; i++)
        if (k_timeout_vals[i] == secs) return (uint8_t)i;
    return TIMEOUT_OPTS_COUNT - 1; /* "Never" */
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static lv_obj_t *make_row(lv_obj_t *parent,
                           const char *key,
                           const char *val_init,
                           lv_obj_t  **val_out)
{
    const ui_theme_t *th = ui_theme_get();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, DISPLAY_H_RES - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, th->card, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_100, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 13, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *key_lbl = lv_label_create(row);
    lv_label_set_text(key_lbl, key);
    lv_obj_set_style_text_color(key_lbl, th->text_dim, 0);
    lv_obj_set_style_text_font(key_lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(key_lbl, LV_OPA_TRANSP, 0);

    lv_obj_t *val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, val_init);
    lv_obj_set_style_text_color(val_lbl, th->text, 0);
    lv_obj_set_style_text_font(val_lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(val_lbl, LV_OPA_TRANSP, 0);

    if (val_out) *val_out = val_lbl;
    return row;
}

static void refresh_dynamic_rows(void)
{
    if (!s_screen) return;

    const ui_theme_t *th = ui_theme_get();

    /* WiFi */
    lv_label_set_text(s_wifi_val,
        wifi_is_connected() ? "Connected" : "Disconnected");
    lv_obj_set_style_text_color(s_wifi_val,
        wifi_is_connected() ? th->user_bubble : th->low_battery, 0);

    /* WebSocket */
    lv_label_set_text(s_ws_val,
        ws_client_is_connected() ? "Connected" : "Disconnected");
    lv_obj_set_style_text_color(s_ws_val,
        ws_client_is_connected() ? th->user_bubble : th->low_battery, 0);

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
 * Sleep timeout dropdowns
 * ------------------------------------------------------------------------- */

/* Style a dropdown to match the theme */
static void style_dropdown(lv_obj_t *dd)
{
    const ui_theme_t *th = ui_theme_get();

    /* Button face */
    lv_obj_set_style_bg_color(dd,     th->card, 0);
    lv_obj_set_style_bg_opa(dd,       LV_OPA_100, 0);
    lv_obj_set_style_border_color(dd, th->border, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd,       8, 0);
    lv_obj_set_style_text_color(dd,   th->text, 0);
    lv_obj_set_style_text_font(dd,    UI_FONT_TEXT, 0);
    lv_obj_set_style_pad_hor(dd,      12, 0);
    lv_obj_set_style_pad_ver(dd,      9,  0);
    /* Restore LVGL built-in font for the indicator (down-arrow symbol)
     * so LV_SYMBOL_DOWN renders correctly even when text font is PuHui. */
    lv_obj_set_style_text_font(dd,  &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dd, th->text_dim,           LV_PART_INDICATOR);

    /* Drop-down list */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list,    th->card, 0);
    lv_obj_set_style_border_color(list, th->border, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_color(list,  th->text, 0);
    lv_obj_set_style_text_font(list,   UI_FONT_TEXT, 0);
    /* Selected item highlight */
    lv_obj_set_style_bg_color(list,   th->accent,
                               LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(list,     LV_OPA_30,
                             LV_PART_SELECTED | LV_STATE_CHECKED);
}

static lv_obj_t *make_dropdown_row(lv_obj_t *parent,
                                   const char *label,
                                   const char *options,
                                   uint8_t     init_idx,
                                   lv_event_cb_t cb)
{
    const ui_theme_t *th2 = ui_theme_get();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, DISPLAY_H_RES - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row,     th2->card, 0);
    lv_obj_set_style_bg_opa(row,       LV_OPA_100, 0);
    lv_obj_set_style_radius(row,       10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row,      10, 0);
    lv_obj_set_style_pad_hor(row,      14, 0);
    lv_obj_set_flex_flow(row,          LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, th2->text_dim, 0);
    lv_obj_set_style_text_font(lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, init_idx);
    lv_obj_set_width(dd, 110);
    style_dropdown(dd);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return dd;
}

/* forward declaration */
static void build_ui(void);

/* -------------------------------------------------------------------------
 * Theme toggle
 * ------------------------------------------------------------------------- */
static void theme_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw   = lv_event_get_target(e);
    bool      dark = lv_obj_has_state(sw, LV_STATE_CHECKED);

    /* 1. Persist */
    app_config_t cfg = {0};
    config_get(&cfg);
    cfg.dark_theme = dark;
    config_save(&cfg);

    /* 2. Apply theme */
    ui_theme_set_dark(dark);

    /* 3. Invalidate all screens so they rebuild with the new theme */
    launcher_app_reset_screen();
    scene_chat_reset();
    avatar_reset_screen();

    /* 4. Rebuild settings screen in-place */
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    lv_obj_del(s_screen);
    s_screen     = NULL;
    s_status_bar = NULL;
    s_wifi_val   = NULL;
    s_ws_val     = NULL;
    s_heap_val   = NULL;
    s_uptime_val = NULL;
    s_dim_dd     = NULL;
    s_sleep_dd   = NULL;

    build_ui();
    lv_scr_load(s_screen);

    ESP_LOGI(TAG, "Theme → %s", dark ? "dark" : "light");
}

static void dim_dd_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t idx  = (uint8_t)lv_dropdown_get_selected(dd);
    app_config_t cfg = {0};
    config_get(&cfg);
    cfg.dim_timeout_s = k_timeout_vals[idx];
    config_save(&cfg);
    sleep_manager_set_timeouts(cfg.dim_timeout_s, cfg.sleep_timeout_s);
    ESP_LOGI(TAG, "Dim timeout set to %u s", cfg.dim_timeout_s);
}

static void sleep_dd_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t idx  = (uint8_t)lv_dropdown_get_selected(dd);
    app_config_t cfg = {0};
    config_get(&cfg);
    cfg.sleep_timeout_s = k_timeout_vals[idx];
    config_save(&cfg);
    sleep_manager_set_timeouts(cfg.dim_timeout_s, cfg.sleep_timeout_s);
    ESP_LOGI(TAG, "Sleep timeout set to %u s", cfg.sleep_timeout_s);
}

/* -------------------------------------------------------------------------
 * Build settings screen
 * ------------------------------------------------------------------------- */
static void build_ui(void)
{
    const ui_theme_t *th = ui_theme_get();
    app_config_t cfg = {0};
    config_get(&cfg);

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Shared status bar */
    s_status_bar = ui_status_bar_create(s_screen);
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());

    /* --- Header (below status bar) --- */
    lv_obj_t *hdr = lv_obj_create(s_screen);
    lv_obj_set_size(hdr, DISPLAY_H_RES, 50);
    lv_obj_set_pos(hdr, 0, 38);
    lv_obj_set_style_bg_color(hdr, th->bg, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 14, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Back chevron — ghost button */
    lv_obj_t *back_btn = lv_btn_create(hdr);
    lv_obj_set_size(back_btn, 38, 38);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(back_btn, th->text, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, FA_CHEVRON_LEFT);
    lv_obj_set_style_text_color(back_lbl, th->accent, 0);
    lv_obj_set_style_text_font(back_lbl, UI_FONT_ICON, 0);
    lv_obj_set_style_bg_opa(back_lbl, LV_OPA_TRANSP, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "  Settings");
    lv_obj_set_style_text_color(title, th->text, 0);
    lv_obj_set_style_text_font(title, UI_FONT_TEXT_LG, 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_TRANSP, 0);

    /* Thin separator */
    lv_obj_t *sep0 = lv_obj_create(s_screen);
    lv_obj_set_size(sep0, DISPLAY_H_RES, 1);
    lv_obj_set_pos(sep0, 0, 88);   /* 38 status + 50 header */
    lv_obj_set_style_bg_color(sep0, th->border, 0);
    lv_obj_set_style_border_width(sep0, 0, 0);
    lv_obj_set_style_radius(sep0, 0, 0);

    /* --- Content area --- */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_set_size(content, DISPLAY_H_RES, DISPLAY_V_RES - 89);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 89);
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, 16, 0);
    lv_obj_set_style_pad_ver(content, 14, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 7, 0);

    /* Static rows */
    char buf[64];

    snprintf(buf, sizeof(buf), "%s",
             strlen(cfg.wifi_ssid) > 0 ? cfg.wifi_ssid : "—");
    make_row(content, "WiFi", buf, NULL);

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
    make_row(content, "Server", host_buf, NULL);

    make_row(content, "WebSocket", "—", &s_ws_val);
    make_row(content, "Device", cfg.device_id, NULL);

    snprintf(buf, sizeof(buf), "%d%%", cfg.volume);
    make_row(content, "Volume", buf, NULL);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(content);
    lv_obj_set_size(sep, DISPLAY_H_RES - 32, 1);
    lv_obj_set_style_bg_color(sep, th->border, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    /* Dynamic rows */
    make_row(content, "WiFi status", "—", &s_wifi_val);

    make_row(content, "Free heap", "—", &s_heap_val);
    make_row(content, "Uptime", "—", &s_uptime_val);

    /* --- Theme toggle --- */
    lv_obj_t *sep_theme = lv_obj_create(content);
    lv_obj_set_size(sep_theme, DISPLAY_H_RES - 32, 1);
    lv_obj_set_style_bg_color(sep_theme, th->border, 0);
    lv_obj_set_style_border_width(sep_theme, 0, 0);
    lv_obj_set_style_pad_all(sep_theme, 0, 0);

    {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_size(row, DISPLAY_H_RES - 32, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, th->card, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                   LV_FLEX_ALIGN_CENTER,
                                   LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Dark Mode");
        lv_obj_set_style_text_color(lbl, th->text_dim, 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_TEXT, 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 52, 28);
        /* Style the switch track */
        lv_obj_set_style_bg_color(sw, th->border, 0);
        lv_obj_set_style_bg_color(sw, th->accent, LV_STATE_CHECKED);
        /* Knob */
        lv_obj_set_style_bg_color(sw, th->text, LV_PART_KNOB);
        if (cfg.dark_theme) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, theme_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* --- Sleep settings separator --- */
    lv_obj_t *sep2 = lv_obj_create(content);
    lv_obj_set_size(sep2, DISPLAY_H_RES - 32, 1);
    lv_obj_set_style_bg_color(sep2, th->border, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);
    lv_obj_set_style_pad_all(sep2, 0, 0);

    /* Section label */
    lv_obj_t *sleep_hdr = lv_label_create(content);
    lv_label_set_text(sleep_hdr, "Display Sleep");
    lv_obj_set_style_text_color(sleep_hdr, th->accent, 0);
    lv_obj_set_style_text_font(sleep_hdr, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(sleep_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(sleep_hdr, 4, 0);

    /* Dim + sleep dropdowns */
    s_dim_dd   = make_dropdown_row(content, "Dim after",
                                   k_timeout_opts,
                                   timeout_to_idx(cfg.dim_timeout_s),
                                   dim_dd_cb);
    s_sleep_dd = make_dropdown_row(content, "Sleep after",
                                   k_timeout_opts,
                                   timeout_to_idx(cfg.sleep_timeout_s),
                                   sleep_dd_cb);

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
            if (s_status_bar) ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
            refresh_dynamic_rows();
            break;
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            if (s_status_bar) ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());
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
