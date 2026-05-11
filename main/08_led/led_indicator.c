/**
 * LED 状态指示 - 实现
 * 
 * 注意：ESP32-S3-Touch-AMOLED-1.8 可能有 RGB LED
 * 需要根据实际硬件配置 GPIO
 */

#include "led_indicator.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "LED";

// LED GPIO（需要根据实际硬件配置）
#define LED_R_GPIO   GPIO_NUM_38  // 红色
#define LED_G_GPIO   GPIO_NUM_39  // 绿色
#define LED_B_GPIO   GPIO_NUM_40  // 蓝色

static led_color_t s_current_color = LED_OFF;
static led_mode_t s_current_mode = LED_MODE_STATIC;
static bool s_initialized = false;

static const char* color_to_string(led_color_t color)
{
    switch (color) {
        case LED_OFF:    return "OFF";
        case LED_GREEN:  return "GREEN";
        case LED_BLUE:   return "BLUE";
        case LED_YELLOW: return "YELLOW";
        case LED_RED:    return "RED";
        case LED_WHITE:  return "WHITE";
        default:         return "UNKNOWN";
    }
}

static void set_rgb(led_color_t color)
{
    // 关闭所有颜色
    gpio_set_level(LED_R_GPIO, 0);
    gpio_set_level(LED_G_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    
    // 设置对应颜色
    switch (color) {
        case LED_GREEN:
            gpio_set_level(LED_G_GPIO, 1);
            break;
        case LED_BLUE:
            gpio_set_level(LED_B_GPIO, 1);
            break;
        case LED_YELLOW:  // 红+绿
            gpio_set_level(LED_R_GPIO, 1);
            gpio_set_level(LED_G_GPIO, 1);
            break;
        case LED_RED:
            gpio_set_level(LED_R_GPIO, 1);
            break;
        case LED_WHITE:   // 红+绿+蓝
            gpio_set_level(LED_R_GPIO, 1);
            gpio_set_level(LED_G_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            break;
        case LED_OFF:
        default:
            break;
    }
}

static void led_task(void* arg)
{
    bool led_state = false;
    TickType_t delay;
    
    while (1) {
        switch (s_current_mode) {
            case LED_MODE_STATIC:
                // 常亮，保持当前颜色
                delay = portMAX_DELAY;
                break;
                
            case LED_MODE_BLINK:
                // 慢闪（500ms）
                led_state = !led_state;
                // 这里简化处理，实际应该切换引脚
                delay = pdMS_TO_TICKS(500);
                break;
                
            case LED_MODE_BREATHE:
                // 呼吸灯效果（渐变）
                // TODO: 实现 PWM 呼吸
                delay = pdMS_TO_TICKS(100);
                break;
                
            case LED_MODE_FAST_BLINK:
                // 快闪（100ms）
                led_state = !led_state;
                delay = pdMS_TO_TICKS(100);
                break;
        }
        
        vTaskDelay(delay);
    }
}

esp_err_t led_init(void)
{
    if (s_initialized) return ESP_OK;
    
    ESP_LOGI(TAG, "LED initializing...");
    
    // 配置 LED GPIO
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_R_GPIO) | (1ULL << LED_G_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    
    // 默认关闭
    led_off();
    
    // 创建 LED 任务（用于闪烁效果）
    // xTaskCreate(led_task, "led_task", 2048, NULL, 2, NULL);
    
    s_initialized = true;
    ESP_LOGI(TAG, "LED initialized");
    
    return ESP_OK;
}

esp_err_t led_set_color(led_color_t color)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_current_color = color;
    set_rgb(color);
    
    ESP_LOGI(TAG, "LED color set to: %s", color_to_string(color));
    return ESP_OK;
}

esp_err_t led_set_mode(led_mode_t mode)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_current_mode = mode;
    ESP_LOGI(TAG, "LED mode set to: %d", mode);
    return ESP_OK;
}

esp_err_t led_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    led_color_t color;
    led_mode_t mode;
    
    switch (state) {
        case APP_STATE_IDLE:
            color = LED_GREEN;
            mode = LED_MODE_BREATHE;  // 绿色呼吸
            break;
        case APP_STATE_LISTENING:
            color = LED_BLUE;
            mode = LED_MODE_BLINK;    // 蓝色慢闪
            break;
        case APP_STATE_THINKING:
            color = LED_YELLOW;
            mode = LED_MODE_BLINK;    // 黄色慢闪
            break;
        case APP_STATE_SPEAKING:
            color = LED_GREEN;
            mode = LED_MODE_STATIC;   // 绿色常亮
            break;
        case APP_STATE_ERROR:
            color = LED_RED;
            mode = LED_MODE_FAST_BLINK; // 红色快闪
            break;
        case APP_STATE_SLEEP:
        case APP_STATE_INIT:
        default:
            color = LED_OFF;
            mode = LED_MODE_STATIC;
            break;
    }
    
    led_set_color(color);
    led_set_mode(mode);
    
    return ESP_OK;
}

esp_err_t led_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_current_color = LED_OFF;
    s_current_mode = LED_MODE_STATIC;
    set_rgb(LED_OFF);
    
    return ESP_OK;
}