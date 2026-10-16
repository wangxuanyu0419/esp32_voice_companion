#pragma once

#include "esp_err.h"

esp_err_t remote_image_init(void);
esp_err_t remote_image_fetch(const char *url, const char *caption);
