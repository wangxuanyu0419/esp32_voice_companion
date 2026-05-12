/*
 * scene_avatar.c — LVGL avatar UI (Phase B: real SH8601 + FT5x06 drivers).
 *
 * display_driver_init() must be called first (done in application_init_all).
 */

#include "scene_avatar.h"
#include "emotion_map.h"
#include "display_driver.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_status_bar.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SCENE_AVATAR";

static bool             s_initialized    = false;
static avatar_emotion_t s_current_emotion = AVATAR_IDLE;

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_status_bar   = NULL;
static lv_obj_t *s_avatar_img   = NULL;
static lv_obj_t *s_msg_label    = NULL;
static lv_obj_t *s_status_label = NULL;

static esp_timer_handle_t s_emotion_timer = NULL;

static esp_err_t load_avatar_image(avatar_emotion_t emotion)
{
    const char *path = avatar_get_image_path(emotion);
    ESP_LOGI(TAG, "Avatar: emotion=%d path=%s", emotion, path);
    return ESP_OK;
}

static void emotion_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Emotion timeout → IDLE");
    avatar_set_emotion(AVATAR_IDLE);
}

esp_err_t avatar_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Avatar UI initialising...");

    const ui_theme_t *th = ui_theme_get();

    /* display_driver_init() already called LVGL init + registered drivers. */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    s_status_bar = ui_status_bar_create(s_screen);
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());

    s_avatar_img = lv_img_create(s_screen);
    lv_obj_center(s_avatar_img);
    lv_img_set_zoom(s_avatar_img, 256);
    load_avatar_image(AVATAR_IDLE);

    s_msg_label = lv_label_create(s_screen);
    lv_label_set_text(s_msg_label, "");
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_text_color(s_msg_label, th->text, 0);
    lv_obj_set_style_text_font(s_msg_label, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(s_msg_label, LV_OPA_TRANSP, 0);

    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, "初始化中...");
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(s_status_label, th->system_text, 0);
    lv_obj_set_style_text_font(s_status_label, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_TRANSP, 0);

    esp_timer_create_args_t ta = {.callback = emotion_timer_cb,
                                  .name     = "emo_tmr"};
    esp_timer_create(&ta, &s_emotion_timer);

    s_initialized = true;
    ESP_LOGI(TAG, "Avatar UI ready (stub mode)");
    return ESP_OK;
}

esp_err_t avatar_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    const char      *status;
    avatar_emotion_t emo;

    switch (state) {
        case APP_STATE_IDLE:      status = "就绪";   emo = AVATAR_IDLE;      break;
        case APP_STATE_LISTENING: status = "请说话..."; emo = AVATAR_SPEAKING;  break;
        case APP_STATE_THINKING:  status = "思考中..."; emo = AVATAR_SURPRISED; break;
        case APP_STATE_SPEAKING:  status = "播放中..."; emo = AVATAR_SPEAKING;  break;
        case APP_STATE_ERROR:     status = "连接错误"; emo = AVATAR_SAD;       break;
        case APP_STATE_SLEEP:     status = "休眠";   emo = AVATAR_IDLE;      break;
        default:                  status = "";        emo = AVATAR_IDLE;      break;
    }

    lv_label_set_text(s_status_label, status);
    load_avatar_image(emo);
    s_current_emotion = emo;
    return ESP_OK;
}

esp_err_t avatar_set_emotion(avatar_emotion_t emotion)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_current_emotion = emotion;
    load_avatar_image(emotion);
    return ESP_OK;
}

esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t emotion, int duration_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    avatar_set_emotion(emotion);
    if (duration_ms > 0) {
        esp_timer_stop(s_emotion_timer);
        esp_timer_start_once(s_emotion_timer, (uint64_t)duration_ms * 1000);
    }
    return ESP_OK;
}

esp_err_t avatar_show_message(const char *msg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_label_set_text(s_msg_label, msg);
    return ESP_OK;
}

esp_err_t avatar_clear_message(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_label_set_text(s_msg_label, "");
    return ESP_OK;
}

esp_err_t avatar_set_connection_status(bool connected)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const ui_theme_t *th = ui_theme_get();
    lv_obj_set_style_text_color(s_status_label,
                                connected ? th->user_bubble : th->low_battery, 0);
    lv_label_set_text(s_status_label, connected ? "已连接" : "未连接");
    return ESP_OK;
}

avatar_emotion_t avatar_get_current_emotion(void) { return s_current_emotion; }

lv_obj_t *avatar_get_screen(void) { return s_screen; }

void avatar_reset_screen(void)
{
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen       = NULL;
        s_status_bar   = NULL;
        s_avatar_img   = NULL;
        s_msg_label    = NULL;
        s_status_label = NULL;
        s_initialized  = false;
    }
}
