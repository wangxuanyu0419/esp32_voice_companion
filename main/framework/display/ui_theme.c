/*
 * ui_theme.c — Dark / light theme definitions (Xiaozhi-inspired).
 *
 * Themes are initialised at runtime with lv_color_hex() so the values are
 * correct regardless of LVGL color depth / byte-order configuration.
 *
 * Dark theme is default (best on AMOLED).
 */

#include "ui_theme.h"

/* ── State ────────────────────────────────────────────────────────────────── */
static bool       s_dark = true;
static ui_theme_t s_dark_theme;
static ui_theme_t s_light_theme;
static bool       s_initialized = false;

/* ── Init ─────────────────────────────────────────────────────────────────── */
void ui_theme_init(void)
{
    /* Dark theme — Xiaozhi-style AMOLED dark */
    s_dark_theme.bg               = lv_color_hex(0x000000);
    s_dark_theme.text             = lv_color_hex(0xFFFFFF);
    s_dark_theme.text_dim         = lv_color_hex(0x8B95A7);
    s_dark_theme.chat_bg          = lv_color_hex(0x1F1F1F);
    s_dark_theme.user_bubble      = lv_color_hex(0x00BB00);  /* muted green  */
    s_dark_theme.assistant_bubble = lv_color_hex(0x222222);
    s_dark_theme.system_bubble    = lv_color_hex(0x333333);
    s_dark_theme.system_text      = lv_color_hex(0x888888);
    s_dark_theme.border           = lv_color_hex(0x1A2230);
    s_dark_theme.accent           = lv_color_hex(0x18DFF2);  /* cyan         */
    s_dark_theme.low_battery      = lv_color_hex(0xFF3333);
    s_dark_theme.card             = lv_color_hex(0x161B22);
    s_dark_theme.card_empty       = lv_color_hex(0x151B24);

    /* Light theme — future toggle */
    s_light_theme.bg               = lv_color_hex(0xFFFFFF);
    s_light_theme.text             = lv_color_hex(0x000000);
    s_light_theme.text_dim         = lv_color_hex(0x666666);
    s_light_theme.chat_bg          = lv_color_hex(0xE0E0E0);
    s_light_theme.user_bubble      = lv_color_hex(0x00BB00);
    s_light_theme.assistant_bubble = lv_color_hex(0xDDDDDD);
    s_light_theme.system_bubble    = lv_color_hex(0xCCCCCC);
    s_light_theme.system_text      = lv_color_hex(0x666666);
    s_light_theme.border           = lv_color_hex(0xCCCCCC);
    s_light_theme.accent           = lv_color_hex(0x18DFF2);
    s_light_theme.low_battery      = lv_color_hex(0xFF3333);
    s_light_theme.card             = lv_color_hex(0xF0F0F0);
    s_light_theme.card_empty       = lv_color_hex(0xE8E8E8);

    s_dark        = true;
    s_initialized = true;
}

/* ── API ──────────────────────────────────────────────────────────────────── */
const ui_theme_t *ui_theme_get(void)
{
    if (!s_initialized) ui_theme_init();
    return s_dark ? &s_dark_theme : &s_light_theme;
}

void ui_theme_set_dark(bool dark) { s_dark = dark; }

bool ui_theme_is_dark(void) { return s_dark; }
