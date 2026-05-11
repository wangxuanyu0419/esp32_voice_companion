#ifndef AVATAR_UI_H
#define AVATAR_UI_H

#include "app_state.h"
#include "emotion_map.h"
#include <stdint.h>

/**
 * 头像 UI 管理器 - LVGL + AMOLED
 * 
 * 功能：
 * - 屏幕初始化
 * - Q 版头像显示（8种情绪）
 * - 状态动画
 * - 触摸检测
 * 
 * 情绪切换：收到 live2d emotion 事件时切换表情，
 * 支持临时切换后自动恢复（如 2.5s 后回到 idle）
 */

// 初始化屏幕
esp_err_t avatar_init(void);

// 更新应用状态（切换头像）
esp_err_t avatar_set_state(app_state_t state);

// 设置情绪表情（临时切换）
esp_err_t avatar_set_emotion(avatar_emotion_t emotion);

// 设置情绪并自动恢复
esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t emotion, int duration_ms);

// 显示消息（临时显示文字）
esp_err_t avatar_show_message(const char* msg);

// 清除消息
esp_err_t avatar_clear_message(void);

// 更新连接状态
esp_err_t avatar_set_connection_status(bool connected);

// 获取当前情绪
avatar_emotion_t avatar_get_current_emotion(void);

#endif // AVATAR_UI_H