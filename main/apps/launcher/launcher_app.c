/*
 * launcher_app.c — iOS-style home screen for ESP32-S3 Voice Companion
 *
 * Design goals
 *  • AMOLED-first dark palette  — near-black bg, per-tile accent colours
 *  • iOS squircle tiles          — 160×160 px, 26 px corner radius
 *  • Large 40 px symbols        — no text labels on tiles
 *  • Status bar                  — live clock (HH:MM) + WiFi icon + WS dot
 *  • Soft pressed feedback       — tile brightens on tap
 *
 * Screen: 368 × 448 px
 *
 *   ┌──────────────────────────────┐  ← 52 px status bar
 *   │  12:34                 📶 ● │    left=time  right=wifi+ws
 *   ├──────────────────────────────┤  ← 1 px separator
 *   │  ┌──────────┐  ┌──────────┐ │  ← row 0  y=78
 *   │  │          │  │          │ │
 *   │  │    🔉   │  │    ⚙    │ │    160 × 160, r=26
 *   │  │          │  │          │ │
 *   │  └──────────┘  └──────────┘ │
 *   │  ┌──────────┐  ┌──────────┐ │  ← row 1  y=262
 *   │  │          │  │          │ │
 *   │  │    +     │  │    +     │ │    placeholder tiles (dim)
 *   │  │          │  │          │ │
 *   │  └──────────┘  └──────────┘ │
 *   └──────────────────────────────┘
 */

#include "launcher_app.h"
#include "app_registry.h"
#include "event_bus.h"
#include "display_driver.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "lvgl.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "LAUNCHER";

/* ── Colour palette ──────────────────────────────────────────────────────
 * Soft, low-saturation tones.  Near-black bg lets AMOLED pixels turn off.
 * No high-saturation colours — everything is slightly milky / gray.
 * ----------------------------------------------------------------------- */
#define C_BG        0x09090F   /* Screen background — true AMOLED black     */
#define C_SEP       0x16162A   /* 1-px separator under status bar           */
#define C_TIME      0xC0C0CA   /* Clock — soft milky white (not pure white) */
#define C_WIFI_ON   0x6A9474   /* Sage green — connected (desaturated)      */
#define C_WIFI_OFF  0x3A3A52   /* Muted grey — disconnected                 */
#define C_WS_ON     0x6A9474   /* Same sage green for WS dot                */
#define C_WS_OFF    0x2E2E44   /* Slightly darker when off                  */

/* ── Tile colour table ───────────────────────────────────────────────────
 * Each tile carries its own identity colour.  Pressed variant is ~12%
 * lighter (computed at build_ui time via lv_color_mix).
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *app_id;       /* NULL = placeholder              */
    const char *symbol;       /* LV_SYMBOL_*                     */
    uint32_t    bg;           /* Normal background               */
    uint32_t    icon_col;     /* Symbol colour                   */
    bool        placeholder;
} tile_def_t;

static const tile_def_t k_tiles[4] = {
    /*  id          symbol              bg        icon_col  placeholder */
    /*  Chat: very dark cool-navy bg, soft slate-blue icon              */
    { "chat",     LV_SYMBOL_AUDIO,    0x111A28, 0x7A9AB8, false },
    /*  Settings: near-black warm-gray bg, light gray icon              */
    { "settings", LV_SYMBOL_SETTINGS, 0x1A1A24, 0x8888A0, false },
    /*  Placeholders: nearly invisible                                   */
    { NULL,       LV_SYMBOL_PLUS,     0x0D0D14, 0x222232, true  },
    { NULL,       LV_SYMBOL_PLUS,     0x0D0D14, 0x222232, true  },
};

/* ── Layout constants ────────────────────────────────────────────────────
 *  Screen 368 × 448
 *  Status bar: h=52, y=0
 *  Separator:  h=1,  y=52
 *  Grid origin: y=53
 *
 *  2 cols: side_pad=18, col_gap=12
 *    col0_x = 18
 *    col1_x = 18+160+12 = 190
 *
 *  2 rows: row_top_pad=25, row_gap=24
 *    row0_y = 53+25 = 78
 *    row1_y = 78+160+24 = 262
 *    bottom margin = 448-(262+160) = 26 px  (visually balanced)
 * ----------------------------------------------------------------------- */
#define STATUS_H   52
#define TILE_W     160
#define TILE_H     160
#define TILE_R     26
#define COL0_X     18
#define COL1_X     190
#define ROW0_Y     78
#define ROW1_Y     262

/* ── Widget handles ──────────────────────────────────────────────────── */
static lv_obj_t   *s_screen      = NULL;
static lv_obj_t   *s_time_lbl    = NULL;
static lv_obj_t   *s_wifi_icon   = NULL;
static lv_obj_t   *s_ws_dot      = NULL;
static lv_timer_t *s_clock_timer = NULL;

