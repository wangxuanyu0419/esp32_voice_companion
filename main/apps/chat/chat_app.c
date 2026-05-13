/*
 * chat_app.c — ClawChat tap-to-toggle voice conversation app.
 *
 * State machine:
 *   IDLE ──[tap]──▶ LISTENING ──[tap / 600 ms silence]──▶ PROCESSING
 *                                                               │
 *                         ◀── IDLE ──[turn_complete] ──── SPEAKING
 *
 * Binary audio protocol (32-byte header + PCM16-LE):
 *   magic[4] | chunk_idx(4 LE) | sample_rate(4 LE) |
 *   flags(1,bit0=isLast) | channels(1) | bits(1) | rsvd(1) | turn_id[16]
 *
 * TTS: on tts_text received → hand off to tts_player; on complete → voice_turn_complete.
 */

#include "chat_app.h"
#include "ws_protocol.h"
#include "audio_pipeline.h"
#include "tts_player.h"
#include "scene_avatar.h"
#include "application.h"
#include "app_state.h"
#include "button.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "CHAT_APP";

/* ── Chat session state ───────────────────────────────────────────────────── */
typedef enum {
    CHAT_IDLE       = 0,
    CHAT_LISTENING,
    CHAT_PROCESSING,
    CHAT_SPEAKING,
} chat_state_t;

static volatile chat_state_t s_chat_state    = CHAT_IDLE;
static uint8_t               s_turn_id[16];     /* random bytes, stable across chunks */
static volatile uint32_t     s_chunk_idx    = 0;

/* Toggle command queue — allows toggle from button ISR or avatar tap */
typedef enum { TOGGLE_CMD_TOGGLE = 1 } toggle_cmd_t;
static QueueHandle_t s_toggle_queue = NULL;

/* ── VAD parameters ───────────────────────────────────────────────────────── */
/* Each queue chunk = AUDIO_CHUNK_SAMPLES (640) @ 16 kHz = 40 ms.
 *
 * VAD_ENERGY_THRESHOLD: mean-square energy per sample.
 *   16-bit mic at room noise ≈ ±200–500 → energy ≈ 40 000–250 000.
 *   Whisper ≈ ±1000 → 1 000 000.  Tune upward if VAD fires during speech.
 *
 * VAD_MIN_CHUNKS: minimum chunks to record before silence detection starts.
 *   Prevents premature end if the user has a short pause before speaking.
 *   38 × 40 ms = 1 520 ms (≈ 1.5 s).
 *
 * VAD_SILENCE_CHUNKS: consecutive silent chunks required to auto-stop.
 *   25 × 40 ms = 1 000 ms (1 s of silence after the minimum window). */
#define VAD_ENERGY_THRESHOLD   200000UL  /* RMS² — tune via monitor logs */
#define VAD_MIN_CHUNKS         38        /* 38 × 40 ms = 1 520 ms minimum */
#define VAD_SILENCE_CHUNKS     25        /* 25 × 40 ms = 1 000 ms silence */

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static void generate_turn_id(uint8_t id[16])
{
    uint32_t *p = (uint32_t *)id;
    for (int i = 0; i < 4; i++) p[i] = esp_random();
}

static int32_t rms_energy(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++)
        sum += (int64_t)samples[i] * samples[i];
    return (int32_t)(sum / (int64_t)count);
}

static void set_state(chat_state_t st)
{
    s_chat_state = st;
    switch (st) {
        case CHAT_IDLE:       application_set_state(APP_STATE_IDLE);      break;
        case CHAT_LISTENING:  application_set_state(APP_STATE_LISTENING); break;
        case CHAT_PROCESSING: application_set_state(APP_STATE_THINKING);  break;
        case CHAT_SPEAKING:   application_set_state(APP_STATE_SPEAKING);  break;
    }
}

