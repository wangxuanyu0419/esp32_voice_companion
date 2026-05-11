#ifndef BOARD_H
#define BOARD_H

#include "esp_err.h"

/*
 * board_t — Hardware Abstraction Layer interface.
 * One concrete implementation per board target (selected at compile time).
 */
typedef struct {
    const char *name;
    esp_err_t  (*init)(void);
    void       (*get_button_gpio)(int *boot_gpio);
    void       (*get_led_gpio)(int *led_r, int *led_g, int *led_b);
} board_t;

/* Returns the singleton board instance compiled in for this target. */
const board_t *board_get_instance(void);

/* Convenience: call board init and log the board name. */
esp_err_t board_init(void);

#endif /* BOARD_H */
