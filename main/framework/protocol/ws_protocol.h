#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "emotion_map.h"
#include "protocol.h"

/*
 * WebSocket transport + ClawChat protocol in one compilation unit.
 * Implements protocol_t and also exposes legacy ws_client_* helpers so that
 * existing call-sites compile without modification during the Phase-A
 * restructure.
 */

/* Live2D event (matches ClawChat server JSON) */
typedef struct {
    const char *action;     /* "emotion" / "expression" / "motion" */
    const char *name;
    int         duration_ms;
    const char *return_to;
} ws_live2d_event_t;

/* Status event */
typedef struct {
    bool listening;
    bool thinking;
    bool speaking;
} ws_status_event_t;

/* ---- Inbound callback types -------------------------------------------- */
typedef void (*ws_on_tts_cb_t)(const char *text, void *arg);
typedef void (*ws_on_live2d_cb_t)(const ws_live2d_event_t *event, void *arg);
typedef void (*ws_on_status_cb_t)(const ws_status_event_t *status, void *arg);
typedef void (*ws_on_text_cb_t)(const char *text, void *arg);

/* New voice-flow callbacks */
typedef void (*ws_on_voice_ready_cb_t)(void *arg);
typedef void (*ws_on_stt_final_cb_t)(const char *text, void *arg);
typedef void (*ws_on_turn_state_cb_t)(const char *state, void *arg);

/* Protocol events that are not part of the original voice callback set.
 * Keep these callbacks transport-only so the existing avatar/display and
 * audio implementations remain the source of truth. */
typedef struct {
    void (*on_broadcast)(const char *text, void *arg);
    void (*on_agent_ack)(const char *text, void *arg);
    void (*on_audio_broadcast)(const char *stream_id, const char *message, void *arg);
    void (*on_tts_audio_start)(const char *stream_id, int sample_rate,
                               int channels, const char *encoding, void *arg);
    void (*on_tts_audio_chunk)(const uint8_t *data, size_t len, void *arg);
    void (*on_tts_audio_end)(const char *stream_id, void *arg);
    void (*on_image)(const char *url, const char *caption, void *arg);
    void (*on_error)(const char *message, void *arg);
} ws_extended_callbacks_t;

void ws_protocol_register_callbacks(ws_on_tts_cb_t tts_cb,
                                    ws_on_live2d_cb_t live2d_cb,
                                    ws_on_status_cb_t status_cb,
                                    ws_on_text_cb_t text_cb,
                                    void *arg);

/* Register extra voice-flow callbacks (call after ws_protocol_register_callbacks). */
void ws_protocol_register_voice_callbacks(ws_on_voice_ready_cb_t voice_ready_cb,
                                          ws_on_stt_final_cb_t stt_final_cb,
                                          ws_on_turn_state_cb_t turn_state_cb,
                                          void *arg);

void ws_protocol_register_extended_callbacks(const ws_extended_callbacks_t *callbacks,
                                              void *arg);

/* Parse a JSON string received from the server. */
void ws_protocol_parse(const char *json_str);

/* Build a base64-encoded stt_audio JSON message.
 * Returns number of bytes written, or -1 on error. */
int ws_protocol_build_stt_audio(const int16_t *pcm_data, size_t sample_count,
                                bool is_last, char *out_buf, size_t buf_size);

/* ---- Legacy ws_client_* API (backward-compatible wrappers) -------------- */
esp_err_t ws_client_init(void);
esp_err_t ws_client_connect(const char *url);
void      ws_client_disconnect(void);
bool      ws_client_is_connected(void);
bool      ws_client_is_ready(void);

esp_err_t ws_client_send_hello(void);
esp_err_t ws_client_send_text(const char *text);
esp_err_t ws_client_send_stt_partial(const char *text);
esp_err_t ws_client_send_stt_final(const char *text);
esp_err_t ws_client_send_stt_audio(const int16_t *pcm_data,
                                   size_t sample_count, bool is_last);
esp_err_t ws_client_send_interrupt(void);
esp_err_t ws_client_send_ping(void);

/* ---- Voice session & binary audio (ClawChat protocol) ------------------- */
esp_err_t ws_client_send_voice_session_start(void);
esp_err_t ws_client_send_voice_session_stop(void);
esp_err_t ws_client_send_voice_session_idle(const char *reason);
esp_err_t ws_client_send_voice_turn_complete(void);

/* Binary audio frame: 32-byte header + raw PCM16-LE.
 * Header layout: magic(4) | chunk_idx(4 LE) | sample_rate(4 LE) |
 *                flags(1, bit0=isLast) | channels(1) | bits(1) | rsvd(1) |
 *                turn_id(16)
 */
esp_err_t ws_client_send_binary_chunk(const int16_t *pcm, size_t sample_count,
                                      uint32_t chunk_idx,
                                      const uint8_t turn_id[16],
                                      bool is_last);

typedef void (*ws_message_cb_t)(const char *type, const char *data, void *arg);
void ws_register_message_callback(ws_message_cb_t cb, void *arg);

/* ---- protocol_t factory ------------------------------------------------- */
/* Returns a heap-allocated protocol_t backed by this WebSocket implementation. */
protocol_t *protocol_ws_create(void);

#endif /* WS_PROTOCOL_H */
