/*
 * chat_app.c — Push-to-talk chat app, OpenClaw/Hermes protocol.
 *
 * Wraps the recording session logic from the original app_main.c into
 * a pluggable app_t implementation.
 */

#include "chat_app.h"
#include "ws_protocol.h"
#include "audio_pipeline.h"
#include "scene_avatar.h"
#include "scene_chat.h"
#include "application.h"
#include "app_state.h"
#include "button.h"
#include "event_bus.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CHAT_APP";

/* -------------------------------------------------------------------------
 * Volatile session flags (replaced by event_bus in Phase C)
 * ------------------------------------------------------------------------- */
static volatile bool g_recording_active   = false;
static volatile bool g_button_pressed_flag = false;

/* -------------------------------------------------------------------------
 * WS protocol callbacks (server → app)
 * ------------------------------------------------------------------------- */

static void on_server_tts(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] tts_text: %s", text);
    application_set_state(APP_STATE_SPEAKING);
    scene_chat_add_message(CHAT_ROLE_ASSISTANT, text);
    audio_play_tts(text);
}

static void on_server_live2d(const ws_live2d_event_t *event, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] live2d action=%s name=%s dur=%dms",
             event->action, event->name, event->duration_ms);
    if (strcmp(event->action, "emotion") == 0) {
        avatar_emotion_t emo = emotion_id_to_avatar(event->name);
        if (event->duration_ms > 0)
            avatar_set_emotion_with_duration(emo, event->duration_ms);
        else
            avatar_set_emotion(emo);
    }
}

static void on_server_status(const ws_status_event_t *status, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] status L=%d T=%d S=%d",
             status->listening, status->thinking, status->speaking);
    if (status->speaking) {
        application_set_state(APP_STATE_SPEAKING);
        scene_chat_set_state_text("播放中...");
    } else if (status->thinking) {
        application_set_state(APP_STATE_THINKING);
        scene_chat_set_state_text("思考中...");
    } else if (status->listening) {
        application_set_state(APP_STATE_LISTENING);
        scene_chat_set_state_text("请说话...");
    } else {
        application_set_state(APP_STATE_IDLE);
        scene_chat_set_state_text(NULL);
    }
}

static void on_server_text(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS→ESP] assistant_text: %s", text);
    scene_chat_update_last_message(text);
}

/* -------------------------------------------------------------------------
 * Button callbacks (fired from boot_poll_task in button.c)
 * ------------------------------------------------------------------------- */

static void IRAM_ATTR button_press_start_cb(void *arg)
{
    (void)arg;
    g_button_pressed_flag = true;
    if (g_recording_active) return;
    if (audio_is_playing()) audio_stop_playback();
}

static void button_release_cb(bool long_press, void *arg)
{
    (void)arg;
    if (long_press) {
        /* Long press: stop recording and return to launcher */
        ESP_LOGI(TAG, "Button long-press — returning to launcher");
        g_recording_active = false;
        event_t ev = { .type = EVT_APP_SWITCH, .data.ptr = (void *)"launcher" };
        event_bus_post(&ev);
        return;
    }
    if (!g_recording_active) return;
    ESP_LOGI(TAG, "Button released — finalising recording");
    g_recording_active = false;
}

/* -------------------------------------------------------------------------
 * Recording session task (Core 0)
 * ------------------------------------------------------------------------- */

void chat_app_record_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Record task started (Core %d)", xPortGetCoreID());

    while (1) {
        /* Wait for button press */
        while (!g_button_pressed_flag) vTaskDelay(pdMS_TO_TICKS(30));
        g_button_pressed_flag = false;

        app_state_t cur = application_get_state();

        if (cur == APP_STATE_SPEAKING) {
            ESP_LOGW(TAG, "Speaking — skipping record");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (cur == APP_STATE_LISTENING || cur == APP_STATE_THINKING) {
            ws_client_send_interrupt();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGI(TAG, "Starting capture (LISTENING)");
        g_recording_active = true;
        application_set_state(APP_STATE_LISTENING);
        audio_start_capture();

        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        size_t  samples_read = 0;

        while (g_recording_active) {
            if (audio_manager_read_chunk(chunk, &samples_read, 60)) {
                if (!g_recording_active) break;
                esp_err_t ret = ws_client_send_stt_audio(chunk, samples_read, false);
                if (ret != ESP_OK) ESP_LOGW(TAG, "stt_audio send failed");
            } else {
                if (!g_recording_active) break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        ESP_LOGI(TAG, "Stopping capture");
        audio_stop_capture();

        /* Drain any remaining chunks with isLast=true */
        while (audio_manager_read_chunk(chunk, &samples_read, 20)) {
            ws_client_send_stt_audio(chunk, samples_read, true);
        }

        ESP_LOGI(TAG, "Capture done → THINKING");
        application_set_state(APP_STATE_THINKING);
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * app_t lifecycle
 * ------------------------------------------------------------------------- */

static esp_err_t chat_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Chat app enter");
    /* Initialise chat scene if not already done */
    scene_chat_init();
    lv_obj_t *scr = scene_chat_get_screen();
    if (scr) lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    scene_chat_set_state_text(NULL);
    return ESP_OK;
}

static void chat_on_exit(app_t *self)
{
    (void)self;
    g_recording_active = false;
}

static app_t s_chat_app = {
    .id       = "chat",
    .name     = "Chat",
    .on_enter = chat_on_enter,
    .on_exit  = chat_on_exit,
    .on_event = NULL,
    .on_audio_in = NULL,
    .on_tick  = NULL,
    .priv     = NULL,
};

app_t *chat_app_get(void) { return &s_chat_app; }

esp_err_t chat_app_init(void)
{
    /* Register WS protocol callbacks */
    ws_protocol_register_callbacks(
        on_server_tts,
        on_server_live2d,
        on_server_status,
        on_server_text,
        NULL
    );

    /* Register button callbacks */
    button_register_press_start_callback(button_press_start_cb, NULL);
    button_register_boot_callback(button_release_cb, NULL);

    ESP_LOGI(TAG, "Chat app initialised");
    return ESP_OK;
}
