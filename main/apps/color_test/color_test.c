/*
 * color_test.c — RGB565 byte-order diagnostic page.
 *
 * Shows 10 known reference colours so hardware colour fidelity can be
 * verified.  If byte-order is correct every swatch should look as labelled.
 *
 * Tap anywhere → return to launcher.
 */

#include "color_test.h"
#include "app_registry.h"
#include "ui_fonts.h"
#include "display_driver.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG   = "COLOR_TEST";
static lv_obj_t   *s_scr = NULL;

/* ── Diagnostic swatch table ──────────────────────────────────────────────── */
typedef struct { uint32_t hex; const char *name; bool dark_text; } swatch_t;

static const swatch_t k_diag[] = {
    { 0xFFFFFF, "WHITE    #FFFFFF", true  },
    { 0x000000, "BLACK    #000000", false },
    { 0x808080, "GRAY     #808080", false },
    { 0xFF0000, "RED      #FF0000", false },
    { 0x00FF00, "GREEN    #00FF00", true  },
    { 0x0000FF, "BLUE     #0000FF", false },
    { 0x18DFF2, "CYAN     #18DFF2", true  },
    { 0xFFF7E8, "CREAM    #FFF7E8", true  },
    { 0xF1E5D2, "BEIGE    #F1E5D2", true  },
    { 0xEFE2CC, "BORDER   #EFE2CC", true  },
};

#define N_DIAG  (sizeof(k_diag) / sizeof(k_diag[0]))

/* ── Layout constants ─────────────────────────────────────────────────────── */
#define SCR_W    DISPLAY_H_RES      /* 368 */
#define SCR_H    DISPLAY_V_RES      /* 448 */
#define HDR_H    36
#define SWATCH_H 38
#define PAD_X    8

/* ── Tap callback ─────────────────────────────────────────────────────────── */
static void tap_cb(lv_event_t *e)
{
    (void)e;
    app_registry_launch("launcher");
}

/* ── Build ────────────────────────────────────────────────────────────────── */
static void build_ui(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x202020), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, tap_cb, LV_EVENT_CLICKED, NULL);

    /* Header */
    lv_obj_t *hdr = lv_obj_create(s_scr);
    lv_obj_set_size(hdr, SCR_W, HDR_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Colour Diagnostic  —  tap to exit");
    lv_obj_set_style_text_color(title, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(title, UI_FONT_TEXT_SM, 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_TRANSP, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Swatches */
    int swatch_w = SCR_W - PAD_X * 2;
    for (int i = 0; i < (int)N_DIAG; i++) {
        int sy = HDR_H + 4 + i * (SWATCH_H + 3);

        lv_obj_t *sw = lv_obj_create(s_scr);
        lv_obj_set_size(sw, swatch_w, SWATCH_H);
        lv_obj_set_pos(sw, PAD_X, sy);
        lv_obj_set_style_bg_color(sw, lv_color_hex(k_diag[i].hex), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 4, 0);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_set_style_pad_all(sw, 0, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(sw);
        lv_label_set_text(lbl, k_diag[i].name);
        lv_obj_set_style_text_font(lbl, UI_FONT_TEXT_SM, 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(k_diag[i].dark_text ? 0x1A1A1A : 0xF2F6FF), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */
static esp_err_t ct_on_enter(app_t *self)
{
    (void)self;
    if (!s_scr) build_ui();
    lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    ESP_LOGI(TAG, "Colour diagnostic screen loaded");
    return ESP_OK;
}

static void ct_on_exit(app_t *self) { (void)self; }

static app_t s_app = {
    .id       = "color_test",
    .name     = "Color Test",
    .on_enter = ct_on_enter,
    .on_exit  = ct_on_exit,
    .on_event = NULL,
    .priv     = NULL,
};

app_t     *color_test_app_get(void)  { return &s_app; }
esp_err_t  color_test_app_init(void) { return ESP_OK; }
