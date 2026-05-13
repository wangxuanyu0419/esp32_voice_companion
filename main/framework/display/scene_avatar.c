/*
 * scene_avatar.c — ClawChat main screen.
 *
 * Layout (368×448):
 *   [status bar 38px]
 *   [Nahida state image 200×200, centred]   ← tap → toggle recording
 *   [user speech bubble  — green, right-aligned, hidden until stt_final]
 *   [assistant text bubble — grey, left-aligned, hidden until assistant_text]
 *   [state label: 请说话... / 思考中... / ...]
 *
 * Call avatar_set_state() from any task to update the image + label.
 * All LVGL writes happen on the LVGL task via lv_async_call.
 */

#include "scene_avatar.h"
#include "img_assets.h"
#include "display_driver.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "font_manager.h"
#include "ui_status_bar.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "SCENE_AVATAR";

/* ── Tap callback (set by chat_app) ───────────────────────────────────────── */
static void (*s_tap_cb)(void) = NULL;

void avatar_set_tap_callback(void (*cb)(void)) { s_tap_cb = cb; }

/* ── Widget handles ───────────────────────────────────────────────────────── */
static bool      s_initialized    = false;
static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_status_bar     = NULL;
static lv_obj_t *s_state_img      = NULL;   /* Nahida image 200×200 */
static lv_obj_t *s_user_card      = NULL;   /* user speech bubble (green) */
static lv_obj_t *s_user_lbl       = NULL;
static lv_obj_t *s_bubble_card    = NULL;   /* assistant reply card */
static lv_obj_t *s_bubble_lbl     = NULL;
static lv_obj_t *s_state_lbl      = NULL;   /* 请说话... / 思考中... */

/* Avatar size & Y position */
#define AVATAR_SIZE   200
#define STATUS_H       38
#define AVATAR_GAP      4   /* gap between status bar and avatar */
/* Avatar top edge */
#define AVATAR_TOP    (STATUS_H + AVATAR_GAP)
/* Avatar bottom edge */
#define AVATAR_BOT    (AVATAR_TOP + AVATAR_SIZE)   /* = 242 */

/* Bubble layout below the avatar */
#define BUBBLE_MARGIN  16   /* horizontal padding from display edge */
#define BUBBLE_W       (DISPLAY_H_RES - 2 * BUBBLE_MARGIN)
#define USER_BUBBLE_TOP   (AVATAR_BOT + 8)
#define ASST_BUBBLE_TOP   (AVATAR_BOT + 68)  /* ~60px gap for user bubble */

/* ── State → image + label ────────────────────────────────────────────────── */
typedef struct { const lv_img_dsc_t *img; const char *label; } state_entry_t;

static const state_entry_t k_state_table[] = {
    [APP_STATE_IDLE]      = { &img_state_idle,      ""                                                   },
    [APP_STATE_LISTENING] = { &img_state_listening, "\xe8\xaf\xb7\xe8\xaf\xb4\xe8\xaf\x9d..."           },  /* 请说话... */
    [APP_STATE_THINKING]  = { &img_state_thinking,  "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad..."           },  /* 思考中... */
    [APP_STATE_SPEAKING]  = { &img_state_speaking,  "\xe6\x92\xad\xe6\x94\xbe\xe4\xb8\xad..."           },  /* 播放中... */
    [APP_STATE_ERROR]     = { &img_state_error,     "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x96\xad\xe5\xbc\x80" },  /* 连接断开 */
    [APP_STATE_SLEEP]     = { &img_state_idle,      ""                                                   },
};
#define STATE_TABLE_SIZE  (sizeof(k_state_table) / sizeof(k_state_table[0]))

/* ── Internal apply (must be called on LVGL task) ─────────────────────────── */
static void apply_state_lvgl(void *arg)
{
    if (!s_initialized) return;
    app_state_t state = (app_state_t)(uintptr_t)arg;
    if ((size_t)state >= STATE_TABLE_SIZE) state = APP_STATE_IDLE;

    const state_entry_t *e = &k_state_table[state];
    lv_img_set_src(s_state_img, e->img);
    lv_label_set_text(s_state_lbl, e->label);

    /* Update status bar indicators */
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar,   ws_client_is_connected());
}

static void apply_text_lvgl(void *arg)
{
    if (!s_initialized || !arg) return;
    const char *text = (const char *)arg;
    lv_label_set_text(s_bubble_lbl, text);
    lv_obj_set_style_opa(s_bubble_card,
        strlen(text) > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    free(arg);
}

typedef struct { char *text; bool is_user; } text_arg_t;

static void apply_user_text_lvgl(void *arg)
{
    if (!s_initialized || !arg) return;
    char *text = (char *)arg;
    lv_label_set_text(s_user_lbl, text);
    lv_obj_set_style_opa(s_user_card,
        strlen(text) > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    free(arg);
}

/* ── Touch handler ────────────────────────────────────────────────────────── */
static void img_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_tap_cb) s_tap_cb();
}

