#ifndef CHAT_APP_H
#define CHAT_APP_H

#include "app_interface.h"
#include "esp_err.h"

/*
 * chat_app — ClawChat tap-to-toggle voice conversation app.
 *
 * Interaction:
 *   Tap avatar image or short-press BOOT → start/stop recording
 *   Long-press BOOT → return to launcher
 *
 * Protocol:
 *   voice_session_start → binary PCM frames → voice_turn_complete
 *   VAD: 600 ms silence auto-stops recording
 */

/* Returns the singleton app_t for the chat app. */
app_t *chat_app_get(void);

/* Initialise the chat app (register callbacks, create tasks). */
esp_err_t chat_app_init(void);

/*
 * Toggle recording state.  Call from the BOOT button release handler
 * or the avatar tap callback.  Safe to call from any task / ISR context
 * (posts to an internal FreeRTOS queue).
 */
void chat_app_toggle_recording(void);

#endif /* CHAT_APP_H */
