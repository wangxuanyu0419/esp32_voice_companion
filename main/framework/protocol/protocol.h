#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * protocol_t — Abstract transport/protocol interface.
 * The WebSocket implementation (ws_protocol.c) is the only concrete impl for
 * Phase A.  MQTT or other transports can be added later without changing callers.
 */
typedef struct protocol_t protocol_t;

struct protocol_t {
    /* Transport */
    esp_err_t (*connect)(protocol_t *self, const char *url, const char *token);
    void      (*disconnect)(protocol_t *self);
    bool      (*is_connected)(protocol_t *self);

    /* Outbound */
    esp_err_t (*send_hello)(protocol_t *self, const char *device_id,
                            const char *agent);
    esp_err_t (*send_audio)(protocol_t *self, const int16_t *samples,
                            size_t count, bool is_last);
    esp_err_t (*send_text)(protocol_t *self, const char *text);
    esp_err_t (*send_interrupt)(protocol_t *self);

    /* Inbound callbacks — set by the Application layer before connect(). */
    void (*on_tts_text)(const char *text);
    void (*on_assistant_text)(const char *text);
    void (*on_status)(bool listening, bool thinking, bool speaking);
    void (*on_emotion)(const char *name, int duration_ms);
    void (*on_connected)(void);
    void (*on_disconnected)(void);

    /* Implementation-private data pointer. */
    void *priv;
};

/* Factory: create a WebSocket-backed protocol instance (heap-allocated). */
protocol_t *protocol_ws_create(void);

/* Destroy and free a protocol instance returned by protocol_ws_create(). */
void protocol_destroy(protocol_t *proto);

#endif /* PROTOCOL_H */
