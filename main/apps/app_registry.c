#include "app_registry.h"
#include "chat_app.h"
#include "launcher_app.h"
#include "settings_app.h"
#include "color_test.h"
#include "media_player_app.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_REGISTRY";

static app_t *s_apps[APP_REGISTRY_MAX] = {0};
static int    s_count                   = 0;
static app_t *s_active                  = NULL;

esp_err_t app_registry_init(void)
{
    s_count  = 0;
    s_active = NULL;
    memset(s_apps, 0, sizeof(s_apps));

    /* Register built-in apps */
    app_registry_register(launcher_app_get());
    app_registry_register(chat_app_get());
    app_registry_register(settings_app_get());
    app_registry_register(media_player_app_get());
    app_registry_register(color_test_app_get());

    ESP_LOGI(TAG, "App registry ready (%d apps)", s_count);
    return ESP_OK;
}

esp_err_t app_registry_register(app_t *app)
{
    if (!app || s_count >= APP_REGISTRY_MAX) return ESP_ERR_NO_MEM;
    s_apps[s_count++] = app;
    ESP_LOGI(TAG, "Registered app: %s", app->id);
    return ESP_OK;
}

app_t *app_registry_get(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i]->id, id) == 0) return s_apps[i];
    }
    return NULL;
}

app_t *app_registry_get_active(void) { return s_active; }

esp_err_t app_registry_launch(const char *id)
{
    app_t *app = app_registry_get(id);
    if (!app) {
        ESP_LOGE(TAG, "App not found: %s", id);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_active && s_active->on_exit) {
        s_active->on_exit(s_active);
    }

    s_active = app;
    ESP_LOGI(TAG, "Launching app: %s", app->id);

    if (app->on_enter) {
        return app->on_enter(app);
    }
    return ESP_OK;
}
