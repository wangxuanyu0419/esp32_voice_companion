#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
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

void ws_protocol_register_callbacks(ws_on_tts_cb_t tts_cb,
                                    ws_on_live2d_cb_t live2d_cb,
                                    ws_on_status_cb_t status_cb,
                                    ws_on_text_cb_t text_cb,
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

esp_err_t ws_client_send_hello(void);
esp_err_t ws_client_send_text(const char *text);
esp_err_t ws_client_send_stt_partial(const char *text);
esp_err_t ws_client_send_stt_final(const char *text);
esp_err_t ws_client_send_stt_audio(const int16_t *pcm_data,
                                   size_t sample_count, bool is_last);
esp_err_t ws_client_send_interrupt(void);
esp_err_t ws_client_send_ping(void);

typedef void (*ws_message_cb_t)(const char *type, const char *data, void *arg);
void ws_register_message_callback(ws_message_cb_t cb, void *arg);

/* ---- protocol_t factory ------------------------------------------------- */
/* Returns a heap-allocated protocol_t backed by this WebSocket implementation. */
protocol_t *protocol_ws_create(void);

#endif /* WS_PROTOCOL_H */
