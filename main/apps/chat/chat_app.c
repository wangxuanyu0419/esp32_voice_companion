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
#include "pcm_stream_player.h"
#include "tts_player.h"
#include "remote_image.h"
#include "scene_avatar.h"
#include "settings_app.h"
#include "config_store.h"
#include "vad.h"
#include "application.h"
#include "app_state.h"
#include "button.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "esp_heap_caps.h"
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
static volatile bool         s_ws_pcm_active = false;
static volatile bool         s_http_voice_reply = false;
static volatile bool         s_session_active = false;
static volatile bool         s_recording_active = false;

/*
 * Half-duplex voice gate.
 *
 * The WS receive task and the recorder run concurrently.  A plain state
 * check in recording_task is not sufficient: a player callback can arrive
 * between that check and the binary send.  The gate is set before any player
 * is queued, capture is stopped while it is held, and the upload itself is
 * serialized with the gate transition by s_voice_io_mutex.  We deliberately
 * keep the gate held until the server says "listening" again when a voice
 * session is active; this prevents VAD from re-arming in the gap between
 * audio completion and the next server turn.
 */
static volatile bool         s_playback_gate = false;
static volatile bool         s_waiting_server_listening = false;
static volatile bool         s_audio_broadcast_active = false;
static volatile bool         s_settings_open = false;
static SemaphoreHandle_t     s_voice_io_mutex = NULL;
static portMUX_TYPE          s_voice_gate_mux = portMUX_INITIALIZER_UNLOCKED;

static bool playback_gate_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_voice_gate_mux);
    active = s_playback_gate;
    portEXIT_CRITICAL(&s_voice_gate_mux);
    return active;
}

static void playback_gate_begin(const char *source)
{
    bool was_active;
    portENTER_CRITICAL(&s_voice_gate_mux);
    was_active = s_playback_gate;
    s_playback_gate = true;
    s_waiting_server_listening = false;
    portEXIT_CRITICAL(&s_voice_gate_mux);

    if (!was_active) {
        ESP_LOGI(TAG, "Half-duplex: gate ON before %s; stopping capture/VAD",
                 source ? source : "playback");
    }

    /* Serialize the gate transition with the final recorder upload.  Do not
     * hold the critical section across the driver semaphore or I2S calls. */
    bool capture_before = audio_is_capturing();
    if (s_voice_io_mutex) xSemaphoreTake(s_voice_io_mutex, portMAX_DELAY);
    audio_stop_capture();
    if (s_voice_io_mutex) xSemaphoreGive(s_voice_io_mutex);
    ESP_LOGI(TAG, "Half-duplex: capture_before=%d capture_after=%d",
             capture_before ? 1 : 0, audio_is_capturing() ? 1 : 0);
}

static void playback_gate_finish(const char *reason)
{
    bool hold_for_session = s_session_active;
    if (hold_for_session) {
        portENTER_CRITICAL(&s_voice_gate_mux);
        s_waiting_server_listening = true;
        portEXIT_CRITICAL(&s_voice_gate_mux);
        ESP_LOGI(TAG, "Half-duplex: playback ended (%s); waiting for server listening",
                 reason ? reason : "complete");
        return;
    }

    portENTER_CRITICAL(&s_voice_gate_mux);
    s_playback_gate = false;
    s_waiting_server_listening = false;
    portEXIT_CRITICAL(&s_voice_gate_mux);
    ESP_LOGI(TAG, "Half-duplex: gate OFF (%s)", reason ? reason : "complete");
}

static void playback_gate_release_for_settings(void)
{
    portENTER_CRITICAL(&s_voice_gate_mux);
    s_playback_gate = false;
    s_waiting_server_listening = false;
    portEXIT_CRITICAL(&s_voice_gate_mux);
    ESP_LOGI(TAG, "Half-duplex: gate OFF (settings/exit)");
}

/* Toggle command queue — allows toggle from button ISR or avatar tap */
typedef enum { TOGGLE_CMD_TOGGLE = 1 } toggle_cmd_t;
static QueueHandle_t s_toggle_queue = NULL;

/* ── VAD parameters ───────────────────────────────────────────────────────── */
/* Each queue chunk = AUDIO_CHUNK_SAMPLES (640) @ 16 kHz = 40 ms.
 *
 * Android's press-to-talk flow does not submit a turn until speech has been
 * confirmed.  It keeps a bounded pre-roll, then expires the quiet window and
 * returns to idle without sending an empty/fabricated turn. */
