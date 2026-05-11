/*
 * launcher_app.c — Compact dark-theme home screen
 *
 * Design spec (368 × 448 screen):
 *   Background  : #0B0F14   Card        : #161B22
 *   Text        : #EAF2FF   Sub-text    : #8B95A7    Accent : #18DFF2
 *
 *   ┌──────────────────────────────┐
 *   │ 12:34                  📶 ● │  ← status bar  h=38, transparent
 *   │ ── 10% white hairline ──── │
 *   │   ┌────────┐  ┌────────┐    │  ← row 0  y=70
 *   │   │  icon  │  │  icon  │    │    cell 150×132, r=20
 *   │   │ ClawCh │  │Settings│    │    icon 80×80 at top
 *   │   └────────┘  └────────┘    │    label 16px below
 *   │   ┌────────┐  ┌────────┐    │  ← row 1  y=210
 *   │   │   +    │  │   +    │    │    empty slots: subtle placeholder
 *   │   └────────┘  └────────┘    │
 *   │           BOOT: Home         │  ← hint   y=420  14px #7F8DA3
 *   └──────────────────────────────┘
 */

#include "launcher_app.h"
#include "app_registry.h"
#include "event_bus.h"
#include "display_driver.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "img_assets.h"
#include "esp_log.h"
#include "lvgl.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "LAUNCHER";

/* ── Colour palette ───────────────────────────────────────────────────── */
#define C_BG          0x0B0F14   /* Screen background           */
#define C_CARD        0x161B22   /* Tile / card background      */
#define C_CARD_EMPTY  0x151B24   /* Empty-slot background       */
#define C_TEXT        0xEAF2FF   /* Primary text                */
#define C_TEXT_DIM    0x8B95A7   /* Secondary text              */
#define C_HINT        0x7F8DA3   /* Bottom hint                 */
#define C_TIME        0xF5F8FF   /* Clock — near-white          */
#define C_HAIRLINE    0x1A2230   /* ~10% white separator        */
#define C_ACCENT      0x18DFF2   /* Cyan accent                 */
#define C_WIFI_ON     0x18DFF2   /* Cyan when connected         */
#define C_WIFI_OFF    0x3A4150   /* Dim grey when off           */
#define C_WS_ON       0x18DFF2
#define C_WS_OFF      0x2E3340
#define C_PLUS        0x4B5565   /* Empty-slot plus colour      */

/* ── Layout ───────────────────────────────────────────────────────────── */
#define STATUS_H      38
#define CELL_W        150
#define CELL_H        132
#define CELL_R        20
#define COL0_X        24
#define COL1_X        194
#define ROW0_Y        70
#define ROW1_Y        210
#define ICON_SZ       80
#define HINT_Y        420

/* ── Tile descriptor ──────────────────────────────────────────────────── */
typedef struct {
    const char         *app_id;     /* NULL = placeholder */
    const char         *label;
    const lv_img_dsc_t *icon;
    bool                placeholder;
} tile_def_t;

static const tile_def_t k_tiles[4] = {
    { "chat",     "ClawChat", &img_chat_icon,     false },
    { "settings", "Settings", &img_settings_icon, false },
    { NULL,        NULL,       NULL,              true  },
    { NULL,        NULL,       NULL,              true  },
};

/* ── Widget handles ───────────────────────────────────────────────────── */
static lv_obj_t   *s_screen      = NULL;
static lv_obj_t   *s_time_lbl    = NULL;
static lv_obj_t   *s_wifi_icon   = NULL;
static lv_obj_t   *s_ws_dot      = NULL;
static lv_timer_t *s_clock_timer = NULL;

/* ── Clock callback ───────────────────────────────────────────────────── */
static void clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_time_lbl) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    if (!tm || tm->tm_year < 124) {
        lv_label_set_text(s_time_lbl, "--:--");
    } else {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        lv_label_set_text(s_time_lbl, buf);
    }
}

/* ── Tile tap handler ─────────────────────────────────────────────────── */
static void tile_click_cb(lv_event_t *e)
{
    const tile_def_t *t = (const tile_def_t *)lv_event_get_user_data(e);
    if (!t || t->placeholder || !t->app_id) return;
    ESP_LOGI(TAG, "Tile → '%s'", t->app_id);
    app_registry_launch(t->app_id);
}

/* ── Status dot helpers ───────────────────────────────────────────────── */
static void update_wifi(bool on)
{
    if (s_wifi_icon)
        lv_obj_set_style_text_color(s_wifi_icon,
            lv_color_hex(on ? C_WIFI_ON : C_WIFI_OFF), 0);
}

static void update_ws(bool on)
{
    if (s_ws_dot)
        lv_obj_set_style_bg_color(s_ws_dot,
            lv_color_hex(on ? C_WS_ON : C_WS_OFF), 0);
}

