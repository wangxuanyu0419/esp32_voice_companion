/**
 * 按键处理 - 实现
 */

#include "button_handler.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char* TAG = "BUTTON";

// 按键 GPIO
#define BOOT_BUTTON_GPIO     GPIO_NUM_0
#define PWR_BUTTON_GPIO      GPIO_NUM_0  // 复用，实际需要确认

// 按键参数
#define BUTTON_SHORT_THRESHOLD_MS   1000
#define BUTTON_LONG_THRESHOLD_MS    3000

// 按键状态
static bool s_boot_pressed = false;
static bool s_pwr_pressed = false;

// 回调
static button_callback_t s_boot_cb = NULL;
static void* s_boot_cb_arg = NULL;
static button_callback_t s_pwr_cb = NULL;
static void* s_pwr_cb_arg = NULL;

// Phase-1: press-start callback (fire immediately on press, before release)
static button_press_start_cb_t s_press_start_cb = NULL;
static void* s_press_start_cb_arg = NULL;

// 定时器（用于检测长按）
static TimerHandle_t s_boot_timer = NULL;
static TimerHandle_t s_pwr_timer = NULL;

// 按键按下时间
static int64_t s_boot_press_time = 0;
static int64_t s_pwr_press_time = 0;

static void IRAM_ATTR boot_isr_handler(void* arg)
{
    // 简单的防抖处理
    static int64_t last_trigger = 0;
    int64_t now = esp_timer_get_time() / 1000; // ms
    
    if (now - last_trigger < 200) return;  // 200ms 防抖
    last_trigger = now;
    
    BaseType_t high_task_wakeup = pdFALSE;
    
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        // 按键按下
        s_boot_pressed = true;
        s_boot_press_time = now;
        
        // 启动长按检测定时器
        xTimerResetFromISR(s_boot_timer, &high_task_wakeup);
        xTimerStartFromISR(s_boot_timer, &high_task_wakeup);
    } else {
        // 按键释放
        int64_t press_duration = now - s_boot_press_time;
        s_boot_pressed = false;
        
        xTimerStopFromISR(s_boot_timer, &high_task_wakeup);
        
        // 判断短按还是长按
        bool long_press = (press_duration > BUTTON_LONG_THRESHOLD_MS);
        
        // 调用回调
        if (s_boot_cb) {
            // 在任务中调用，避免 ISR 中调用阻塞
            // 使用 Timer 回调传递事件
        }
    }
}

static void IRAM_ATTR pwr_isr_handler(void* arg)
{
    // 类似实现
}

static void boot_timer_callback(TimerHandle_t timer)
{
    // 长按检测触发
    if (s_boot_cb && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT long press detected");
        s_boot_cb(true, s_boot_cb_arg);
    }
}

static void pwr_timer_callback(TimerHandle_t timer)
{
    if (s_pwr_cb && gpio_get_level(PWR_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "PWR long press detected");
        s_pwr_cb(true, s_pwr_cb_arg);
    }
}

static void boot_poll_task(void* arg)
{
    // 轮询方式检测短按（更可靠）
    bool last_state = false;
    int64_t press_time = 0;
    
    while (1) {
        bool current_state = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
        
        if (current_state && !last_state) {
            // 按键按下
            press_time = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "BOOT pressed");

            // Phase-1: fire press-start callback (e.g. to send interrupt)
            if (s_press_start_cb) {
                s_press_start_cb(s_press_start_cb_arg);
            }
        }
        else if (!current_state && last_state) {
            // 按键释放
            int64_t duration = esp_timer_get_time() / 1000 - press_time;
            
            if (duration < BUTTON_LONG_THRESHOLD_MS && s_boot_cb) {
                ESP_LOGI(TAG, "BOOT short press (%ld ms)", duration);
                s_boot_cb(false, s_boot_cb_arg);
            }
        }
        
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Button initializing...");
    
    // 配置 BOOT 按键 GPIO
    gpio_config_t boot_config = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // 上升沿和下降沿都触发
    };
    
    ESP_ERROR_CHECK(gpio_config(&boot_config));
    
    // 创建定时器（用于长按检测）
    s_boot_timer = xTimerCreate("boot_timer", 
                                pdMS_TO_TICKS(BUTTON_LONG_THRESHOLD_MS),
                                pdFALSE, NULL, boot_timer_callback);
    
    // 创建轮询任务（检测短按）
    xTaskCreate(boot_poll_task, "boot_poll", 2048, NULL, 10, NULL);
    
    // 如果有 PWR 按键，配置同样的方式
    // PWR 按键通常通过 AXP2101 中断触发，这里简化处理
    
    ESP_LOGI(TAG, "Button initialized (BOOT=GPIO0)");
    return ESP_OK;
}

void button_reset(void)
{
    s_boot_pressed = false;
    s_pwr_pressed = false;
}

bool button_boot_is_pressed(void)
{
    return s_boot_pressed;
}

bool button_pwr_is_pressed(void)
{
    return s_pwr_pressed;
}

void button_register_boot_callback(button_callback_t cb, void* arg)
{
    s_boot_cb = cb;
    s_boot_cb_arg = arg;
}

void button_register_pwr_callback(button_callback_t cb, void* arg)
{
    s_pwr_cb = cb;
    s_pwr_cb_arg = arg;
}

void button_register_press_start_callback(button_press_start_cb_t cb, void* arg)
{
    s_press_start_cb = cb;
    s_press_start_cb_arg = arg;
}