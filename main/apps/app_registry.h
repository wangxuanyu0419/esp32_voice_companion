#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include "app_interface.h"
#include "esp_err.h"

#define APP_REGISTRY_MAX 8

esp_err_t app_registry_init(void);
esp_err_t app_registry_register(app_t *app);
app_t    *app_registry_get(const char *id);
app_t    *app_registry_get_active(void);
esp_err_t app_registry_launch(const char *id);

#endif /* APP_REGISTRY_H */
