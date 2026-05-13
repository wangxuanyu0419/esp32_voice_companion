#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define MEDIA_STORAGE_MOUNT_POINT "/sdcard"

esp_err_t    media_storage_mount(void);
bool         media_storage_is_mounted(void);
const char  *media_storage_mount_point(void);

