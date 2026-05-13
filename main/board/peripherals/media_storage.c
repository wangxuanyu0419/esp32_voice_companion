#include "media_storage.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "MEDIA_STORAGE";

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.8 TF slot.
 * The board examples use 1-bit SD/MMC for ESP-IDF. These pins correspond to
 * the same physical lines exposed as SCK/CMD/D0 in the Arduino SD examples.
 */
#define SD_CLK_IO GPIO_NUM_2
#define SD_CMD_IO GPIO_NUM_1
#define SD_D0_IO  GPIO_NUM_3

static sdmmc_card_t *s_card;
static bool          s_mounted;

esp_err_t media_storage_mount(void)
{
    if (s_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Mounting TF card at %s", MEDIA_STORAGE_MOUNT_POINT);

    gpio_set_pull_mode(SD_CMD_IO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_D0_IO, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = SD_CLK_IO;
    slot_cfg.cmd = SD_CMD_IO;
    slot_cfg.d0 = SD_D0_IO;
    slot_cfg.d1 = GPIO_NUM_NC;
    slot_cfg.d2 = GPIO_NUM_NC;
    slot_cfg.d3 = GPIO_NUM_NC;
    slot_cfg.d4 = GPIO_NUM_NC;
    slot_cfg.d5 = GPIO_NUM_NC;
    slot_cfg.d6 = GPIO_NUM_NC;
    slot_cfg.d7 = GPIO_NUM_NC;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        MEDIA_STORAGE_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TF mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool media_storage_is_mounted(void)
{
    return s_mounted;
}

const char *media_storage_mount_point(void)
{
    return MEDIA_STORAGE_MOUNT_POINT;
}