/* ── Recording task ───────────────────────────────────────────────────────── */
static void recording_task(void *arg)
{
    ESP_LOGI(TAG, "Recording task started (Core %d)", xPortGetCoreID());

    audio_start_capture();

    int silence_chunks = 0;
    int total_chunks   = 0;

    while (s_chat_state == CHAT_LISTENING) {
        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        size_t  count = 0;

        /* Wait up to 80 ms for a chunk; loop back if none yet */
        if (!audio_manager_read_chunk(chunk, &count, 80)) {
            /* Check if we were told to stop (toggle fired while waiting) */
            toggle_cmd_t cmd;
            if (xQueueReceive(s_toggle_queue, &cmd, 0) == pdTRUE) {
                ESP_LOGI(TAG, "Manual stop during wait");
                break;
            }
            continue;
        }

        /* Send binary chunk */
        ws_client_send_binary_chunk(chunk, count, s_chunk_idx++, s_turn_id, false);
        total_chunks++;

        /* Log energy for first 30 chunks (1.2 s) to help tune threshold */
        int32_t energy = rms_energy(chunk, count);
        if (total_chunks <= 30) {
            ESP_LOGI(TAG, "chunk #%d  energy=%" PRId32 "  (threshold=%" PRIu32 ")",
                     total_chunks, energy, (uint32_t)VAD_ENERGY_THRESHOLD);
        }

        /* VAD: energy-based silence detection — only after minimum window */
        if (total_chunks >= VAD_MIN_CHUNKS) {
            if ((uint32_t)energy < VAD_ENERGY_THRESHOLD) {
                if (++silence_chunks >= VAD_SILENCE_CHUNKS) {
                    ESP_LOGI(TAG, "VAD: 1 s silence after %d chunks → auto-finalise",
                             total_chunks);
                    break;
                }
            } else {
                silence_chunks = 0;
            }
        }

        /* Check for manual toggle (tap again) */
        toggle_cmd_t cmd;
        if (xQueueReceive(s_toggle_queue, &cmd, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Manual stop by user (total_chunks=%d)", total_chunks);
            break;
        }
    }

    /* Send final (isLast=true) chunk — empty payload is fine */
    ws_client_send_binary_chunk(NULL, 0, s_chunk_idx++, s_turn_id, true);
    ESP_LOGI(TAG, "Final chunk sent (chunk_idx=%" PRIu32 ")", s_chunk_idx - 1);

    audio_stop_capture();

    set_state(CHAT_PROCESSING);
    avatar_show_assistant_text("");   /* clear old reply */

    vTaskDelete(NULL);
}

/* ── TTS completion callback ──────────────────────────────────────────────── */
static void on_tts_complete(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TTS complete → voice_turn_complete");
    ws_client_send_voice_turn_complete();
    set_state(CHAT_IDLE);
}

/* ── WS protocol callbacks (server → app) ────────────────────────────────── */
static void on_server_tts(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] tts_text: %.60s", text);
    if (strlen(text) == 0) return;
    set_state(CHAT_SPEAKING);
    tts_player_play(text);
}

static void on_server_live2d(const ws_live2d_event_t *event, void *arg)
{
    (void)arg;
    /* Ignore Live2D events — we use static Nahida state images */
    (void)event;
}

static void on_server_status(const ws_status_event_t *status, void *arg)
{
    (void)arg;
    /* voice_turn_state supersedes this for the new flow; kept for compat */
    (void)status;
}

static void on_server_text(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] assistant_text: %.60s", text);
    avatar_show_assistant_text(text);
}

static void on_voice_ready(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] voice_session_ready");
    /* Session is ready — we already started sending audio */
}

static void on_stt_final(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] stt_final: %.60s", text);
    if (text && strlen(text) > 0) {
        /* Show what the user said as a user-side speech bubble */
        avatar_show_user_text(text);
    }
}

static void on_turn_state(const char *state, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] voice_turn_state: %s", state);

    if (strcmp(state, "listening") == 0) {
        /* Server is ready for audio again.
         * If we were PROCESSING (waiting on a response that never came — e.g.
         * empty audio, error, timeout) or SPEAKING (turn completed), reset. */
        if (s_chat_state == CHAT_PROCESSING || s_chat_state == CHAT_SPEAKING
            || s_chat_state == CHAT_IDLE) {
            set_state(CHAT_IDLE);
        }

    } else if (strcmp(state, "processing") == 0) {
        if (s_chat_state == CHAT_PROCESSING) {
            application_set_state(APP_STATE_THINKING); /* refresh display */
        }

    } else if (strcmp(state, "speaking") == 0) {
        /* Server-driven speaking state (sent before tts_text) */
        if (s_chat_state == CHAT_PROCESSING) {
            s_chat_state = CHAT_SPEAKING;
            application_set_state(APP_STATE_SPEAKING);
        }

    } else if (strcmp(state, "idle") == 0) {
        set_state(CHAT_IDLE);
    }
}

