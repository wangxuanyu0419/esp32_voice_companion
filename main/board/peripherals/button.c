#include "button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "BUTTON";

#define BOOT_BUTTON_GPIO            GPIO_NUM_0
#define BUTTON_SHORT_THRESHOLD_MS   1000
#define BUTTON_LONG_THRESHOLD_MS    3000

static bool s_boot_pressed = false;
static bool s_pwr_pressed  = false;

static button_callback_t      s_boot_cb         = NULL;
static void                  *s_boot_cb_arg      = NULL;
static button_callback_t      s_pwr_cb           = NULL;
static void                  *s_pwr_cb_arg       = NULL;
static button_press_start_cb_t s_press_start_cb  = NULL;
static void                  *s_press_start_arg  = NULL;

static TimerHandle_t s_boot_timer = NULL;

static void boot_timer_callback(TimerHandle_t timer)
{
    if (s_boot_cb && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT long press");
        s_boot_cb(true, s_boot_cb_arg);
    }
}

static void boot_poll_task(void *arg)
{
    bool last_state = false;
    int64_t press_time = 0;

    while (1) {
        bool current = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

        if (current && !last_state) {
            /* Button just pressed — start long-press countdown */
            press_time = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "BOOT pressed");
            if (s_boot_timer) xTimerStart(s_boot_timer, 0);
            if (s_press_start_cb) s_press_start_cb(s_press_start_arg);
        } else if (!current && last_state) {
            /* Button just released — cancel timer, fire short press if quick */
            if (s_boot_timer) xTimerStop(s_boot_timer, 0);
            int64_t duration = esp_timer_get_time() / 1000 - press_time;
            if (duration < BUTTON_LONG_THRESHOLD_MS && s_boot_cb) {
                ESP_LOGI(TAG, "BOOT short press (%lld ms)", duration);
                s_boot_cb(false, s_boot_cb_arg);
            }
        }

        last_state = current;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Button initialising (BOOT=GPIO%d)...", BOOT_BUTTON_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_boot_timer = xTimerCreate("boot_tmr",
                                pdMS_TO_TICKS(BUTTON_LONG_THRESHOLD_MS),
                                pdFALSE, NULL, boot_timer_callback);

    xTaskCreate(boot_poll_task, "boot_poll", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button ready");
    return ESP_OK;
}

void button_reset(void)
{
    s_boot_pressed = false;
    s_pwr_pressed  = false;
}

bool button_boot_is_pressed(void) { return s_boot_pressed; }
bool button_pwr_is_pressed(void)  { return s_pwr_pressed;  }

void button_register_press_start_callback(button_press_start_cb_t cb, void *arg)
{
    s_press_start_cb  = cb;
    s_press_start_arg = arg;
}

void button_register_boot_callback(button_callback_t cb, void *arg)
{
    s_boot_cb     = cb;
    s_boot_cb_arg = arg;
}

void button_register_pwr_callback(button_callback_t cb, void *arg)
{
    s_pwr_cb     = cb;
    s_pwr_cb_arg = arg;
}
