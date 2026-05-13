#pragma once

#include "app_interface.h"
#include "esp_err.h"

app_t    *media_player_app_get(void);
esp_err_t media_player_app_init(void);

