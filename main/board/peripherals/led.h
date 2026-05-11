#ifndef LED_H
#define LED_H

#include "app_state.h"
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_GREEN,
    LED_BLUE,
    LED_YELLOW,
    LED_RED,
    LED_WHITE,
} led_color_t;

typedef enum {
    LED_MODE_STATIC,
    LED_MODE_BLINK,
    LED_MODE_BREATHE,
    LED_MODE_FAST_BLINK,
} led_mode_t;

esp_err_t led_init(void);
esp_err_t led_set_color(led_color_t color);
esp_err_t led_set_mode(led_mode_t mode);
esp_err_t led_set_state(app_state_t state);
esp_err_t led_off(void);

#endif /* LED_H */
