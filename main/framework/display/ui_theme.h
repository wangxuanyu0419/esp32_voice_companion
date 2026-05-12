/*
 * ui_theme.h — Centralised colour theme for all UI screens.
 *
 * Based on the Xiaozhi dark/light theme system.
 * Call ui_theme_init() once at boot; call ui_theme_get() everywhere else.
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

/* ── Theme struct ─────────────────────────────────────────────────────────── */
typedef struct {
    lv_color_t bg;                 /* screen background           */
    lv_color_t text;               /* primary text                */
    lv_color_t text_dim;           /* secondary / dim text        */
    lv_color_t chat_bg;            /* chat / card area background */
    lv_color_t user_bubble;        /* user message bubble         */
    lv_color_t assistant_bubble;   /* assistant message bubble    */
    lv_color_t system_bubble;      /* system/status message       */
    lv_color_t system_text;        /* system message text         */
    lv_color_t border;             /* separator / border          */
    lv_color_t accent;             /* cyan accent (project-wide)  */
    lv_color_t low_battery;        /* low-battery warning         */
    lv_color_t card;               /* tile / info card background */
    lv_color_t card_empty;         /* placeholder tile background */
} ui_theme_t;

/* ── API ──────────────────────────────────────────────────────────────────── */
void              ui_theme_init(void);
const ui_theme_t *ui_theme_get(void);
void              ui_theme_set_dark(bool dark);
bool              ui_theme_is_dark(void);
