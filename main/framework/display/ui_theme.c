/*
 * ui_theme.c — Dark / light theme definitions.
 *
 * ui_theme_init() initialises the colour tables. It is IDEMPOTENT:
 * calling it multiple times (from avatar_init, launcher_app_init, etc.)
 * does NOT reset the user's dark/light preference.
 *
 * Dark theme is the factory default. Call ui_theme_set_dark(false) AFTER
 * ui_theme_init() to switch to light.
 */

#include "ui_theme.h"

/* ── State ────────────────────────────────────────────────────────────────── */
static bool       s_dark        = true;   /* preference survives re-init calls */
static bool       s_colors_init = false;  /* colour tables filled?             */
static ui_theme_t s_dark_theme;
static ui_theme_t s_light_theme;

/* ── Init ─────────────────────────────────────────────────────────────────── */
void ui_theme_init(void)
{
    /* Rebuild colour tables every call (cheap, idempotent).
     * Do NOT touch s_dark here — the user's preference must survive. */

    /* ── Dark theme — AMOLED-optimised ───────────────────────────────────── */
    s_dark_theme.bg               = lv_color_hex(0x000000);
    s_dark_theme.text             = lv_color_hex(0xFFFFFF);
    s_dark_theme.text_dim         = lv_color_hex(0x8B95A7);
    s_dark_theme.chat_bg          = lv_color_hex(0x1F1F1F);
    s_dark_theme.user_bubble      = lv_color_hex(0x00BB00);
    s_dark_theme.assistant_bubble = lv_color_hex(0x2A2A2A);
    s_dark_theme.system_bubble    = lv_color_hex(0x333333);
    s_dark_theme.system_text      = lv_color_hex(0x888888);
    s_dark_theme.border           = lv_color_hex(0x2A3040);
    s_dark_theme.accent           = lv_color_hex(0x18DFF2);
    s_dark_theme.low_battery      = lv_color_hex(0xFF3333);
    s_dark_theme.card             = lv_color_hex(0x161B22);
    s_dark_theme.card_empty       = lv_color_hex(0x0F1318);
    s_dark_theme.card_pressed     = lv_color_hex(0x1E2A3A);

    /* ── Light theme — Xiaozhi spec ──────────────────────────────────────────
     *   bg / chat_bg  : #FFFFFF  (pure white, matching spec)
     *   text          : #000000  (pure black)
     *   cards         : #F5F5F5  (slightly off-white so tiles are visible on white bg)
     *   card_empty    : #EBEBEB
     *   bubbles       : user #07C160 (WeChat green), assistant #F0F0F0 (light gray)
     *   border/sep    : #E0E0E0  (soft divider)
     *   text_dim      : #666666
     *   accent        : #18DFF2  (keep project cyan)
     * ─────────────────────────────────────────────────────────────────────── */
    s_light_theme.bg               = lv_color_hex(0xFFFFFF);
    s_light_theme.text             = lv_color_hex(0x000000);
    s_light_theme.text_dim         = lv_color_hex(0x666666);
    s_light_theme.chat_bg          = lv_color_hex(0xFFFFFF);
    s_light_theme.user_bubble      = lv_color_hex(0x07C160);
    s_light_theme.assistant_bubble = lv_color_hex(0xF0F0F0);
    s_light_theme.system_bubble    = lv_color_hex(0xE8E8E8);
    s_light_theme.system_text      = lv_color_hex(0x666666);
    s_light_theme.border           = lv_color_hex(0xEFE2CC);
    s_light_theme.accent           = lv_color_hex(0x18DFF2);
    s_light_theme.low_battery      = lv_color_hex(0xFF3B30);
    s_light_theme.card             = lv_color_hex(0xFFF7E8);
    s_light_theme.card_empty       = lv_color_hex(0xF1E5D2);
    s_light_theme.card_pressed     = lv_color_hex(0xF1E5D2);

    /* Mark colours as ready; DO NOT reset s_dark */
    s_colors_init = true;
}

/* ── API ──────────────────────────────────────────────────────────────────── */
const ui_theme_t *ui_theme_get(void)
{
    if (!s_colors_init) ui_theme_init();
    return s_dark ? &s_dark_theme : &s_light_theme;
}

void ui_theme_set_dark(bool dark) { s_dark = dark; }

bool ui_theme_is_dark(void) { return s_dark; }
