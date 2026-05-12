/*
 * scene_chat.c — WeChat-style chat bubble screen (LVGL v8).
 *
 * Screen layout (368 × 448):
 *   [0..37]   status bar (ui_status_bar_create)
 *   [38..397] scrollable message list (360 px tall)
 *   [398..447] bottom hint bar (50 px)
 */

#include "scene_chat.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_status_bar.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SCENE_CHAT";

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define STATUS_H    38
#define BOTTOM_H    50
#define CONTENT_H   (448 - STATUS_H - BOTTOM_H)  /* 360 */
#define SCREEN_W    368
#define BUBBLE_MAX_W LV_PCT(80)

/* ── State ───────────────────────────────────────────────────────────────── */
static bool       s_initialized = false;
static lv_obj_t  *s_screen      = NULL;
static lv_obj_t  *s_status_bar  = NULL;
static lv_obj_t  *s_list        = NULL;   /* flex-column scroll container   */
static lv_obj_t  *s_bottom_hint = NULL;

/* Ring of bubble label pointers for update_last_message */
static lv_obj_t  *s_last_label  = NULL;

/* Message count for MAX_CHAT_MESSAGES eviction */
static int        s_msg_count   = 0;

/* ── Build screen ────────────────────────────────────────────────────────── */
static void build_ui(void)
{
    const ui_theme_t *th = ui_theme_get();

    /* Screen */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    s_status_bar = ui_status_bar_create(s_screen);
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());

    /* Scrollable message list */
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, SCREEN_W, CONTENT_H);
    lv_obj_set_pos(s_list, 0, STATUS_H);
    lv_obj_set_style_bg_color(s_list, th->chat_bg, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_radius(s_list, 0, 0);

    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* Bottom hint bar */
    s_bottom_hint = lv_obj_create(s_screen);
    lv_obj_set_size(s_bottom_hint, SCREEN_W, BOTTOM_H);
    lv_obj_set_pos(s_bottom_hint, 0, STATUS_H + CONTENT_H);
    lv_obj_set_style_bg_color(s_bottom_hint, th->bg, 0);
    lv_obj_set_style_bg_opa(s_bottom_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bottom_hint, 0, 0);
    lv_obj_set_style_pad_all(s_bottom_hint, 0, 0);
    lv_obj_set_style_radius(s_bottom_hint, 0, 0);
    lv_obj_clear_flag(s_bottom_hint, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint_lbl = lv_label_create(s_bottom_hint);
    lv_label_set_text(hint_lbl, "Hold BOOT to talk  •  Long-press to go Home");
    lv_obj_set_style_text_color(hint_lbl, th->system_text, 0);
    lv_obj_set_style_text_font(hint_lbl, UI_FONT_TEXT_SM, 0);
    lv_obj_set_style_bg_opa(hint_lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(hint_lbl, LV_ALIGN_CENTER, 0, 0);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t scene_chat_init(void)
{
    if (s_initialized) return ESP_OK;
    build_ui();
    s_initialized = true;
    ESP_LOGI(TAG, "Chat scene ready");
    return ESP_OK;
}

lv_obj_t *scene_chat_get_screen(void) { return s_screen; }

void scene_chat_add_message(chat_role_t role, const char *text)
{
    if (!s_initialized || !s_list) return;
    const ui_theme_t *th = ui_theme_get();

    /* Evict oldest message if at cap */
    if (s_msg_count >= MAX_CHAT_MESSAGES) {
        lv_obj_t *first = lv_obj_get_child(s_list, 0);
        if (first) lv_obj_del(first);
        s_msg_count--;
    }

    /* Bubble container */
    lv_obj_t *bubble = lv_obj_create(s_list);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_70, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bubble, BUBBLE_MAX_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    switch (role) {
        case CHAT_ROLE_USER:
            lv_obj_set_style_bg_color(bubble, th->user_bubble, 0);
            lv_obj_set_style_align(bubble, LV_ALIGN_RIGHT_MID, 0);
            break;
        case CHAT_ROLE_ASSISTANT:
            lv_obj_set_style_bg_color(bubble, th->assistant_bubble, 0);
            lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);
            break;
        case CHAT_ROLE_SYSTEM:
            lv_obj_set_style_bg_color(bubble, th->system_bubble, 0);
            lv_obj_set_style_align(bubble, LV_ALIGN_CENTER, 0);
            lv_obj_set_width(bubble, LV_PCT(90));
            break;
    }

    /* Text label inside bubble */
    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_font(lbl, UI_FONT_TEXT, 0);

    lv_color_t text_col;
    if (role == CHAT_ROLE_SYSTEM) text_col = th->system_text;
    else                          text_col = th->text;
    lv_obj_set_style_text_color(lbl, text_col, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);

    s_last_label = lbl;
    s_msg_count++;

    /* Scroll to bottom */
    lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_ON);
}

void scene_chat_update_last_message(const char *text)
{
    if (!s_last_label) return;
    lv_label_set_text(s_last_label, text ? text : "");
    lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_ON);
}

void scene_chat_clear(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_msg_count  = 0;
    s_last_label = NULL;
}

void scene_chat_reset(void)
{
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen      = NULL;
        s_status_bar  = NULL;
        s_list        = NULL;
        s_bottom_hint = NULL;
        s_last_label  = NULL;
        s_msg_count   = 0;
        s_initialized = false;
    }
}

void scene_chat_set_state_text(const char *text)
{
    if (!s_status_bar) return;
    if (text && *text)
        ui_status_bar_set_notification(s_status_bar, text);
    else
        ui_status_bar_clear_notification(s_status_bar);
}