/* ── Build ────────────────────────────────────────────────────────────────── */
esp_err_t avatar_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Avatar UI initialising...");

    const ui_theme_t *th = ui_theme_get();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    s_status_bar = ui_status_bar_create(s_screen);
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar,   ws_client_is_connected());

    /* State image — 200×200, centred below status bar, tappable */
    s_state_img = lv_img_create(s_screen);
    lv_img_set_src(s_state_img, &img_state_booting);
    lv_obj_set_size(s_state_img, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_state_img, LV_ALIGN_TOP_MID, 0, AVATAR_TOP);
    lv_obj_add_flag(s_state_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_state_img, img_tap_cb, LV_EVENT_CLICKED, NULL);

    /* ── User speech bubble (green, WeChat-style) ───────────────────────── */
    s_user_card = lv_obj_create(s_screen);
    lv_obj_set_size(s_user_card, BUBBLE_W, LV_SIZE_CONTENT);
    lv_obj_align(s_user_card, LV_ALIGN_TOP_MID, 0, USER_BUBBLE_TOP);
    lv_obj_set_style_bg_color(s_user_card, lv_color_hex(0x07C160), 0); /* WeChat green */
    lv_obj_set_style_bg_opa(s_user_card, LV_OPA_TRANSP, 0); /* hidden until text */
    lv_obj_set_style_radius(s_user_card, 10, 0);
    lv_obj_set_style_border_width(s_user_card, 0, 0);
    lv_obj_set_style_pad_all(s_user_card, 8, 0);
    lv_obj_clear_flag(s_user_card, LV_OBJ_FLAG_SCROLLABLE);

    s_user_lbl = lv_label_create(s_user_card);
    lv_label_set_text(s_user_lbl, "");
    lv_label_set_long_mode(s_user_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_user_lbl, BUBBLE_W - 20);
    lv_obj_set_style_text_color(s_user_lbl, lv_color_hex(0xFFFFFF), 0); /* white text */
    lv_obj_set_style_text_font(s_user_lbl, g_font_cn_20, 0);  /* comprehensive CJK font */
    lv_obj_set_style_bg_opa(s_user_lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(s_user_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    /* ── Assistant text bubble card (grey) ──────────────────────────────── */
    s_bubble_card = lv_obj_create(s_screen);
    lv_obj_set_size(s_bubble_card, BUBBLE_W, LV_SIZE_CONTENT);
    lv_obj_align(s_bubble_card, LV_ALIGN_TOP_MID, 0, ASST_BUBBLE_TOP);
    lv_obj_set_style_bg_color(s_bubble_card, th->assistant_bubble, 0);
    lv_obj_set_style_bg_opa(s_bubble_card, LV_OPA_TRANSP, 0); /* hidden until text */
    lv_obj_set_style_radius(s_bubble_card, 10, 0);
    lv_obj_set_style_border_width(s_bubble_card, 0, 0);
    lv_obj_set_style_pad_all(s_bubble_card, 8, 0);
    lv_obj_clear_flag(s_bubble_card, LV_OBJ_FLAG_SCROLLABLE);

    s_bubble_lbl = lv_label_create(s_bubble_card);
    lv_label_set_text(s_bubble_lbl, "");
    lv_label_set_long_mode(s_bubble_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_bubble_lbl, BUBBLE_W - 20);
    lv_obj_set_style_text_color(s_bubble_lbl, th->text, 0);
    lv_obj_set_style_text_font(s_bubble_lbl, g_font_cn_20, 0);  /* comprehensive CJK font */
    lv_obj_set_style_bg_opa(s_bubble_lbl, LV_OPA_TRANSP, 0);

    /* State label (请说话... / 思考中...) — just above bottom edge */
    s_state_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_state_lbl, "");
    lv_obj_align(s_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_state_lbl, th->text_dim, 0);
    lv_obj_set_style_text_font(s_state_lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(s_state_lbl, LV_OPA_TRANSP, 0);

    s_initialized = true;
    ESP_LOGI(TAG, "Avatar UI ready");
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

lv_obj_t *avatar_get_screen(void) { return s_screen; }

esp_err_t avatar_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_async_call(apply_state_lvgl, (void *)(uintptr_t)state);
    return ESP_OK;
}

void avatar_show_assistant_text(const char *text)
{
    if (!s_initialized || !text) return;
    char *copy = strdup(text);
    if (copy) lv_async_call(apply_text_lvgl, copy);
}

void avatar_show_user_text(const char *text)
{
    if (!s_initialized || !text) return;
    char *copy = strdup(text);
    if (copy) lv_async_call(apply_user_text_lvgl, copy);
}

void avatar_clear_assistant_text(void)
{
    avatar_show_assistant_text("");
}

/* Status bar helpers — safe to call from any task */
esp_err_t avatar_set_connection_status(bool connected)
{
    (void)connected;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_async_call(apply_state_lvgl, (void *)(uintptr_t)APP_STATE_IDLE);
    return ESP_OK;
}

/* Legacy stubs kept for compatibility */
esp_err_t avatar_set_emotion(avatar_emotion_t emotion)                    { (void)emotion; return ESP_OK; }
esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t e, int d)     { (void)e; (void)d; return ESP_OK; }
esp_err_t avatar_show_message(const char *msg)                            { avatar_show_assistant_text(msg); return ESP_OK; }
esp_err_t avatar_clear_message(void)                                      { avatar_clear_assistant_text(); return ESP_OK; }
avatar_emotion_t avatar_get_current_emotion(void)                         { return AVATAR_IDLE; }

void avatar_reset_screen(void)
{
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen       = NULL;
        s_status_bar   = NULL;
        s_state_img    = NULL;
        s_user_card    = NULL;
        s_user_lbl     = NULL;
        s_bubble_card  = NULL;
        s_bubble_lbl   = NULL;
        s_state_lbl    = NULL;
        s_initialized  = false;
    }
}