/* ── Clock callback (fires every second) ────────────────────────────── */
static void clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_time_lbl) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    /* Year < 2024 → SNTP not yet synced */
    if (!tm || tm->tm_year < 124) {
        lv_label_set_text(s_time_lbl, "--:--");
    } else {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        lv_label_set_text(s_time_lbl, buf);
    }
}

/* ── Tile tap handler ───────────────────────────────────────────────── */
static void tile_click_cb(lv_event_t *e)
{
    const tile_def_t *t = (const tile_def_t *)lv_event_get_user_data(e);
    if (!t || t->placeholder || !t->app_id) return;
    ESP_LOGI(TAG, "Tile → '%s'", t->app_id);
    app_registry_launch(t->app_id);
}

/* ── Status dot helpers ─────────────────────────────────────────────── */
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

/* ── Build the launcher screen ──────────────────────────────────────── */
static void build_ui(void)
{
    /* ---- Screen -------------------------------------------------------- */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    /* ---- Status bar ---------------------------------------------------- */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_set_size(bar, DISPLAY_H_RES, STATUS_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 18, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    /* Time — 32 px, left side */
    s_time_lbl = lv_label_create(bar);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(C_TIME), 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_bg_color(s_time_lbl, lv_color_hex(C_BG), 0); /* AA blend bg */

    /* Right cluster: WiFi icon + WS dot */
    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 2, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right,
        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 10, 0);

    /* WiFi symbol */
    s_wifi_icon = lv_label_create(right);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(C_WIFI_OFF), 0);
    lv_obj_set_style_bg_color(s_wifi_icon, lv_color_hex(C_BG), 0); /* AA blend bg */

    /* WS dot — 11 × 11 circle */
    s_ws_dot = lv_obj_create(right);
    lv_obj_set_size(s_ws_dot, 11, 11);
    lv_obj_set_style_radius(s_ws_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ws_dot, 0, 0);
    lv_obj_set_style_pad_all(s_ws_dot, 0, 0);
    lv_obj_set_style_bg_color(s_ws_dot, lv_color_hex(C_WS_OFF), 0);

    /* 1-px separator */
    lv_obj_t *sep = lv_obj_create(s_screen);
    lv_obj_set_size(sep, DISPLAY_H_RES, 1);
    lv_obj_set_pos(sep, 0, STATUS_H);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_SEP), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    /* ---- App tiles ------------------------------------------------------ */
    static const int col_x[2] = { COL0_X, COL1_X };
    static const int row_y[2] = { ROW0_Y, ROW1_Y };

    for (int i = 0; i < 4; i++) {
        const tile_def_t *t = &k_tiles[i];
        int cx = col_x[i & 1];
        int ry = row_y[i >> 1];

        /* ---- Tile container ---- */
        lv_obj_t *tile = lv_obj_create(s_screen);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, cx, ry);
        lv_obj_set_style_bg_color(tile, lv_color_hex(t->bg), 0);
        lv_obj_set_style_radius(tile, TILE_R, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);

        /* Subtle pressed-state: ~14% mix toward white = gentle brightening */
        lv_color_t pressed_bg = lv_color_mix(
            lv_color_white(), lv_color_hex(t->bg), 220);  /* 220/255 ≈ 86% bg */
        lv_obj_set_style_bg_color(tile, pressed_bg, LV_STATE_PRESSED);

        /* ---- Icon label — centred ---- */
        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, t->symbol);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(t->icon_col), 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(t->bg), 0);  /* AA blend bg */
        lv_obj_center(icon);

        /* Dim icon slightly on press (mirrors tile brightening) */
        lv_obj_set_style_text_opa(icon, LV_OPA_70, LV_STATE_PRESSED);

        /* Tap callback only for real apps */
        if (!t->placeholder) {
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(tile, tile_click_cb,
                                LV_EVENT_CLICKED, (void *)t);
        } else {
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* ---- Clock timer (1 s) -------------------------------------------- */
    clock_cb(NULL);   /* paint immediately */
    if (!s_clock_timer)
        s_clock_timer = lv_timer_create(clock_cb, 1000, NULL);
}

/* ── App lifecycle callbacks ─────────────────────────────────────────── */
static esp_err_t launcher_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Launcher enter");
    if (!s_screen) build_ui();

    update_wifi(wifi_is_connected());
    update_ws(ws_client_is_connected());
    clock_cb(NULL);  /* ensure clock shows immediately on re-entry */

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

/* ── Singleton ──────────────────────────────────────────────────────── */
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
