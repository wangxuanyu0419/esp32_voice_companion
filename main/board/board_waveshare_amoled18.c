#include "board.h"
#include "button.h"
#include "led.h"
#include "esp_log.h"

static const char *TAG = "BOARD_WAVESHARE";

/* GPIO assignments for Waveshare ESP32-S3-Touch-AMOLED-1.8 */
#define WAVESHARE_BOOT_GPIO  0
#define WAVESHARE_LED_R_GPIO 38
#define WAVESHARE_LED_G_GPIO 39
#define WAVESHARE_LED_B_GPIO 40

static esp_err_t waveshare_init(void)
{
    ESP_LOGI(TAG, "Initialising Waveshare AMOLED-1.8 peripherals...");

    esp_err_t ret;

    ret = led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = button_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Board peripherals ready");
    return ESP_OK;
}

static void waveshare_get_button_gpio(int *boot_gpio)
{
    *boot_gpio = WAVESHARE_BOOT_GPIO;
}

static void waveshare_get_led_gpio(int *led_r, int *led_g, int *led_b)
{
    *led_r = WAVESHARE_LED_R_GPIO;
    *led_g = WAVESHARE_LED_G_GPIO;
    *led_b = WAVESHARE_LED_B_GPIO;
}

static const board_t s_waveshare_board = {
    .name            = "Waveshare ESP32-S3-Touch-AMOLED-1.8",
    .init            = waveshare_init,
    .get_button_gpio = waveshare_get_button_gpio,
    .get_led_gpio    = waveshare_get_led_gpio,
};

const board_t *board_get_instance(void)
{
    return &s_waveshare_board;
}

esp_err_t board_init(void)
{
    const board_t *board = board_get_instance();
    ESP_LOGI(TAG, "Board: %s", board->name);
    return board->init();
}
