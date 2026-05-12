/*
 * ui_status_bar.c — Shared status bar implementation.
 *
 * Layout (368 × 38 px, transparent background):
 *   [clock 20px]  ─── hairline ───  [FA wifi] [WS dot] [bat]
 *
 * Each app screen gets its own bar object via ui_status_bar_create().
 * Widget pointers are stored in the bar object's user_data (bar_ctx_t).
 */

#include "ui_status_bar.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "display_driver.h"   /* DISPLAY_H_RES */
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "STATUS_BAR";

/* ── Sizes ───────────────────────────────────────────────────────────────── */
#define BAR_H       38
#define HAIRLINE_Y  (BAR_H - 1)

/* ── Per-bar context (stored in lv_obj user_data) ─────────────────────────── */
typedef struct {
    lv_obj_t     *time_lbl;
    lv_obj_t     *wifi_lbl;
    lv_obj_t     *ws_dot;
    lv_obj_t     *bat_lbl;
    lv_obj_t     *notif_lbl;
    lv_timer_t   *clock_timer;
} bar_ctx_t;

/* ── Clock ───────────────────────────────────────────────────────────────── */
static void clock_cb(lv_timer_t *t)
{
    bar_ctx_t *ctx = (bar_ctx_t *)t->user_data;
    if (!ctx || !ctx->time_lbl) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm || tm->tm_year < 124) {
        lv_label_set_text(ctx->time_lbl, "--:--");
    } else {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        lv_label_set_text(ctx->time_lbl, buf);
    }
}

/* ── Battery glyph helper ────────────────────────────────────────────────── */
static const char *bat_glyph(int pct)
{
    if (pct < 0)  return "";
    if (pct > 60) return FA_BATTERY_FULL;
    if (pct > 25) return FA_BATTERY_HALF;
    return FA_BATTERY_LOW;
}

/* ── Create ──────────────────────────────────────────────────────────────── */
lv_obj_t *ui_status_bar_create(lv_obj_t *parent)
{
    const ui_theme_t *th = ui_theme_get();

    /* Allocate context */
    bar_ctx_t *ctx = (bar_ctx_t *)malloc(sizeof(bar_ctx_t));
    if (!ctx) { ESP_LOGE(TAG, "OOM"); return NULL; }
    memset(ctx, 0, sizeof(*ctx));

    /* Container — transparent, fixed to top */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_H_RES, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(bar, ctx);

    /* Clock label — left */
    ctx->time_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->time_lbl, "--:--");
    lv_obj_set_style_text_color(ctx->time_lbl, th->text, 0);
    lv_obj_set_style_text_font(ctx->time_lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(ctx->time_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(ctx->time_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    /* WS dot — rightmost */
    ctx->ws_dot = lv_obj_create(bar);
    lv_obj_set_size(ctx->ws_dot, 9, 9);
    lv_obj_set_style_radius(ctx->ws_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ctx->ws_dot, 0, 0);
    lv_obj_set_style_pad_all(ctx->ws_dot, 0, 0);
    lv_obj_set_style_bg_color(ctx->ws_dot, th->border, 0);
    lv_obj_align(ctx->ws_dot, LV_ALIGN_RIGHT_MID, -12, 0);

    /* WiFi icon — to the left of WS dot */
    ctx->wifi_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->wifi_lbl, FA_WIFI);
    lv_obj_set_style_text_font(ctx->wifi_lbl, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(ctx->wifi_lbl, th->border, 0);
    lv_obj_set_style_bg_opa(ctx->wifi_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align_to(ctx->wifi_lbl, ctx->ws_dot, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    /* Battery icon — to the left of WiFi */
    ctx->bat_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->bat_lbl, FA_BATTERY_FULL);
    lv_obj_set_style_text_font(ctx->bat_lbl, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(ctx->bat_lbl, th->text_dim, 0);
    lv_obj_set_style_bg_opa(ctx->bat_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align_to(ctx->bat_lbl, ctx->wifi_lbl, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    /* Hairline separator */
    lv_obj_t *sep = lv_obj_create(bar);
    lv_obj_set_size(sep, DISPLAY_H_RES - 40, 1);
    lv_obj_set_pos(sep, 20, HAIRLINE_Y);
    lv_obj_set_style_bg_color(sep, th->border, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    /* Notification label — centered overlay */
    ctx->notif_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->notif_lbl, "");
    lv_obj_set_style_text_color(ctx->notif_lbl, th->accent, 0);
    lv_obj_set_style_text_font(ctx->notif_lbl, UI_FONT_TEXT_SM, 0);
    lv_obj_set_style_bg_opa(ctx->notif_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(ctx->notif_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Clock timer — 1 s, fires with ctx */
    ctx->clock_timer = lv_timer_create(clock_cb, 1000, ctx);
    clock_cb(ctx->clock_timer);  /* immediate first tick */

    return bar;
}

/* ── Update helpers ──────────────────────────────────────────────────────── */

void ui_status_bar_set_wifi(lv_obj_t *bar, bool connected)
{
    bar_ctx_t *ctx = (bar_ctx_t *)lv_obj_get_user_data(bar);
    if (!ctx || !ctx->wifi_lbl) return;
    const ui_theme_t *th = ui_theme_get();
    lv_obj_set_style_text_color(ctx->wifi_lbl,
        connected ? th->accent : th->border, 0);
}

void ui_status_bar_set_ws(lv_obj_t *bar, bool connected)
{
    bar_ctx_t *ctx = (bar_ctx_t *)lv_obj_get_user_data(bar);
    if (!ctx || !ctx->ws_dot) return;
    const ui_theme_t *th = ui_theme_get();
    lv_obj_set_style_bg_color(ctx->ws_dot,
        connected ? th->accent : th->border, 0);
}

void ui_status_bar_set_battery(lv_obj_t *bar, int percent)
{
    bar_ctx_t *ctx = (bar_ctx_t *)lv_obj_get_user_data(bar);
    if (!ctx || !ctx->bat_lbl) return;
    const ui_theme_t *th = ui_theme_get();
    lv_label_set_text(ctx->bat_lbl, bat_glyph(percent));
    lv_color_t col = (percent >= 0 && percent < 20) ? th->low_battery : th->text_dim;
    lv_obj_set_style_text_color(ctx->bat_lbl, col, 0);
}

void ui_status_bar_set_mute(lv_obj_t *bar, bool muted)
{
    (void)bar; (void)muted;  /* placeholder — add mute icon in future */
}

void ui_status_bar_set_notification(lv_obj_t *bar, const char *text)
{
    bar_ctx_t *ctx = (bar_ctx_t *)lv_obj_get_user_data(bar);
    if (!ctx || !ctx->notif_lbl) return;
    lv_label_set_text(ctx->notif_lbl, text ? text : "");
}

void ui_status_bar_clear_notification(lv_obj_t *bar)
{
    ui_status_bar_set_notification(bar, "");
}
