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

    /* ── Light theme — iOS-inspired, good contrast on LCD/AMOLED ─────────── */
    /*
     * Background stack (light to dark):
     *   screen bg  #F2F2F7  — iOS system grouped background (soft blue-gray)
     *   card       #FFFFFF  — white cards pop on the gray bg
     *   card_empty #E5E5EA  — subtle placeholder tiles
     *
     * Text:
     *   primary  #1C1C1E  — near-black (not harsh pure black)
     *   dim      #6C6C70  — secondary gray (iOS label secondary)
     *
     * Accent: keep cyan #18DFF2 — project identity colour
     *
     * Chat bubbles (WeChat-style on light):
     *   user       #07C160  — WeChat green
     *   assistant  #FFFFFF  — white bubble, slight border
     *   system     #E5E5EA  — neutral pill
     */
    s_light_theme.bg               = lv_color_hex(0xF2F2F7);
    s_light_theme.text             = lv_color_hex(0x1C1C1E);
    s_light_theme.text_dim         = lv_color_hex(0x6C6C70);
    s_light_theme.chat_bg          = lv_color_hex(0xEFEFF4);
    s_light_theme.user_bubble      = lv_color_hex(0x07C160);
    s_light_theme.assistant_bubble = lv_color_hex(0xFFFFFF);
    s_light_theme.system_bubble    = lv_color_hex(0xE5E5EA);
    s_light_theme.system_text      = lv_color_hex(0x6C6C70);
    s_light_theme.border           = lv_color_hex(0xC7C7CC);
    s_light_theme.accent           = lv_color_hex(0x18DFF2);
    s_light_theme.low_battery      = lv_color_hex(0xFF3B30);
    s_light_theme.card             = lv_color_hex(0xFFFFFF);
    s_light_theme.card_empty       = lv_color_hex(0xE5E5EA);

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
