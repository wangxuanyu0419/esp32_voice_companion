#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "APP_STATE";

const char *app_state_to_string(app_state_t state)
{
    switch (state) {
        case APP_STATE_INIT:      return "INIT";
        case APP_STATE_IDLE:      return "IDLE";
        case APP_STATE_LISTENING: return "LISTENING";
        case APP_STATE_THINKING:  return "THINKING";
        case APP_STATE_SPEAKING:  return "SPEAKING";
        case APP_STATE_SLEEP:     return "SLEEP";
        case APP_STATE_ERROR:     return "ERROR";
        default:                  return "UNKNOWN";
    }
}

typedef struct {
    app_state_t from;
    app_state_t to;
} transition_t;

static const transition_t k_valid[] = {
    {APP_STATE_INIT,      APP_STATE_IDLE},
    {APP_STATE_INIT,      APP_STATE_ERROR},
    {APP_STATE_IDLE,      APP_STATE_LISTENING},
    {APP_STATE_IDLE,      APP_STATE_SLEEP},
    {APP_STATE_IDLE,      APP_STATE_ERROR},
    {APP_STATE_LISTENING, APP_STATE_THINKING},
    {APP_STATE_LISTENING, APP_STATE_IDLE},
    {APP_STATE_LISTENING, APP_STATE_SLEEP},
    {APP_STATE_THINKING,  APP_STATE_SPEAKING},
    {APP_STATE_THINKING,  APP_STATE_IDLE},
    {APP_STATE_THINKING,  APP_STATE_ERROR},
    {APP_STATE_SPEAKING,  APP_STATE_IDLE},
    {APP_STATE_SPEAKING,  APP_STATE_LISTENING},
    {APP_STATE_SPEAKING,  APP_STATE_ERROR},
    {APP_STATE_SLEEP,     APP_STATE_IDLE},
    {APP_STATE_ERROR,     APP_STATE_IDLE},
    {APP_STATE_ERROR,     APP_STATE_SLEEP},
};

bool app_state_can_transition(app_state_t from, app_state_t to)
{
    if (from == to) return true;
    for (size_t i = 0; i < sizeof(k_valid) / sizeof(k_valid[0]); i++) {
        if (k_valid[i].from == from && k_valid[i].to == to) return true;
    }
    return false;
}

static app_state_t     s_current            = APP_STATE_INIT;
static int64_t         s_last_activity_us   = 0;
static const int64_t   DEEP_SLEEP_TIMEOUT   = 300ULL * 1000000ULL; /* 5 min */

app_state_t app_state_get_current(void) { return s_current; }

void app_state_set_current(app_state_t state) { s_current = state; }

void reset_idle_timer(void) { s_last_activity_us = esp_timer_get_time(); }

bool is_active_state(app_state_t state)
{
    return (state == APP_STATE_LISTENING ||
            state == APP_STATE_THINKING  ||
            state == APP_STATE_SPEAKING);
}

bool should_enter_deep_sleep(void)
{
    if (is_active_state(app_state_get_current())) {
        reset_idle_timer();
        return false;
    }
    return (esp_timer_get_time() - s_last_activity_us) > DEEP_SLEEP_TIMEOUT;
}
