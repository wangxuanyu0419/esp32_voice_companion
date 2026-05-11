#include "application.h"
#include "app_state.h"
#include "scene_avatar.h"
#include "led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "APPLICATION";

static SemaphoreHandle_t s_state_mutex = NULL;

esp_err_t application_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    reset_idle_timer();
    ESP_LOGI(TAG, "Application framework ready");
    return ESP_OK;
}

void application_set_state(app_state_t new_state)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    app_state_t cur = app_state_get_current();
    if (cur != new_state) {
        ESP_LOGI(TAG, "State: %s → %s",
                 app_state_to_string(cur),
                 app_state_to_string(new_state));
        app_state_set_current(new_state);
        avatar_set_state(new_state);
        led_set_state(new_state);
        reset_idle_timer();
    }

    xSemaphoreGive(s_state_mutex);
}

app_state_t application_get_state(void)
{
    return app_state_get_current();
}
