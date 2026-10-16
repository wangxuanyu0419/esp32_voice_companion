#include "button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

static const char *TAG = "BUTTON";

#define BOOT_BUTTON_GPIO            GPIO_NUM_0
#define BUTTON_SHORT_THRESHOLD_MS   1000
#define BUTTON_LONG_THRESHOLD_MS    3000
/* Edges closer together than this are treated as contact bounce, not a real
 * transition.  30 ms comfortably covers typical tactile-switch bounce and is
 * well under the old 50 ms poll period this replaces. */
#define BUTTON_DEBOUNCE_MS          30

static bool s_boot_pressed = false;
static bool s_pwr_pressed  = false;

static button_callback_t      s_boot_cb         = NULL;
static void                  *s_boot_cb_arg      = NULL;
static button_callback_t      s_pwr_cb           = NULL;
static void                  *s_pwr_cb_arg       = NULL;
static button_press_start_cb_t s_press_start_cb  = NULL;
static void                  *s_press_start_arg  = NULL;

static TimerHandle_t s_boot_timer     = NULL;
static QueueHandle_t s_gpio_evt_queue = NULL;

static void boot_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_boot_cb && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT long press");
        s_boot_cb(true, s_boot_cb_arg);
    }
}

/*
 * Minimal, IRAM-safe ISR: no logging, no floating point, no blocking calls —
 * just a timestamp handed off to boot_event_task via a queue.  All the real
 * work (debounce, short/long-press decision, calling back into the app) that
 * used to run in the 50 ms boot_poll_task now runs in boot_event_task
 * instead, unchanged in spirit and stack budget.
 */
static void IRAM_ATTR boot_gpio_isr_handler(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();
    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_gpio_evt_queue, &now_us, &higher_prio_woken);
    if (higher_prio_woken) portYIELD_FROM_ISR();
}

static void boot_event_task(void *arg)
{
    (void)arg;
    bool    last_state   = false;
    int64_t press_time   = 0;
    int64_t last_edge_us = 0;

    for (;;) {
        int64_t evt_us;
        if (xQueueReceive(s_gpio_evt_queue, &evt_us, portMAX_DELAY) != pdTRUE) continue;

        /* Debounce: collapse edges that arrive faster than a real button
         * bounce would allow (using the ISR-captured timestamp, so a burst
         * of bounce edges queued during the settle delay below is filtered
         * out too, not just the first one). */
        if (evt_us - last_edge_us < (int64_t)BUTTON_DEBOUNCE_MS * 1000) continue;
        last_edge_us = evt_us;

        /* Let contact bounce finish, then sample the settled level — this is
         * the same effective debounce the old 50 ms poll cadence gave for
         * free, just event-driven instead of continuous. */
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        bool current = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
        if (current == last_state) continue;   /* bounce, no real transition */

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
    }
}

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Button initialising (BOOT=GPIO%d, interrupt-driven)...", BOOT_BUTTON_GPIO);

    /* GPIO0 is also the BOOT strapping pin, but strapping is latched by the
     * ROM bootloader before app_main() ever runs; configuring it as an input
     * with an interrupt afterwards (same as the previous GPIO_INTR_DISABLE
     * polling config, just with edges enabled) does not affect boot mode. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_gpio_evt_queue = xQueueCreate(8, sizeof(int64_t));
    if (!s_gpio_evt_queue) return ESP_ERR_NO_MEM;

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means the shared GPIO ISR service is already
         * installed (e.g. by another peripheral) — not an error for us. */
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_ret));
        return isr_ret;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_gpio_isr_handler, NULL));

    s_boot_timer = xTimerCreate("boot_tmr",
                                pdMS_TO_TICKS(BUTTON_LONG_THRESHOLD_MS),
                                pdFALSE, NULL, boot_timer_callback);

    /* boot_event_task and the FreeRTOS timer task both have deliberately
     * small stacks (see chat_app.c's button_event_cb) — keep the same 2048
     * word budget the old boot_poll_task used; this task does no more work
     * than that one did. */
    xTaskCreate(boot_event_task, "boot_evt", 2048, NULL, 10, NULL);

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
