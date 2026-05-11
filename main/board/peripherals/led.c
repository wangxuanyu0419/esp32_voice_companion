#include "led.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LED";

#define LED_R_GPIO  GPIO_NUM_38
#define LED_G_GPIO  GPIO_NUM_39
#define LED_B_GPIO  GPIO_NUM_40

static led_color_t s_color       = LED_OFF;
static led_mode_t  s_mode        = LED_MODE_STATIC;
static bool        s_initialized = false;

static void set_rgb(led_color_t color)
{
    gpio_set_level(LED_R_GPIO, 0);
    gpio_set_level(LED_G_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    switch (color) {
        case LED_GREEN:  gpio_set_level(LED_G_GPIO, 1); break;
        case LED_BLUE:   gpio_set_level(LED_B_GPIO, 1); break;
        case LED_YELLOW: gpio_set_level(LED_R_GPIO, 1); gpio_set_level(LED_G_GPIO, 1); break;
        case LED_RED:    gpio_set_level(LED_R_GPIO, 1); break;
        case LED_WHITE:  gpio_set_level(LED_R_GPIO, 1); gpio_set_level(LED_G_GPIO, 1); gpio_set_level(LED_B_GPIO, 1); break;
        default: break;
    }
}

esp_err_t led_init(void)
{
    if (s_initialized) return ESP_OK;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_R_GPIO) | (1ULL << LED_G_GPIO) | (1ULL << LED_B_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    led_off();
    s_initialized = true;
    ESP_LOGI(TAG, "LED ready (R=%d G=%d B=%d)", LED_R_GPIO, LED_G_GPIO, LED_B_GPIO);
    return ESP_OK;
}

esp_err_t led_set_color(led_color_t color)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_color = color;
    set_rgb(color);
    return ESP_OK;
}

esp_err_t led_set_mode(led_mode_t mode)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_mode = mode;
    return ESP_OK;
}

esp_err_t led_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    led_color_t color;
    led_mode_t  mode;
    switch (state) {
        case APP_STATE_IDLE:      color = LED_GREEN;  mode = LED_MODE_BREATHE;    break;
        case APP_STATE_LISTENING: color = LED_BLUE;   mode = LED_MODE_BLINK;      break;
        case APP_STATE_THINKING:  color = LED_YELLOW; mode = LED_MODE_BLINK;      break;
        case APP_STATE_SPEAKING:  color = LED_GREEN;  mode = LED_MODE_STATIC;     break;
        case APP_STATE_ERROR:     color = LED_RED;    mode = LED_MODE_FAST_BLINK; break;
        default:                  color = LED_OFF;    mode = LED_MODE_STATIC;     break;
    }
    led_set_color(color);
    led_set_mode(mode);
    return ESP_OK;
}

esp_err_t led_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_color = LED_OFF;
    s_mode  = LED_MODE_STATIC;
    set_rgb(LED_OFF);
    return ESP_OK;
}
