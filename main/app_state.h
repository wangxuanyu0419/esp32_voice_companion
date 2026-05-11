#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>

// 应用状态枚举
typedef enum {
    APP_STATE_INIT = 0,
    APP_STATE_IDLE,        // 待机，WiFi 已连接
    APP_STATE_LISTENING,    // 正在录音
    APP_STATE_THINKING,     // 等待服务器回复
    APP_STATE_SPEAKING,     // 播放 TTS
    APP_STATE_SLEEP,        // 深度睡眠
    APP_STATE_ERROR,        // 连接错误
} app_state_t;

// 状态转换检查
typedef struct {
    app_state_t from;
    app_state_t to;
    bool allowed;
} state_transition_t;

// 获取状态字符串
const char* app_state_to_string(app_state_t state);

// 检查状态转换是否有效
bool app_state_can_transition(app_state_t from, app_state_t to);

// 深度睡眠判断（无操作超时）
bool should_enter_deep_sleep(void);

// 重置空闲计时器
void reset_idle_timer(void);

// 检查是否在活动状态
bool is_active_state(app_state_t state);

#endif // APP_STATE_H