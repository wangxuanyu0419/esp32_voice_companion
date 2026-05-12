#include "config_store.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG           = "CONFIG_STORE";
static const char *NVS_NAMESPACE = "esp32_voice";

esp_err_t config_init(void)
{
    ESP_LOGI(TAG, "Config store initialising...");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, creating default config");
        config_reset();
    } else {
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Config store ready");
    return ESP_OK;
}

esp_err_t config_get(app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t len;

    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", cfg->wifi_ssid, &len);

    len = sizeof(cfg->wifi_password);
    nvs_get_str(handle, "wifi_pass", cfg->wifi_password, &len);

    len = sizeof(cfg->server_url);
    nvs_get_str(handle, "server_url", cfg->server_url, &len);

    len = sizeof(cfg->device_id);
    nvs_get_str(handle, "device_id", cfg->device_id, &len);

    len = sizeof(cfg->agent);
    nvs_get_str(handle, "agent", cfg->agent, &len);

    len = sizeof(cfg->cf_client_id);
    nvs_get_str(handle, "cf_client_id", cfg->cf_client_id, &len);

    len = sizeof(cfg->cf_client_secret);
    nvs_get_str(handle, "cf_secret", cfg->cf_client_secret, &len);

    len = sizeof(cfg->ws_auth_token);
    nvs_get_str(handle, "ws_token", cfg->ws_auth_token, &len);

    {
        int32_t tmp_vol  = 0;
        int32_t tmp_auto = 0;
        int32_t tmp_beep = 0;
        nvs_get_i32(handle, "volume",    &tmp_vol);
        nvs_get_i32(handle, "auto_play", &tmp_auto);
        nvs_get_i32(handle, "beep",      &tmp_beep);
        cfg->volume        = (int)tmp_vol;
        cfg->auto_play_tts = (bool)tmp_auto;
        cfg->beep_feedback = (bool)tmp_beep;
    }

    /* Sleep timeouts — check return code so 0 ("never") is stored correctly */
    {
        int32_t tmp = -1;
        cfg->dim_timeout_s   = (nvs_get_i32(handle, "dim_t",   &tmp) == ESP_OK)
                               ? (uint16_t)tmp : DEFAULT_DIM_TIMEOUT_S;
        tmp = -1;
        cfg->sleep_timeout_s = (nvs_get_i32(handle, "sleep_t", &tmp) == ESP_OK)
                               ? (uint16_t)tmp : DEFAULT_SLEEP_TIMEOUT_S;
        tmp = DEFAULT_DARK_THEME ? 1 : 0;
        nvs_get_i32(handle, "dark_theme", &tmp);
        cfg->dark_theme = (bool)tmp;
    }

    nvs_close(handle);

    if (strlen(cfg->device_id) == 0)
        strncpy(cfg->device_id, DEFAULT_DEVICE_ID, sizeof(cfg->device_id) - 1);
    if (strlen(cfg->agent) == 0)
        strncpy(cfg->agent, DEFAULT_AGENT, sizeof(cfg->agent) - 1);
    if (strlen(cfg->server_url) == 0)
        strncpy(cfg->server_url, DEFAULT_SERVER_URL, sizeof(cfg->server_url) - 1);
    if (cfg->volume == 0)
        cfg->volume = DEFAULT_VOLUME;

    ESP_LOGI(TAG, "Config loaded: device=%s agent=%s server=%s",
             cfg->device_id, cfg->agent, cfg->server_url);
    return ESP_OK;
}

esp_err_t config_save(const app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = nvs_set_str(handle, "wifi_ssid",  cfg->wifi_ssid))    != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "wifi_pass",  cfg->wifi_password)) != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "server_url", cfg->server_url))    != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "device_id",  cfg->device_id))     != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "agent",      cfg->agent))         != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "volume",     cfg->volume))        != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "auto_play",  cfg->auto_play_tts ? 1 : 0)) != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "beep",       cfg->beep_feedback  ? 1 : 0)) != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "cf_client_id", cfg->cf_client_id))    != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "cf_secret",    cfg->cf_client_secret)) != ESP_OK) goto out;
    if ((ret = nvs_set_str(handle, "ws_token",     cfg->ws_auth_token))    != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "dim_t",     (int32_t)cfg->dim_timeout_s))   != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "sleep_t",   (int32_t)cfg->sleep_timeout_s)) != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "dark_theme",(int32_t)(cfg->dark_theme ? 1 : 0))) != ESP_OK) goto out;
    ret = nvs_commit(handle);

out:
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t config_reset(void)
{
    app_config_t cfg = {0};
    strncpy(cfg.device_id,   DEFAULT_DEVICE_ID,  sizeof(cfg.device_id)   - 1);
    strncpy(cfg.agent,       DEFAULT_AGENT,       sizeof(cfg.agent)       - 1);
    strncpy(cfg.server_url,  DEFAULT_SERVER_URL,  sizeof(cfg.server_url)  - 1);
    cfg.volume          = DEFAULT_VOLUME;
    cfg.auto_play_tts   = DEFAULT_AUTO_PLAY_TTS;
    cfg.beep_feedback   = DEFAULT_BEEP_FEEDBACK;
    cfg.dim_timeout_s   = DEFAULT_DIM_TIMEOUT_S;
    cfg.sleep_timeout_s = DEFAULT_SLEEP_TIMEOUT_S;
    cfg.dark_theme      = DEFAULT_DARK_THEME;
    return config_save(&cfg);
}

esp_err_t config_generate_device_id(char *id, size_t len)
{
    uint8_t mac[6];
    if (esp_base_mac_addr_get(mac) != ESP_OK) {
        snprintf(id, len, "esp32s3_%08" PRIx32, (uint32_t)esp_random());
        return ESP_OK;
    }
    snprintf(id, len, "esp32s3_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}
