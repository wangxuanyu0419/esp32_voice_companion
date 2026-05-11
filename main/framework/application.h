#ifndef APPLICATION_H
#define APPLICATION_H

#include "app_state.h"
#include "esp_err.h"

/*
 * Application singleton — owns the global state mutex and drives LED/UI
 * updates on every state transition.
 */

esp_err_t application_init(void);

/* Thread-safe state transition: updates app_state + LED + avatar + idle timer. */
void application_set_state(app_state_t new_state);

app_state_t application_get_state(void);

#endif /* APPLICATION_H */