/* ── Button callbacks ─────────────────────────────────────────────────────── */
static void button_event_cb(bool long_press, void *arg)
{
    (void)arg;
    if (long_press) {
        ESP_LOGI(TAG, "Long press → launcher");
        ws_client_send_voice_session_stop();
        if (s_chat_state == CHAT_LISTENING) {
            audio_stop_capture();
            s_chat_state = CHAT_IDLE;
        }
        if (tts_player_is_playing()) tts_player_stop();
        event_t ev = { .type = EVT_APP_SWITCH, .data.ptr = (void *)"launcher" };
        event_bus_post(&ev);
    } else {
        /* Short press: same as avatar tap */
        chat_app_toggle_recording();
    }
}

/* ── Public toggle ────────────────────────────────────────────────────────── */
void chat_app_toggle_recording(void)
{
    chat_state_t cur = s_chat_state;
    ESP_LOGI(TAG, "Toggle recording (cur state=%d)", cur);

    if (cur == CHAT_IDLE) {
        /* Start a new voice turn */
        generate_turn_id(s_turn_id);
        s_chunk_idx = 0;
        set_state(CHAT_LISTENING);
        ws_client_send_voice_session_start();
        /* Drain leftover toggles */
        toggle_cmd_t dummy;
        while (xQueueReceive(s_toggle_queue, &dummy, 0) == pdTRUE) {}
        xTaskCreate(recording_task, "chat_rec", 6144, NULL, 10, NULL);

    } else if (cur == CHAT_LISTENING) {
        /* Manual stop — signal the recording task */
        toggle_cmd_t cmd = TOGGLE_CMD_TOGGLE;
        xQueueSend(s_toggle_queue, &cmd, pdMS_TO_TICKS(50));

    } else if (cur == CHAT_SPEAKING) {
        /* Interrupt TTS */
        ESP_LOGI(TAG, "Interrupting TTS");
        tts_player_stop();
        ws_client_send_interrupt();
        set_state(CHAT_IDLE);

    } else {
        /* PROCESSING: ignore; wait for server */
        ESP_LOGD(TAG, "Toggle ignored (processing)");
    }
}

/* ── app_t lifecycle ──────────────────────────────────────────────────────── */
static esp_err_t chat_on_enter(app_t *self)
{
    (void)self;
    ESP_LOGI(TAG, "Chat app enter");

    /* Ensure avatar screen is ready and load it */
    avatar_init();
    lv_obj_t *scr = avatar_get_screen();
    if (scr) {
        lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    }

    avatar_show_assistant_text("");
    s_chat_state = CHAT_IDLE;

    /* Register tap callback on the avatar image */
    avatar_set_tap_callback(chat_app_toggle_recording);

    return ESP_OK;
}

static void chat_on_exit(app_t *self)
{
    (void)self;
    if (s_chat_state == CHAT_LISTENING) {
        audio_stop_capture();
    }
    if (tts_player_is_playing()) tts_player_stop();
    s_chat_state = CHAT_IDLE;
    avatar_set_tap_callback(NULL);
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
    /* Toggle command queue */
    s_toggle_queue = xQueueCreate(4, sizeof(toggle_cmd_t));
    if (!s_toggle_queue) return ESP_ERR_NO_MEM;

    /* Init TTS player */
    tts_player_init();
    tts_player_register_complete_callback(on_tts_complete, NULL);

    /* Register WS protocol callbacks */
    ws_protocol_register_callbacks(
        on_server_tts,
        on_server_live2d,
        on_server_status,
        on_server_text,
        NULL
    );
    ws_protocol_register_voice_callbacks(
        on_voice_ready,
        on_stt_final,
        on_turn_state,
        NULL
    );

    /* Register button callbacks */
    button_register_boot_callback(button_event_cb, NULL);

    ESP_LOGI(TAG, "Chat app initialised");
    return ESP_OK;
}