/* ── Build the launcher screen ────────────────────────────────────────── */
static void build_ui(void)
{
    /* ---- Screen -------------------------------------------------------- */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Status bar (38 px, transparent) ------------------------------ */
    /* Time at left, 20 px from left edge */
    s_time_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(C_TIME), 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_opa(s_time_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_LEFT, 20, 10);

    /* WiFi icon — right side */
    s_wifi_icon = lv_label_create(s_screen);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(C_WIFI_OFF), 0);
    lv_obj_set_style_bg_opa(s_wifi_icon, LV_OPA_TRANSP, 0);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_RIGHT, -38, 11);

    /* WS dot — small circle to the right of WiFi */
    s_ws_dot = lv_obj_create(s_screen);
    lv_obj_set_size(s_ws_dot, 9, 9);
    lv_obj_set_style_radius(s_ws_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ws_dot, 0, 0);
    lv_obj_set_style_pad_all(s_ws_dot, 0, 0);
    lv_obj_set_style_bg_color(s_ws_dot, lv_color_hex(C_WS_OFF), 0);
    lv_obj_align(s_ws_dot, LV_ALIGN_TOP_RIGHT, -20, 16);

    /* Subtle hairline below status bar (~10% white) */
    lv_obj_t *sep = lv_obj_create(s_screen);
    lv_obj_set_size(sep, DISPLAY_H_RES - 40, 1);
    lv_obj_set_pos(sep, 20, STATUS_H);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_HAIRLINE), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    /* ---- App grid ----------------------------------------------------- */
    static const int col_x[2] = { COL0_X, COL1_X };
    static const int row_y[2] = { ROW0_Y, ROW1_Y };

    for (int i = 0; i < 4; i++) {
        const tile_def_t *t = &k_tiles[i];
        int cx = col_x[i & 1];
        int ry = row_y[i >> 1];

        /* ---- Card ---- */
        lv_obj_t *card = lv_obj_create(s_screen);
        lv_obj_set_size(card, CELL_W, CELL_H);
        lv_obj_set_pos(card, cx, ry);
        lv_obj_set_style_radius(card, CELL_R, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        if (t->placeholder) {
            /* Subtle placeholder — 50% opacity card, dim plus icon */
            lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD_EMPTY), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

            /* Plus glyph centred */
            lv_obj_t *plus = lv_label_create(card);
            lv_label_set_text(plus, LV_SYMBOL_PLUS);
            lv_obj_set_style_text_color(plus, lv_color_hex(C_PLUS), 0);
            lv_obj_set_style_text_font(plus, &lv_font_montserrat_24, 0);
            lv_obj_set_style_bg_opa(plus, LV_OPA_TRANSP, 0);
            lv_obj_center(plus);
            continue;
        }

        /* Active tile */
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

        /* Pressed feedback — slight lift toward accent */
        lv_obj_set_style_bg_color(card,
            lv_color_mix(lv_color_hex(C_ACCENT), lv_color_hex(C_CARD), 32),
            LV_STATE_PRESSED);

        /* Icon — 80×80 PNG, centred horizontally near the top */
        if (t->icon) {
            lv_obj_t *img = lv_img_create(card);
            lv_img_set_src(img, t->icon);
            lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_style_img_opa(img, LV_OPA_70, LV_STATE_PRESSED);
        }

        /* Name label — 8 px below icon (icon ends at y=88, label at y=96) */
        if (t->label) {
            lv_obj_t *name = lv_label_create(card);
            lv_label_set_text(name, t->label);
            lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
            lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, 0);
            lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 8 + ICON_SZ + 8);
        }

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, tile_click_cb, LV_EVENT_CLICKED, (void *)t);
    }

    /* ---- Bottom hint -------------------------------------------------- */
    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "BOOT: Home");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, HINT_Y);

    /* ---- Clock timer (1 s) -------------------------------------------- */
    clock_cb(NULL);
    if (!s_clock_timer)
        s_clock_timer = lv_timer_create(clock_cb, 1000, NULL);
}

/* ── App lifecycle callbacks ──────────────────────────────────────────── */
static esp_err_t launcher_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Launcher enter");
    if (!s_screen) build_ui();

    update_wifi(wifi_is_connected());
    update_ws(ws_client_is_connected());
    clock_cb(NULL);

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
    if (!s_screen) return;
    switch (evt->type) {
        case EVT_WIFI_CONNECTED:
        case EVT_WIFI_DISCONNECTED:
            update_wifi(wifi_is_connected());
            break;
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            update_ws(ws_client_is_connected());
            break;
        default:
            break;
    }
}

/* ── Singleton ────────────────────────────────────────────────────────── */
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