#define VAD_MIN_CHUNKS         38        /* 38 × 40 ms = 1 520 ms minimum */
#define VAD_SILENCE_CHUNKS     25        /* 25 × 40 ms = 1 000 ms silence */
#define VAD_NO_SPEECH_CHUNKS   125       /* 125 × 40 ms = Android's 5 s window */
#define VAD_PRE_ROLL_CHUNKS    25        /* 1 s leading context, like Android */

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static void generate_turn_id(uint8_t id[16])
{
    uint32_t *p = (uint32_t *)id;
    for (int i = 0; i < 4; i++) p[i] = esp_random();
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
    (void)arg;
    ESP_LOGI(TAG, "Recording task started (Core %d)", xPortGetCoreID());
    s_recording_active = true;

    if (playback_gate_active()) {
        ESP_LOGI(TAG, "Recording task suppressed by half-duplex playback gate");
        s_recording_active = false;
        vTaskDelete(NULL);
        return;
    }

    audio_start_capture();

    if (playback_gate_active()) {
        audio_stop_capture();
        ESP_LOGI(TAG, "Capture stopped before first voice chunk (playback started)");
        s_recording_active = false;
        vTaskDelete(NULL);
        return;
    }

    int silence_chunks = 0;
    int total_chunks   = 0;
    bool speech_started = false;
    bool playback_interrupted = false;

    /* Keep the Android-sized leading context off the recorder task stack.
     * PSRAM is preferred; the small internal-heap fallback keeps the feature
     * usable on boards without PSRAM while remaining bounded at 32 KiB. */
    const size_t pre_roll_bytes = (size_t)VAD_PRE_ROLL_CHUNKS *
                                  AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
    int16_t *pre_roll = heap_caps_malloc(pre_roll_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pre_roll) {
        pre_roll = heap_caps_malloc(pre_roll_bytes, MALLOC_CAP_8BIT);
    }
    if (!pre_roll) {
        ESP_LOGW(TAG, "VAD pre-roll allocation failed; speech will start without leading context");
    }
    size_t pre_roll_count = 0;
    size_t pre_roll_next = 0;
    size_t pre_roll_sizes[VAD_PRE_ROLL_CHUNKS] = {0};

    while (s_chat_state == CHAT_LISTENING) {
        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        size_t  count = 0;

        if (playback_gate_active()) {
            playback_interrupted = true;
            break;
        }

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

        total_chunks++;

        /* VAD runs before network submission.  Android keeps these frames
         * locally until two speech frames confirm an onset; do the same with
         * the bounded ring below so a quiet window never becomes an STT turn. */
        vad_state_t vad_state = vad_process_frame(chunk, count);
        int32_t energy = vad_get_energy();
        int32_t threshold = vad_get_effective_threshold();
        if (total_chunks <= 30 || total_chunks == VAD_NO_SPEECH_CHUNKS) {
            ESP_LOGI(TAG, "chunk #%d energy=%" PRId32 " threshold=%" PRId32,
                     total_chunks, energy, threshold);
        }

        bool chunk_sent = false;
        if (!speech_started) {
            if (pre_roll) {
                size_t slot = pre_roll_next;
                size_t copy_count = count > AUDIO_CHUNK_SAMPLES
                                  ? AUDIO_CHUNK_SAMPLES : count;
                memcpy(pre_roll + slot * AUDIO_CHUNK_SAMPLES,
                       chunk, copy_count * sizeof(int16_t));
                pre_roll_sizes[slot] = copy_count;
                if (pre_roll_count < VAD_PRE_ROLL_CHUNKS) pre_roll_count++;
                pre_roll_next = (pre_roll_next + 1) % VAD_PRE_ROLL_CHUNKS;
            }

            if (vad_state == VAD_STATE_SPEECH) {
                speech_started = true;
                ESP_LOGI(TAG, "VAD: speech confirmed after %d chunks, pre_roll=%u",
                         total_chunks, (unsigned)pre_roll_count);

                /* Flush oldest → newest so STT receives the same leading
                 * context that Android retains around its speech onset. */
                if (pre_roll) {
                    size_t first = pre_roll_count == VAD_PRE_ROLL_CHUNKS
                                 ? pre_roll_next : 0;
                    for (size_t i = 0; i < pre_roll_count; i++) {
                        size_t slot = (first + i) % VAD_PRE_ROLL_CHUNKS;
                        if (s_voice_io_mutex) {
                            xSemaphoreTake(s_voice_io_mutex, portMAX_DELAY);
                        }
                        bool can_send = !playback_gate_active();
                        if (can_send) {
                            ws_client_send_binary_chunk(
                                pre_roll + slot * AUDIO_CHUNK_SAMPLES,
                                pre_roll_sizes[slot], s_chunk_idx++,
                                s_turn_id, false);
                        }
                        if (s_voice_io_mutex) xSemaphoreGive(s_voice_io_mutex);
                        if (!can_send) {
                            playback_interrupted = true;
                            break;
                        }
                    }
                    chunk_sent = !playback_interrupted;
                }
            }

            if (!speech_started) {
                /* This is the Android quiet-window expiry: close the session
                 * without an isLast frame, which prevents a [沉默] STT turn
                 * and prevents the server from re-arming the next turn. */
                if (total_chunks >= VAD_NO_SPEECH_CHUNKS) {
                    ESP_LOGI(TAG, "VAD: no speech after %d chunks (%d ms) -> idle",
                             total_chunks, total_chunks * 40);
                    break;
                }
            }
        }

        if (speech_started && !chunk_sent && !playback_interrupted) {
            /* If pre-roll was unavailable, or this is a normal post-onset
             * frame, submit the current chunk now. */
            if (s_voice_io_mutex) xSemaphoreTake(s_voice_io_mutex, portMAX_DELAY);
            bool can_send = !playback_gate_active();
            if (can_send) {
                ws_client_send_binary_chunk(chunk, count, s_chunk_idx++,
                                             s_turn_id, false);
            }
            if (s_voice_io_mutex) xSemaphoreGive(s_voice_io_mutex);
            if (!can_send) playback_interrupted = true;
        }

        if (playback_interrupted) break;

        /* VAD: energy-based silence detection — only after the same minimum
         * recording window used by the previous firmware. */
        if (speech_started && total_chunks >= VAD_MIN_CHUNKS) {
            if (energy < threshold) {
                if (++silence_chunks >= VAD_SILENCE_CHUNKS) {
                    ESP_LOGI(TAG, "VAD: 1 s silence after %d chunks -> auto-finalise",
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

    /* If playback took over while the microphone was waiting for speech,
     * preserve the existing half-duplex handoff.  The player owns the gate;
     * this recorder must not close the voice session underneath it. */
    if (playback_interrupted || playback_gate_active()) {
        audio_stop_capture();
        s_recording_active = false;
        if (pre_roll) heap_caps_free(pre_roll);
        ESP_LOGI(TAG, "Recording task stopped for playback before speech");
        vTaskDelete(NULL);
        return;
    }

    /* Do not send a synthetic final frame when no speech was confirmed.  This
     * is the key distinction from the old implementation: an all-quiet VAD
     * window is a cancelled session, not an STT turn containing silence. */
    if (!speech_started) {
        audio_stop_capture();
        s_recording_active = false;
        if (pre_roll) heap_caps_free(pre_roll);
        if (s_session_active) {
            s_session_active = false;
            ws_client_send_voice_session_idle("timeout");
            ws_client_send_voice_session_stop();
        }
        ESP_LOGI(TAG, "Idle: reason=%s chunks=%d",
                 total_chunks >= VAD_NO_SPEECH_CHUNKS ? "no-speech-timeout"
                                                       : "manual-stop-before-speech",
                 total_chunks);
        set_state(CHAT_IDLE);
        vTaskDelete(NULL);
        return;
    }

    /* Do not send a synthetic final frame after playback has taken over.  A
     * stale final frame would reopen the server turn while the device speaks. */
    if (!playback_interrupted && !playback_gate_active()) {
        if (s_voice_io_mutex) xSemaphoreTake(s_voice_io_mutex, portMAX_DELAY);
        bool can_finalize = !playback_gate_active();
        if (can_finalize) {
            ws_client_send_binary_chunk(NULL, 0, s_chunk_idx++, s_turn_id, true);
            ESP_LOGI(TAG, "Final chunk sent (chunk_idx=%" PRIu32 ")", s_chunk_idx - 1);
        }
        if (s_voice_io_mutex) xSemaphoreGive(s_voice_io_mutex);
        playback_interrupted = !can_finalize;
    } else {
        ESP_LOGI(TAG, "Final chunk suppressed by half-duplex gate");
    }

    audio_stop_capture();
    s_recording_active = false;
    if (pre_roll) heap_caps_free(pre_roll);

    if (playback_interrupted || playback_gate_active()) {
        ESP_LOGI(TAG, "Recording task stopped for playback (chunks=%d)", total_chunks);
        vTaskDelete(NULL);
        return;
    }

    set_state(CHAT_PROCESSING);
    avatar_show_assistant_text("");   /* clear old reply */

    vTaskDelete(NULL);
}

static void start_recording_turn(void)
{
    if (!s_session_active || s_recording_active ||
        s_chat_state == CHAT_LISTENING || !ws_client_is_ready()) return;
    if (playback_gate_active() || tts_player_is_playing() ||
        pcm_stream_player_is_playing()) {
        ESP_LOGI(TAG, "Recording start deferred by half-duplex playback gate");
        return;
    }
    generate_turn_id(s_turn_id);
    s_chunk_idx = 0;
    vad_reset();
    set_state(CHAT_LISTENING);
    ESP_LOGI(TAG, "Listen start: vad_threshold=%" PRId32,
             vad_get_effective_threshold());
    s_recording_active = true;
    if (xTaskCreate(recording_task, "chat_rec", 6144, NULL, 10, NULL) != pdPASS) {
        s_recording_active = false;
        set_state(CHAT_IDLE);
        ESP_LOGE(TAG, "Failed to start continuous recording task");
        ESP_LOGI(TAG, "Idle: reason=task-create-failed");
    }
}

/* ── TTS completion callback ──────────────────────────────────────────────── */
static void on_tts_complete(void *arg)
{
    (void)arg;
    bool voice_reply = s_http_voice_reply;
    bool audio_broadcast = s_audio_broadcast_active;
    s_http_voice_reply = false;
    s_audio_broadcast_active = false;
    if (voice_reply) {
        ESP_LOGI(TAG, "HTTP TTS complete → voice_turn_complete");
        ws_client_send_voice_turn_complete();
    }
    if (s_chat_state == CHAT_PROCESSING || s_chat_state == CHAT_SPEAKING)
        set_state(CHAT_IDLE);
    ESP_LOGI(TAG, "Half-duplex: HTTP playback complete capture=%d pcm=%d",
             audio_is_capturing() ? 1 : 0,
             pcm_stream_player_is_playing() ? 1 : 0);
    playback_gate_finish(audio_broadcast ? "audio-broadcast complete" :
                         "HTTP TTS complete");
}

static void on_tts_started(void *arg)
{
    (void)arg;
    if (s_chat_state == CHAT_PROCESSING) {
        ESP_LOGI(TAG, "Playback started after first TTS PCM block");
        set_state(CHAT_SPEAKING);
    }
}

static void on_pcm_started(void *arg)
{
    (void)arg;
    if (s_chat_state == CHAT_PROCESSING) {
        ESP_LOGI(TAG, "Playback started after first WS PCM block");
        set_state(CHAT_SPEAKING);
    }
}

static void on_pcm_complete(bool ok, void *arg)
{
    (void)arg;
    if (!s_ws_pcm_active) return;
    s_ws_pcm_active = false;
    if (ok) {
        ESP_LOGI(TAG, "WS PCM complete (ok) → voice_turn_complete");
        ws_client_send_voice_turn_complete();
    } else {
        /* A dropped/corrupt stream is not a successful turn.  Do not tell the
         * server that audio playback completed after the player aborted. */
        ESP_LOGW(TAG, "WS PCM aborted; suppressing voice_turn_complete");
    }
    set_state(CHAT_IDLE);
    ESP_LOGI(TAG, "Half-duplex: WS PCM complete ok=%d capture=%d tts=%d",
             ok ? 1 : 0, audio_is_capturing() ? 1 : 0,
             tts_player_is_playing() ? 1 : 0);
    playback_gate_finish(ok ? "WS PCM complete" : "WS PCM aborted");
}

/* ── WS protocol callbacks (server → app) ────────────────────────────────── */
static void on_server_tts(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] tts_text: %.60s", text);
    if (!text || strlen(text) == 0 || s_ws_pcm_active) return;

    /* Gate capture before queuing HTTP TTS.  If this is another sentence in
     * the same HTTP stream, the existing player is allowed to drain its
     * bounded queue; a different player is stopped first. */
    playback_gate_begin("HTTP TTS");
    if (s_audio_broadcast_active) {
        s_audio_broadcast_active = false;
        if (tts_player_is_playing()) tts_player_stop();
    }
    s_http_voice_reply = true;
    set_state(CHAT_PROCESSING);
    if (tts_player_play(text) != ESP_OK) {
        s_http_voice_reply = false;
        playback_gate_finish("HTTP TTS queue failure");
        ws_client_send_voice_turn_complete();
        set_state(CHAT_IDLE);
    }
}

static void on_server_live2d(const ws_live2d_event_t *event, void *arg)
{
    (void)arg;
    if (!event || !event->name || !*event->name) return;
    avatar_emotion_t emotion = emotion_id_to_avatar(event->name);
    avatar_emotion_t return_emotion = emotion_id_to_avatar(event->return_to);
    int duration = event->duration_ms > 0 ? event->duration_ms :
                   emotion_get_duration(g_emotion_configs[emotion].level);
    /* Unknown emotion IDs deliberately map to AVATAR_IDLE in
     * emotion_id_to_avatar; no unbounded asset/path lookup is performed. */
    avatar_set_emotion_with_return(emotion, duration, return_emotion);
    ESP_LOGI(TAG, "[WS] avatar emotion=%s mapped=%d return=%d duration=%d",
             event->name, emotion, return_emotion, duration);
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

static void on_server_broadcast(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] broadcast: %.80s", text ? text : "");
    if (text && *text) avatar_show_assistant_text(text);
}

static void on_server_agent_ack(const char *text, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[WS] agent_ack: %.80s", text ? text : "");
    if (text && *text) avatar_show_assistant_text(text);
}

static void on_server_audio_broadcast(const char *stream_id,
                                      const char *message, void *arg)
{
    (void)arg;
    UBaseType_t ws_stack_before = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "[WS] audio_broadcast: stream=%s message=%.60s",
             stream_id ? stream_id : "", message ? message : "");
    if (message && *message) avatar_show_assistant_text(message);
    if (stream_id && *stream_id) {
        playback_gate_begin("audio-broadcast");
        if (s_ws_pcm_active) {
            s_ws_pcm_active = false;
            pcm_stream_player_stop();
        }
        if (tts_player_is_playing()) tts_player_stop();
        s_http_voice_reply = false;
        s_audio_broadcast_active = true;
        set_state(CHAT_PROCESSING);
        esp_err_t ret = tts_player_play_audio_stream(stream_id);
        ESP_LOGI(TAG, "[WS] audio_broadcast enqueue ret=%s ws_stack_free=%u",
                 esp_err_to_name(ret), (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (ret != ESP_OK) {
            s_audio_broadcast_active = false;
            playback_gate_finish("audio-broadcast queue failure");
            set_state(CHAT_IDLE);
            ESP_LOGW(TAG, "[WS] audio_broadcast queue failed: %s",
                     esp_err_to_name(ret));
        }
    }
    ESP_LOGI(TAG, "[WS] audio_broadcast callback done ws_stack_before=%u ws_stack_free=%u",
             (unsigned)ws_stack_before,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void on_server_tts_audio_start(const char *stream_id, int sample_rate,
                                      int channels, const char *encoding, void *arg)
{
    (void)arg;
    if (!stream_id || !*stream_id || !encoding ||
        strcmp(encoding, "pcm_s16le") != 0) {
        ESP_LOGW(TAG, "Ignoring unsupported WS TTS encoding: %s",
                 encoding ? encoding : "(null)");
        return;
    }
    playback_gate_begin("WS PCM");
    tts_player_stop();
    s_http_voice_reply = false;
    s_audio_broadcast_active = false;
    if (s_ws_pcm_active) s_ws_pcm_active = false;
    pcm_stream_player_stop();
    s_ws_pcm_active = (pcm_stream_player_start(stream_id, sample_rate, channels) == ESP_OK);
    if (s_ws_pcm_active) {
        ESP_LOGI(TAG, "WS PCM TTS start id=%s rate=%d channels=%d",
                 stream_id, sample_rate, channels);
        set_state(CHAT_PROCESSING);
    } else {
        playback_gate_finish("WS PCM start failure");
    }
}

static void on_server_tts_audio_chunk(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (s_ws_pcm_active) {
        esp_err_t ret = pcm_stream_player_push(data, len);
        if (ret != ESP_OK) ESP_LOGW(TAG, "WS PCM chunk dropped: %s", esp_err_to_name(ret));
    }
}

static void on_server_tts_audio_end(const char *stream_id, void *arg)
{
    (void)arg;
    if (s_ws_pcm_active) {
        esp_err_t ret = pcm_stream_player_end(stream_id);
        if (ret != ESP_OK) ESP_LOGW(TAG, "WS PCM end failed: %s", esp_err_to_name(ret));
    }
}

static void on_server_error(const char *message, void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "[WS] server error: %.80s", message ? message : "");
}

static void on_server_image(const char *url, const char *caption, void *arg)
{
    (void)arg;
    esp_err_t ret = remote_image_fetch(url, caption);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[WS] image rejected: %s", esp_err_to_name(ret));
        avatar_show_assistant_text("Image unavailable");
    }
}

static void open_settings(void)
{
    ESP_LOGI(TAG, "Opening integrated settings page");
    /* Settings is a modal view over the same ClawChat session.  Keep capture,
     * pending server turns, and playback alive; only the visible root scene
     * is hidden while the page is foreground. */
    s_settings_open = true;
    ESP_LOGI(TAG, "Settings overlay keeps chat state=%d session=%d playback=%d",
             s_chat_state, s_session_active ? 1 : 0,
             (tts_player_is_playing() || pcm_stream_player_is_playing()) ? 1 : 0);
    settings_app_open();
}

static app_state_t app_state_for_chat_state(chat_state_t state)
{
    switch (state) {
        case CHAT_LISTENING:  return APP_STATE_LISTENING;
        case CHAT_PROCESSING: return APP_STATE_THINKING;
        case CHAT_SPEAKING:  return APP_STATE_SPEAKING;
        case CHAT_IDLE:
        default:             return APP_STATE_IDLE;
    }
}

void chat_app_settings_closed(void)
{
    s_settings_open = false;
    /* Repaint the root from the live chat model.  This intentionally does not
     * force IDLE: a turn may have progressed from listening to processing or
     * speaking while settings was foreground. */
    app_state_t state = app_state_for_chat_state(s_chat_state);
    application_set_state(state);
    avatar_set_state(state);
    ESP_LOGI(TAG, "Settings page closed -> ClawChat state=%d session=%d playback=%d",
             s_chat_state, s_session_active ? 1 : 0,
             (tts_player_is_playing() || pcm_stream_player_is_playing()) ? 1 : 0);
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
        if (s_chat_state == CHAT_PROCESSING || s_chat_state == CHAT_SPEAKING ||
            s_chat_state == CHAT_IDLE) set_state(CHAT_IDLE);
        if (playback_gate_active() && !tts_player_is_playing() &&
            !pcm_stream_player_is_playing() && !s_ws_pcm_active) {
            /* Completion callbacks normally leave the gate held until this
             * explicit server state.  This is the only automatic re-arm. */
            portENTER_CRITICAL(&s_voice_gate_mux);
            s_playback_gate = false;
            s_waiting_server_listening = false;
            portEXIT_CRITICAL(&s_voice_gate_mux);
            ESP_LOGI(TAG, "Half-duplex: gate OFF (server listening) capture=%d",
                     audio_is_capturing() ? 1 : 0);
        }
        /* Keep the session half-duplex: a completed TTS/PCM turn arms VAD
         * again only after the server explicitly returns to listening. */
        if (s_session_active && !s_recording_active &&
            !playback_gate_active() && !tts_player_is_playing() &&
            !pcm_stream_player_is_playing() && !s_ws_pcm_active) {
            start_recording_turn();
        }

    } else if (strcmp(state, "processing") == 0) {
        if (s_chat_state == CHAT_PROCESSING) {
            application_set_state(APP_STATE_THINKING); /* refresh display */
        }

    } else if (strcmp(state, "speaking") == 0) {
        /* The server announces speaking before the first audio bytes. Keep
         * the UI in waiting/thinking until a player reports its first I2S
         * block, so a stalled/empty response is not shown as playing. */
        if (s_chat_state == CHAT_PROCESSING) {
            application_set_state(APP_STATE_THINKING);
            ESP_LOGI(TAG, "Server speaking state received; waiting for audio");
        }

    } else if (strcmp(state, "idle") == 0) {
        set_state(CHAT_IDLE);
    }
}

/* ── Button callbacks ─────────────────────────────────────────────────────── */
static void button_event_cb(bool long_press, void *arg)
{
    (void)arg;

    /* boot_poll and the FreeRTOS timer task have deliberately small stacks.
     * Defer the real session/WS/audio work to main.c's existing event-owner
     * task instead of calling into it synchronously from either callback. */
    event_t ev = {
        .type = long_press ? EVT_BUTTON_LONG_PRESS : EVT_BUTTON_RELEASE,
        .data.ival = long_press ? 1 : 0,
    };
    if (event_bus_post(&ev) != ESP_OK) {
        ESP_LOGW(TAG, "Button event dropped (event bus full, long=%d)",
                 long_press ? 1 : 0);
    }
}

void chat_app_handle_button_event(bool long_press)
{
    if (long_press) {
        ESP_LOGI(TAG, "Long press → ClawChat root");
        s_session_active = false;
        ws_client_send_voice_session_stop();
        if (s_chat_state == CHAT_LISTENING) {
            audio_stop_capture();
            s_chat_state = CHAT_IDLE;
        }
        s_recording_active = false;
        pcm_stream_player_stop();
        s_ws_pcm_active = false;
        s_http_voice_reply = false;
        if (tts_player_is_playing()) tts_player_stop();
        playback_gate_release_for_settings();
        event_t ev = { .type = EVT_APP_SWITCH, .data.ptr = (void *)"chat" };
        event_bus_post(&ev);
    } else {
        /* Short press: same as avatar tap */
        chat_app_toggle_recording();
    }
}

/*
 * Stop whatever audio is currently playing (HTTP TTS reply, WS PCM reply, or
 * a server-pushed audio broadcast) and return to idle.  Shared by both tap
 * paths that mean "stop the sound": an explicit interrupt from CHAT_SPEAKING,
 * and a tap that lands while s_chat_state has already fallen back to
 * CHAT_IDLE even though a player is still running (this happens for
 * s_audio_broadcast_active, since a broadcast is not tracked by the server's
 * voice_turn_state machine and can outlive it locally).  Factored into one
 * helper so the two call sites cannot drift apart, and so every flag/gate
 * cleared here stays in sync.
 *
 * A tap here NEVER auto-starts recording — it only stops the sound, matching
 * the pre-existing CHAT_SPEAKING behavior.  A second tap starts talking.
 */
static void stop_playback_for_interrupt(const char *log_source)
{
    bool was_http_reply = s_http_voice_reply;
    bool was_ws_pcm     = s_ws_pcm_active;
    bool was_broadcast  = s_audio_broadcast_active;

    ESP_LOGI(TAG, "Interrupt (%s): stopping playback http_reply=%d ws_pcm=%d "
             "broadcast=%d tts_playing=%d pcm_playing=%d",
             log_source, was_http_reply ? 1 : 0, was_ws_pcm ? 1 : 0,
             was_broadcast ? 1 : 0, tts_player_is_playing() ? 1 : 0,
             pcm_stream_player_is_playing() ? 1 : 0);

    playback_gate_begin("user interrupt");
    tts_player_stop();
    pcm_stream_player_stop();
    s_ws_pcm_active = false;
    s_http_voice_reply = false;
    s_audio_broadcast_active = false;

    /* A broadcast is a server push, not an in-flight reply the server is
     * generating for us — there is no turn on the server side to abort.
     * Sending "interrupt" here has nothing to attach to and could be
     * misread as cancelling an unrelated, actually-in-flight reply.  Only
     * tell the server when we are actually cutting off a conversational
     * reply it was streaming to us (HTTP TTS text or WS PCM audio). */
    if (was_http_reply || was_ws_pcm) {
        ws_client_send_interrupt();
    }

    set_state(CHAT_IDLE);
}

/* ── Public toggle ────────────────────────────────────────────────────────── */
void chat_app_toggle_recording(void)
{
    chat_state_t cur = s_chat_state;
    ESP_LOGI(TAG, "Toggle recording (cur state=%d)", cur);

    if (cur == CHAT_IDLE) {
        if (playback_gate_active() || tts_player_is_playing() ||
            pcm_stream_player_is_playing()) {
            /* State already fell back to idle (e.g. a broadcast is not part
             * of the server's turn machine) but audio is still audibly
             * playing.  A tap here must stop it, not be swallowed. */
            stop_playback_for_interrupt("idle-but-playing");
            return;
        }
        /* Start or resume a continuous voice session. */
        if (!s_session_active) {
            s_session_active = true;
            ws_client_send_voice_session_start();
        }
        /* Drain leftover toggles */
        toggle_cmd_t dummy;
        while (xQueueReceive(s_toggle_queue, &dummy, 0) == pdTRUE) {}
        start_recording_turn();

    } else if (cur == CHAT_LISTENING) {
        /* Manual stop — signal the recording task */
        toggle_cmd_t cmd = TOGGLE_CMD_TOGGLE;
        xQueueSend(s_toggle_queue, &cmd, pdMS_TO_TICKS(50));

    } else if (cur == CHAT_SPEAKING) {
        stop_playback_for_interrupt("speaking");

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
    s_session_active = false;
    s_settings_open = false;
    playback_gate_release_for_settings();

    /* Register tap callback on the avatar image */
    avatar_set_tap_callback(chat_app_toggle_recording);
    avatar_set_settings_callback(open_settings);

    return ESP_OK;
}

static void chat_on_exit(app_t *self)
{
    (void)self;
    if (s_chat_state == CHAT_LISTENING) {
        audio_stop_capture();
    }
    pcm_stream_player_stop();
    s_ws_pcm_active = false;
    s_http_voice_reply = false;
    s_session_active = false;
    s_settings_open = false;
    s_recording_active = false;
    if (tts_player_is_playing()) tts_player_stop();
    playback_gate_release_for_settings();
    s_chat_state = CHAT_IDLE;
    avatar_set_tap_callback(NULL);
    avatar_set_settings_callback(NULL);
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
    app_config_t vad_cfg = {0};
    if (config_get(&vad_cfg) == ESP_OK) {
        vad_init();
        vad_configure(vad_cfg.vad_threshold, vad_cfg.vad_auto_threshold);
    } else {
        vad_init();
    }

    s_voice_io_mutex = xSemaphoreCreateMutex();
    if (!s_voice_io_mutex) return ESP_ERR_NO_MEM;

    /* Toggle command queue */
    s_toggle_queue = xQueueCreate(4, sizeof(toggle_cmd_t));
    if (!s_toggle_queue) return ESP_ERR_NO_MEM;

    /* Init both server audio paths.  They share B's audio_write_pcm/I2S
     * implementation; only the bounded PCM stream queue is new. */
    tts_player_init();
    esp_err_t pcm_ret = pcm_stream_player_init();
    if (pcm_ret != ESP_OK) return pcm_ret;
    esp_err_t image_ret = remote_image_init();
    if (image_ret != ESP_OK) return image_ret;
    tts_player_register_complete_callback(on_tts_complete, NULL);
    tts_player_register_start_callback(on_tts_started, NULL);
    pcm_stream_player_register_complete_callback(on_pcm_complete, NULL);
    pcm_stream_player_register_start_callback(on_pcm_started, NULL);
    settings_app_set_close_callback(chat_app_settings_closed);

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
    ws_extended_callbacks_t extended = {
        .on_broadcast = on_server_broadcast,
        .on_agent_ack = on_server_agent_ack,
        .on_audio_broadcast = on_server_audio_broadcast,
        .on_tts_audio_start = on_server_tts_audio_start,
        .on_tts_audio_chunk = on_server_tts_audio_chunk,
        .on_tts_audio_end = on_server_tts_audio_end,
        .on_image = on_server_image,
        .on_error = on_server_error,
    };
    ws_protocol_register_extended_callbacks(&extended, NULL);

    /* Register button callbacks */
    button_register_boot_callback(button_event_cb, NULL);

    ESP_LOGI(TAG, "Chat app initialised");
    return ESP_OK;
}
