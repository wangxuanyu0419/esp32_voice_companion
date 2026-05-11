#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "APP_STATE";

// 状态字符串映射
const char* app_state_to_string(app_state_t state)
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

// 有效的状态转换
static const state_transition_t g_valid_transitions[] = {
    // 从 INIT
    {APP_STATE_INIT, APP_STATE_IDLE, true},
    {APP_STATE_INIT, APP_STATE_ERROR, true},
    
    // 从 IDLE
    {APP_STATE_IDLE, APP_STATE_LISTENING, true},
    {APP_STATE_IDLE, APP_STATE_SLEEP, true},
    {APP_STATE_IDLE, APP_STATE_ERROR, true},
    
    // 从 LISTENING
    {APP_STATE_LISTENING, APP_STATE_THINKING, true},
    {APP_STATE_LISTENING, APP_STATE_IDLE, true},    // 中断
    {APP_STATE_LISTENING, APP_STATE_SLEEP, true},
    
    // 从 THINKING
    {APP_STATE_THINKING, APP_STATE_SPEAKING, true},
    {APP_STATE_THINKING, APP_STATE_IDLE, true},     // 无回复
    {APP_STATE_THINKING, APP_STATE_ERROR, true},
    
    // 从 SPEAKING
    {APP_STATE_SPEAKING, APP_STATE_IDLE, true},
    {APP_STATE_SPEAKING, APP_STATE_LISTENING, true}, // 中断说话
    {APP_STATE_SPEAKING, APP_STATE_ERROR, true},
    
    // 从 SLEEP（只能到 IDLE）
    {APP_STATE_SLEEP, APP_STATE_IDLE, true},
    
    // 从 ERROR
    {APP_STATE_ERROR, APP_STATE_IDLE, true},        // 重试
    {APP_STATE_ERROR, APP_STATE_SLEEP, true},
};

bool app_state_can_transition(app_state_t from, app_state_t to)
{
    // 相同状态允许
    if (from == to) return true;
    
    for (size_t i = 0; i < sizeof(g_valid_transitions) / sizeof(g_valid_transitions[0]); i++) {
        if (g_valid_transitions[i].from == from && 
            g_valid_transitions[i].to == to) {
            return true;
        }
    }
    
    return false;
}

// 空闲计时器
static int64_t g_last_activity_time = 0;
static const int64_t DEEP_SLEEP_TIMEOUT_US = 300ULL * 1000000ULL; // 5分钟

void reset_idle_timer(void)
{
    g_last_activity_time = esp_timer_get_time();
}

bool should_enter_deep_sleep(void)
{
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - g_last_activity_time;
    
    // 活动状态不进入深度睡眠
    if (is_active_state(app_state_get_current())) {
        reset_idle_timer();
        return false;
    }
    
    return elapsed > DEEP_SLEEP_TIMEOUT_US;
}

bool is_active_state(app_state_t state)
{
    return (state == APP_STATE_LISTENING || 
            state == APP_STATE_THINKING || 
            state == APP_STATE_SPEAKING);
}

app_state_t app_state_get_current(void);

// 设置当前状态（供外部调用）
void app_state_set_current(app_state_t state);

static app_state_t g_current_state = APP_STATE_INIT;

app_state_t app_state_get_current(void)
{
    return g_current_state;
}

void app_state_set_current(app_state_t state)
{
    g_current_state = state;
}