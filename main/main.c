/*
 * main.c — ESP32-S3 Voice Companion entry point.
 *
 * Lifecycle: board_init → system_init → application_init → application_run
 */

#include "board.h"
#include "wifi_manager.h"
#include "config_store.h"
#include "event_bus.h"
#include "application.h"
#include "app_state.h"
#include "display_driver.h"
#include "scene_avatar.h"
#include "audio_pipeline.h"
#include "ws_protocol.h"
#include "chat_app.h"
#include "launcher_app.h"
#include "settings_app.h"
#include "app_registry.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "MAIN";

/* -------------------------------------------------------------------------
 * system_init — NVS, event bus, config, wifi
 * ------------------------------------------------------------------------- */
static void system_init(void)
{
    /* NVS (required by WiFi and config) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and reinitialised");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Event bus (FreeRTOS queue) */
    ESP_ERROR_CHECK(event_bus_init());

    /* Persistent config (NVS-backed) */
    ESP_ERROR_CHECK(config_init());

    /* WiFi (non-blocking start; CONNECTED flag set by internal callback) */
    ESP_ERROR_CHECK(wifi_manager_init());
}

/* -------------------------------------------------------------------------
 * application_init — framework + protocol + audio
 * ------------------------------------------------------------------------- */
static void application_init_all(void)
{
    /* State machine + mutex */
    ESP_ERROR_CHECK(application_init());

    /* Display hardware + LVGL init (must come before any UI) */
    ESP_ERROR_CHECK(display_driver_init());

    /* Avatar UI scene */
    ESP_ERROR_CHECK(avatar_init());

    /* Audio pipeline */
    ESP_ERROR_CHECK(audio_init());

    /* WebSocket client (not connected yet) */
    ESP_ERROR_CHECK(ws_client_init());
}

/* -------------------------------------------------------------------------
 * lvgl_task — drives the LVGL timer loop.
 * Starts IMMEDIATELY so the screen shows UI regardless of network state.
 * Pinned to Core 0.
 * ------------------------------------------------------------------------- */
static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started (Core %d)", xPortGetCoreID());
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * net_task — waits for WiFi then connects WebSocket.
 * Runs independently of LVGL; pinned to Core 1.
 * ------------------------------------------------------------------------- */
static void net_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Net task: waiting for WiFi...");
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi connected — syncing time + connecting WebSocket");

    /* SNTP time sync — set UTC, server will keep wall-clock accurate */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org, UTC)");

    app_config_t cfg;
    if (config_get(&cfg) == ESP_OK) {
        ESP_LOGI(TAG, "WS URL: %s", cfg.server_url);
        esp_err_t ret = ws_client_connect(cfg.server_url);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WS connect failed: %s — will retry via events",
                     esp_err_to_name(ret));
        }
    }
    application_set_state(APP_STATE_IDLE);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * event_handler_task — processes event_bus events.
 * Pinned to Core 1.
 * ------------------------------------------------------------------------- */
static void event_handler_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "event_handler_task started (Core %d)", xPortGetCoreID());

    event_t evt;
    while (1) {
        if (event_bus_receive(&evt, 100) == ESP_OK) {
            switch (evt.type) {
                case EVT_WIFI_CONNECTED:
                    ESP_LOGI(TAG, "[EVT] WiFi connected");
                    break;
                case EVT_WIFI_DISCONNECTED:
                    ESP_LOGW(TAG, "[EVT] WiFi disconnected");
                    application_set_state(APP_STATE_ERROR);
                    break;
                case EVT_WS_CONNECTED:
                    ESP_LOGI(TAG, "[EVT] WS connected — sending hello");
                    ws_client_send_hello();
                    application_set_state(APP_STATE_IDLE);
                    break;
                case EVT_WS_DISCONNECTED:
                    ESP_LOGW(TAG, "[EVT] WS disconnected");
                    application_set_state(APP_STATE_ERROR);
                    break;
                case EVT_BUTTON_PRESS:
                    ESP_LOGI(TAG, "[EVT] Button pressed");
                    break;
                case EVT_BUTTON_RELEASE:
                    ESP_LOGI(TAG, "[EVT] Button released");
                    break;
                case EVT_APP_SWITCH: {
                    const char *target = (const char *)evt.data.ptr;
                    if (target) {
                        ESP_LOGI(TAG, "[EVT] App switch → %s", target);
                        app_registry_launch(target);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * application_run — create pinned tasks and return
 * ------------------------------------------------------------------------- */
static void application_run(void)
{
    /* Audio recording task — Core 0, highest user priority */
    xTaskCreatePinnedToCore(
        chat_app_record_task,
        "record_task",
        4096,
        NULL,
        5,
        NULL,
        0
    );

    /* LVGL rendering loop — Core 0, starts immediately (no network dependency) */
    xTaskCreatePinnedToCore(
        lvgl_task,
        "lvgl",
        6144,
        NULL,
        4,
        NULL,
        0
    );

    /* Network task — waits for WiFi then connects WS (Core 1)
     * ws_client_connect uses mbedTLS which needs ~6KB of stack */
    xTaskCreatePinnedToCore(
        net_task,
        "net_task",
        8192,
        NULL,
        3,
        NULL,
        1
    );

    /* Event handler — processes event_bus queue (Core 1) */
    xTaskCreatePinnedToCore(
        event_handler_task,
        "evt_handler",
        4096,
        NULL,
        3,
        NULL,
        1
    );
}

/* -------------------------------------------------------------------------
 * app_main — ESP-IDF entry point
 * ------------------------------------------------------------------------- */
/* Periodic heartbeat so the web monitor always shows activity even if
 * boot messages were missed due to USB CDC enumeration timing.            */
static void heartbeat_task(void *arg)
{
    uint32_t tick = 0;
    for (;;) {
        ESP_LOGI(TAG, "♥ heartbeat #%lu — WiFi:%s WS:%s heap:%lu",
                 (unsigned long)++tick,
                 wifi_is_connected()      ? "up"   : "down",
                 ws_client_is_connected() ? "up"   : "down",
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    /*
     * Wait for USB-Serial/JTAG CDC to enumerate and for the host monitor
     * to reconnect (DTR assert) before printing any logs.  Without this
     * delay the ESP32-S3 drops the startup messages because the CDC TX
     * FIFO is full before the host terminal opens the port.
     */
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "=== ESP32-S3 Voice Companion boot ===");

    /* Layer 1: Board HAL */
    ESP_ERROR_CHECK(board_init());

    /* Layer 2: System services */
    system_init();

    /* Layer 3: Application framework */
    application_init_all();

    /* Layer 4: Apps */
    ESP_ERROR_CHECK(launcher_app_init());
    ESP_ERROR_CHECK(chat_app_init());
    ESP_ERROR_CHECK(settings_app_init());
    ESP_ERROR_CHECK(app_registry_init());
    /* Start at the home launcher */
    ESP_ERROR_CHECK(app_registry_launch("launcher"));

    /* Kick off tasks */
    application_run();

    /* Periodic heartbeat visible in the web monitor */
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "Boot complete — scheduler running");
    /* app_main returns; FreeRTOS scheduler owns the CPU */
}
