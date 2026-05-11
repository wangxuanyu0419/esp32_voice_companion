#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include "app_state.h"
#include <stdint.h>

/**
 * LED 状态指示
 * 
 * 使用板载 RGB LED 或单色 LED
 * 
 * 颜色定义：
 * - 绿色：正常/待机
 * - 蓝色：录音/倾听
 * - 黄色：思考中
 * - 红色：错误
 */

typedef enum {
    LED_OFF = 0,
    LED_GREEN,
    LED_BLUE,
    LED_YELLOW,
    LED_RED,
    LED_WHITE,
} led_color_t;

typedef enum {
    LED_MODE_STATIC,     // 常亮
    LED_MODE_BLINK,      // 闪烁
    LED_MODE_BREATHE,    // 呼吸
    LED_MODE_FAST_BLINK, // 快闪
} led_mode_t;

// 初始化 LED
esp_err_t led_init(void);

// 设置 LED 颜色
esp_err_t led_set_color(led_color_t color);

// 设置 LED 模式
esp_err_t led_set_mode(led_mode_t mode);

// 更新应用状态（自动设置颜色和模式）
esp_err_t led_set_state(app_state_t state);

// 关闭 LED
esp_err_t led_off(void);

#endif // LED_INDICATOR_H