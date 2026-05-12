/*
 * ui_status_bar.h — Shared status bar for all app screens.
 *
 * Shows: clock (left), WiFi icon + WS dot (right).
 * Optionally shows a centered notification text overlay.
 *
 * Usage:
 *   lv_obj_t *bar = ui_status_bar_create(screen);
 *   ui_status_bar_set_wifi(bar, true);
 *   ui_status_bar_set_notification(bar, "Listening…");
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

/* Create a status bar attached to parent; returns the bar container object. */
lv_obj_t *ui_status_bar_create(lv_obj_t *parent);

/* Update individual indicators (pass the bar returned by ui_status_bar_create) */
void ui_status_bar_set_wifi(lv_obj_t *bar, bool connected);
void ui_status_bar_set_ws(lv_obj_t *bar, bool connected);
void ui_status_bar_set_battery(lv_obj_t *bar, int percent);   /* 0-100, -1 = hide */
void ui_status_bar_set_mute(lv_obj_t *bar, bool muted);

/* Centered notification overlay (transient status text) */
void ui_status_bar_set_notification(lv_obj_t *bar, const char *text);
void ui_status_bar_clear_notification(lv_obj_t *bar);
