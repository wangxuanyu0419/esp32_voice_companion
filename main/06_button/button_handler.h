#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * 按键处理
 * 
 * BOOT 按键：
 * - 短按（<1s）：开始/停止录音
 * - 长按（>3s）：进入配网模式
 * 
 * PWR 按键：
 * - 短按：亮屏/息屏
 */

// 初始化按键
esp_err_t button_init(void);

// 重置按键状态
void button_reset(void);

// 获取 BOOT 按键是否被按下
bool button_boot_is_pressed(void);

// 获取 PWR 按键是否被按下
bool button_pwr_is_pressed(void);

// 注册按键回调
typedef void (*button_callback_t)(bool long_press, void* arg);

// Phase-1: interrupt callback (called on press start — for interrupt-before-capture)
typedef void (*button_press_start_cb_t)(void* arg);
void button_register_press_start_callback(button_press_start_cb_t cb, void* arg);

void button_register_boot_callback(button_callback_t cb, void* arg);
void button_register_pwr_callback(button_callback_t cb, void* arg);

#endif // BUTTON_HANDLER_H