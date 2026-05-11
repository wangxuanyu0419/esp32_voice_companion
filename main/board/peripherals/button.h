#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* BOOT button (GPIO0) + optional PWR button. */

esp_err_t button_init(void);
void      button_reset(void);
bool      button_boot_is_pressed(void);
bool      button_pwr_is_pressed(void);

typedef void (*button_callback_t)(bool long_press, void *arg);
typedef void (*button_press_start_cb_t)(void *arg);

void button_register_press_start_callback(button_press_start_cb_t cb, void *arg);
void button_register_boot_callback(button_callback_t cb, void *arg);
void button_register_pwr_callback(button_callback_t cb, void *arg);

#endif /* BUTTON_H */
