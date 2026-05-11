#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "EVENT_BUS";

#define EVENT_QUEUE_DEPTH 32

static QueueHandle_t s_queue = NULL;

esp_err_t event_bus_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Event bus initialised");
    return ESP_OK;
}

esp_err_t event_bus_post(const event_t *evt)
{
    if (!s_queue || !evt) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event bus full, dropping event type=%d", evt->type);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int event_bus_post_from_isr(const event_t *evt)
{
    if (!s_queue || !evt) return 0;
    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_queue, evt, &higher_prio_woken);
    return (int)higher_prio_woken;
}

esp_err_t event_bus_receive(event_t *evt, uint32_t timeout_ms)
{
    if (!s_queue || !evt) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = (timeout_ms == UINT32_MAX)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_queue, evt, ticks) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
