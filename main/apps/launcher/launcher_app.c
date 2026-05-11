/*
 * launcher_app.c — Desktop OS home screen (LVGL v8, 368×448).
 *
 * Layout:
 *   ┌──────────────────────────┐  ← Title bar 44 px
 *   │  ESP32 OS   ●WiFi  ●WS  │
 *   ├──────────────────────────┤
 *   │  ┌──────┐   ┌──────┐   │  ← 2 columns × N rows, 150×150 px tiles
 *   │  │  🤖  │   │  ⚙️  │  │
 *   │  │      │   │      │  │
 *   │  │Chat  │   │Sett. │  │
 *   │  └──────┘   └──────┘   │
 *   ├──────────────────────────┤
 *   │  Touch tile to open app  │  ← Hint bar 24 px
 *   └──────────────────────────┘
 */

#include "launcher_app.h"
#include "app_registry.h"
#include "event_bus.h"
#include "display_driver.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "LAUNCHER";

/* -------------------------------------------------------------------------
 * UI state
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_wifi_dot    = NULL;
static lv_obj_t *s_ws_dot      = NULL;

/* -------------------------------------------------------------------------
 * Tile descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *label;    /* Display name */
    const char *app_id;   /* app_registry ID to launch */
    const char *icon;     /* LVGL symbol or short emoji fallback */
} tile_desc_t;

static const tile_desc_t k_tiles[] = {
    { "ClawChat",  "chat",     LV_SYMBOL_AUDIO  },
    { "Settings",  "settings", LV_SYMBOL_SETTINGS },
};
#define NUM_TILES  (sizeof(k_tiles) / sizeof(k_tiles[0]))

/* -------------------------------------------------------------------------
 * Tile tap handler
 * ------------------------------------------------------------------------- */
static void tile_click_cb(lv_event_t *e)
{
    const tile_desc_t *t = (const tile_desc_t *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Tile tapped: %s → launching '%s'", t->label, t->app_id);
    app_registry_launch(t->app_id);
}

/* -------------------------------------------------------------------------
 * Status dot helper  (green = ok, red = no)
 * ------------------------------------------------------------------------- */
static lv_obj_t *make_dot(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    return dot;
}

static void dot_set_color(lv_obj_t *dot, bool ok)
{
    lv_obj_set_style_bg_color(dot, ok ? lv_color_hex(0x00CC44)
                                       : lv_color_hex(0xCC3300), 0);
}

/* -------------------------------------------------------------------------
 * Build the launcher screen
 * ------------------------------------------------------------------------- */
static void build_ui(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x111111), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* --- Title bar --- */
    lv_obj_t *title_bar = lv_obj_create(s_screen);
    lv_obj_set_size(title_bar, DISPLAY_H_RES, 44);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x222233), 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_left(title_bar, 12, 0);
    lv_obj_set_style_pad_right(title_bar, 12, 0);
    lv_obj_set_style_pad_top(title_bar, 0, 0);
    lv_obj_set_flex_flow(title_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_bar,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, "ESP32 OS");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);

    /* Status dots container */
    lv_obj_t *dots = lv_obj_create(title_bar);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 2, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 6, 0);

    /* WiFi label + dot */
    lv_obj_t *wifi_lbl = lv_label_create(dots);
    lv_label_set_text(wifi_lbl, "WiFi");
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_12, 0);

    s_wifi_dot = make_dot(dots);

    /* WS label + dot */
    lv_obj_t *ws_lbl = lv_label_create(dots);
    lv_label_set_text(ws_lbl, "WS");
    lv_obj_set_style_text_color(ws_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(ws_lbl, &lv_font_montserrat_12, 0);

    s_ws_dot = make_dot(dots);

    /* --- Tile grid --- */
    lv_obj_t *grid = lv_obj_create(s_screen);
    lv_obj_set_size(grid, DISPLAY_H_RES, DISPLAY_V_RES - 44 - 28);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < NUM_TILES; i++) {
        const tile_desc_t *t = &k_tiles[i];

        lv_obj_t *tile = lv_obj_create(grid);
        lv_obj_set_size(tile, 150, 150);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1E2030), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x3344AA), 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, 16, 0);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x2A3050), LV_STATE_PRESSED);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* Icon */
        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, t->icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xCCDDFF), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);

        /* Label */
        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, t->label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void *)t);
    }

    /* --- Hint bar --- */
    lv_obj_t *hint_bar = lv_obj_create(s_screen);
    lv_obj_set_size(hint_bar, DISPLAY_H_RES, 28);
    lv_obj_align(hint_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(hint_bar, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_radius(hint_bar, 0, 0);

    lv_obj_t *hint = lv_label_create(hint_bar);
    lv_label_set_text(hint, "Tap to open  •  Hold BOOT to return here");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_center(hint);
}

/* -------------------------------------------------------------------------
 * App lifecycle callbacks
 * ------------------------------------------------------------------------- */
static esp_err_t launcher_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Launcher enter");
    if (!s_screen) build_ui();

    /* Refresh status dots */
    dot_set_color(s_wifi_dot, wifi_is_connected());
    dot_set_color(s_ws_dot,   ws_client_is_connected());

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    return ESP_OK;
}

static void launcher_on_exit(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Launcher exit");
}

static void launcher_on_event(app_t *self, const event_t *evt)
{
    (void)self;
    if (!s_screen) return;
    switch (evt->type) {
        case EVT_WIFI_CONNECTED:
        case EVT_WIFI_DISCONNECTED:
            dot_set_color(s_wifi_dot, wifi_is_connected());
            break;
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            dot_set_color(s_ws_dot, ws_client_is_connected());
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * Singleton app_t
 * ------------------------------------------------------------------------- */
static app_t s_launcher_app = {
    .id       = "launcher",
    .name     = "Launcher",
    .on_enter = launcher_on_enter,
    .on_exit  = launcher_on_exit,
    .on_event = launcher_on_event,
    .priv     = NULL,
};

app_t *launcher_app_get(void) { return &s_launcher_app; }

esp_err_t launcher_app_init(void)
{
    ESP_LOGI(TAG, "Launcher app init");
    return ESP_OK;
}
