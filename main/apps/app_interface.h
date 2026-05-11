#ifndef APP_INTERFACE_H
#define APP_INTERFACE_H

#include <stdint.h>
#include "esp_err.h"
#include "event_bus.h"

/*
 * app_t — Pluggable application interface.
 * Each app (Chat, Settings, Clock…) fills in the lifecycle hooks it needs.
 * NULL function pointers are silently ignored by the framework.
 */
typedef struct app_t app_t;

struct app_t {
    const char *id;    /* Unique identifier: "chat", "settings", … */
    const char *name;  /* Human-readable display name */

    /* Lifecycle */
    esp_err_t (*on_enter)(app_t *self);   /* App becomes active */
    void      (*on_exit)(app_t *self);    /* App loses focus */

    /* Event / data callbacks */
    void (*on_event)(app_t *self, const event_t *evt);
    void (*on_audio_in)(app_t *self, const int16_t *samples, size_t count);
    void (*on_tick)(app_t *self, uint32_t dt_ms);  /* ~10 ms period */

    /* Implementation-private data pointer */
    void *priv;
};

#endif /* APP_INTERFACE_H */
