#ifndef CHAT_APP_H
#define CHAT_APP_H

#include "app_interface.h"
#include "esp_err.h"

/*
 * chat_app — OpenClaw/Hermes push-to-talk chat application.
 *
 * Responsibilities:
 *   - Register BOOT button callbacks (press-start → record, release → finalise)
 *   - Register WS protocol callbacks (tts_text, live2d, status, assistant_text)
 *   - Run the recording session task (Core 0)
 */

/* Returns the singleton app_t for the chat app. */
app_t *chat_app_get(void);

/* Initialise the chat app (register callbacks, create tasks). */
esp_err_t chat_app_init(void);

/* Compatibility: expose the recording session task entry for pinned creation. */
void chat_app_record_task(void *arg);

#endif /* CHAT_APP_H */
