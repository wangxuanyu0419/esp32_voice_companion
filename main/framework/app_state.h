#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    APP_STATE_INIT = 0,
    APP_STATE_IDLE,
    APP_STATE_LISTENING,
    APP_STATE_THINKING,
    APP_STATE_SPEAKING,
    APP_STATE_SLEEP,
    APP_STATE_ERROR,
} app_state_t;

const char   *app_state_to_string(app_state_t state);
bool          app_state_can_transition(app_state_t from, app_state_t to);

app_state_t   app_state_get_current(void);
void          app_state_set_current(app_state_t state);

bool          should_enter_deep_sleep(void);
void          reset_idle_timer(void);
bool          is_active_state(app_state_t state);

#endif /* APP_STATE_H */
