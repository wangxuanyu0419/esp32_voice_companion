/*
 * launcher_app.c — Home-screen launcher, Xiaozhi-style theme.
 *
 * Screen layout (368 × 448):
 *   [0..37]   Shared status bar (clock + wifi + ws dot)
 *   [70..201] Row 0: ClawChat tile, Settings tile  (150×132, r=20)
 *   [210..341] Row 1: empty placeholders
 *   [420]     Boot hint label
 */

#include "launcher_app.h"
#include "app_registry.h"
#include "event_bus.h"
#include "display_driver.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_status_bar.h"
#include "img_assets.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "LAUNCHER";

/* ── Layout ───────────────────────────────────────────────────────────────── */
#define CELL_W    150
#define CELL_H    132
#define CELL_R    20
#define COL0_X    24
#define COL1_X    194
#define ROW0_Y    70
#define ROW1_Y    210
#define ICON_SZ   80
#define HINT_Y    420

/* ── Tile descriptor ──────────────────────────────────────────────────────── */
typedef struct {
    const char         *app_id;
    const char         *label;
    const lv_img_dsc_t *icon;
    bool                placeholder;
} tile_def_t;

static const tile_def_t k_tiles[4] = {
    { "chat",     "ClawChat", &img_chat_icon,     false },
    { "settings", "Settings", &img_settings_icon, false },
    { NULL,        NULL,       NULL,               true  },
    { NULL,        NULL,       NULL,               true  },
};

/* ── Widget handles ───────────────────────────────────────────────────────── */
static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_status_bar = NULL;

/* ── Tile tap handler ─────────────────────────────────────────────────────── */
static void tile_click_cb(lv_event_t *e)
{
    const tile_def_t *t = (const tile_def_t *)lv_event_get_user_data(e);
    if (!t || t->placeholder || !t->app_id) return;
    ESP_LOGI(TAG, "Tile → '%s'", t->app_id);
    app_registry_launch(t->app_id);
}

/* ── Build the launcher screen ────────────────────────────────────────────── */
static void build_ui(void)
{
    const ui_theme_t *th = ui_theme_get();

    /* Screen */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Shared status bar */
    s_status_bar = ui_status_bar_create(s_screen);

    /* ---- App grid ---- */
    static const int col_x[2] = { COL0_X, COL1_X };
    static const int row_y[2] = { ROW0_Y, ROW1_Y };

    for (int i = 0; i < 4; i++) {
        const tile_def_t *t = &k_tiles[i];
        int cx = col_x[i & 1];
        int ry = row_y[i >> 1];

        lv_obj_t *card = lv_obj_create(s_screen);
        lv_obj_set_size(card, CELL_W, CELL_H);
        lv_obj_set_pos(card, cx, ry);
        lv_obj_set_style_radius(card, CELL_R, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        if (t->placeholder) {
            lv_obj_set_style_bg_color(card, th->card_empty, 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *plus = lv_label_create(card);
            lv_label_set_text(plus, FA_PLUS);
            lv_obj_set_style_text_color(plus, th->border, 0);
            lv_obj_set_style_text_font(plus, UI_FONT_ICON_LG, 0);
            lv_obj_set_style_bg_opa(plus, LV_OPA_TRANSP, 0);
            lv_obj_center(plus);
            continue;
        }

        /* Active tile */
        lv_obj_set_style_bg_color(card, th->card, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

        /* Press highlight: tint toward accent */
        lv_obj_set_style_bg_color(card,
            lv_color_mix(th->accent, th->card, 32),
            LV_STATE_PRESSED);

        /* Icon */
        if (t->icon) {
            lv_obj_t *img = lv_img_create(card);
            lv_img_set_src(img, t->icon);
            lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_style_img_opa(img, LV_OPA_70, LV_STATE_PRESSED);
        }

        /* Name label */
        if (t->label) {
            lv_obj_t *name = lv_label_create(card);
            lv_label_set_text(name, t->label);
            lv_obj_set_style_text_color(name, th->text, 0);
            lv_obj_set_style_text_font(name, UI_FONT_TEXT_SM, 0);
            lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, 0);
            lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 8 + ICON_SZ + 8);
        }

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, tile_click_cb, LV_EVENT_CLICKED, (void *)t);
    }

    /* Bottom hint */
    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "BOOT: Home");
    lv_obj_set_style_text_color(hint, th->system_text, 0);
    lv_obj_set_style_text_font(hint, UI_FONT_TEXT_SM, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, HINT_Y);
}

/* ── App lifecycle ────────────────────────────────────────────────────────── */
static esp_err_t launcher_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Launcher enter");
    if (!s_screen) build_ui();

    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
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
    if (!s_status_bar) return;
    switch (evt->type) {
        case EVT_WIFI_CONNECTED:
        case EVT_WIFI_DISCONNECTED:
            ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
            break;
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());
            break;
        default:
            break;
    }
}

/* ── Singleton ────────────────────────────────────────────────────────────── */
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
    ui_theme_init();
    ESP_LOGI(TAG, "Launcher app init");
    return ESP_OK;
}
