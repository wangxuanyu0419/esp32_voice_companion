#include "config_store.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG           = "CONFIG_STORE";
static const char *NVS_NAMESPACE = "esp32_voice";
static const char *VOLUME_MIGRATION_KEY = "vol_mig";

/* The old firmware stored the UI value but always drove the codec at the
 * fixed DAC32=0xCB level. Version 1 moved the value to 24, which maps to
 * DAC32=0xB6 (about 30% of the old amplitude). Version 2 moves the current
 * device to 12 (about half that amplitude) exactly once. Explicit custom
 * values are preserved, while both historical defaults are recognised. */
static esp_err_t migrate_audio_volume_once(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    int32_t version = 0;
    ret = nvs_get_i32(handle, VOLUME_MIGRATION_KEY, &version);
    if (ret == ESP_OK && version >= AUDIO_VOLUME_MIGRATION_VERSION) {
        nvs_close(handle);
        return ESP_OK;
    }

    int32_t old_volume = 0;
    const esp_err_t old_ret = nvs_get_i32(handle, "volume", &old_volume);
    int32_t target_volume = DEFAULT_VOLUME;
    if (old_ret == ESP_OK && old_volume > 0 &&
        old_volume != LEGACY_DEFAULT_VOLUME &&
        old_volume != PREVIOUS_DEFAULT_VOLUME) {
        /* A user-selected value is not a default; do not silently scale it. */
        target_volume = old_volume;
        if (target_volume > 100) target_volume = 100;
    }
    ret = nvs_set_i32(handle, "volume", target_volume);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(handle, VOLUME_MIGRATION_KEY,
                          AUDIO_VOLUME_MIGRATION_VERSION);
    }
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Audio volume migration: %ld -> %ld (one-time)",
                 (long)((old_ret == ESP_OK) ? old_volume : 0),
                 (long)target_volume);
    } else {
        ESP_LOGW(TAG, "Audio volume migration failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ── 可选服务器主机列表（第一个为默认/优先） ────────────────────────────── */
const server_host_t g_server_hosts[] = {
    { "clawchat.xuanyu.uk",     "clawchat.xuanyu.uk"     },
    { "clawchat_mac.xuanyu.uk", "clawchat_mac.xuanyu.uk" },
};
const int g_server_hosts_count =
    (int)(sizeof(g_server_hosts) / sizeof(g_server_hosts[0]));

/* 从 "wss://host/ws" 或 "https://host/..." 中提取主机名到 out。 */
static void extract_host(const char *url, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && i < out_len - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

int config_get_tts_base_url(char *out, size_t len)
{
    app_config_t cfg = {0};
    if (config_get(&cfg) != ESP_OK) return -1;
    char host[96];
    extract_host(cfg.server_url, host, sizeof(host));
    if (host[0] == '\0') return -1;
    int n = snprintf(out, len, "https://%s/stream?text=", host);
    return (n > 0 && (size_t)n < len) ? n : -1;
}

int config_get_server_host_idx(void)
{
    app_config_t cfg = {0};
    if (config_get(&cfg) != ESP_OK) return 0;
    char host[96];
    extract_host(cfg.server_url, host, sizeof(host));
    for (int i = 0; i < g_server_hosts_count; i++) {
        if (strcmp(host, g_server_hosts[i].host) == 0) return i;
    }
    return 0;
}

esp_err_t config_set_server_host(int idx)
{
    if (idx < 0 || idx >= g_server_hosts_count) return ESP_ERR_INVALID_ARG;
    app_config_t cfg = {0};
    esp_err_t ret = config_get(&cfg);
    if (ret != ESP_OK) return ret;
    snprintf(cfg.server_url, sizeof(cfg.server_url),
             "wss://%s/ws", g_server_hosts[idx].host);
    ESP_LOGI(TAG, "Server host → %s (%s)", g_server_hosts[idx].label, cfg.server_url);
    return config_save(&cfg);
}

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

    /* This is a bounded, one-time update of only volume and its version key. */
    ret = migrate_audio_volume_once();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio volume migration skipped: %s", esp_err_to_name(ret));
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
        int32_t tmp_vad  = DEFAULT_VAD_THRESHOLD;
        int32_t tmp_vad_auto = DEFAULT_VAD_AUTO ? 1 : 0;
        int32_t tmp_auto = 0;
        int32_t tmp_beep = 0;
        nvs_get_i32(handle, "volume",    &tmp_vol);
        nvs_get_i32(handle, "vad_threshold", &tmp_vad);
        nvs_get_i32(handle, "vad_auto", &tmp_vad_auto);
        nvs_get_i32(handle, "auto_play", &tmp_auto);
        nvs_get_i32(handle, "beep",      &tmp_beep);
        cfg->volume        = (int)tmp_vol;
        cfg->vad_threshold = (int)tmp_vad;
        cfg->vad_auto_threshold = (bool)tmp_vad_auto;
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
    if (cfg->volume < 0) cfg->volume = 0;
    if (cfg->volume > 100) cfg->volume = 100;
    if (cfg->vad_threshold < MIN_VAD_THRESHOLD)
        cfg->vad_threshold = MIN_VAD_THRESHOLD;
    if (cfg->vad_threshold > MAX_VAD_THRESHOLD)
        cfg->vad_threshold = MAX_VAD_THRESHOLD;

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
    if ((ret = nvs_set_i32(handle, "vad_threshold", cfg->vad_threshold)) != ESP_OK) goto out;
    if ((ret = nvs_set_i32(handle, "vad_auto", cfg->vad_auto_threshold ? 1 : 0)) != ESP_OK) goto out;
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
    cfg.vad_threshold   = DEFAULT_VAD_THRESHOLD;
    cfg.vad_auto_threshold = DEFAULT_VAD_AUTO;
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
