#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char server_url[128];
    char device_id[32];
    char agent[16];
    int  volume;
    bool auto_play_tts;
    bool beep_feedback;
    /* Authentication — stored in NVS, sent as WebSocket headers */
    char cf_client_id[64];      /* CF-Access-Client-Id (Cloudflare Zero Trust) */
    char cf_client_secret[128]; /* CF-Access-Client-Secret */
    char ws_auth_token[128];    /* Authorization: Bearer <token> (server-side) */
    /* Sleep / display power management */
    uint16_t dim_timeout_s;   /* Seconds of inactivity before dimming.  0 = never. */
    uint16_t sleep_timeout_s; /* Seconds of inactivity before screen off. 0 = never. */
    /* Display theme */
    bool dark_theme;          /* true = dark (default), false = light */
} app_config_t;

#define DEFAULT_DEVICE_ID       "esp32s3_001"
#define DEFAULT_AGENT           "main"
#define DEFAULT_SERVER_URL      "wss://clawchat.xuanyu.uk/ws"
#define DEFAULT_VOLUME          80
#define DEFAULT_AUTO_PLAY_TTS   true
#define DEFAULT_BEEP_FEEDBACK   true
#define DEFAULT_DIM_TIMEOUT_S   30    /* dim after 30 s */
#define DEFAULT_SLEEP_TIMEOUT_S 120   /* screen off after 2 min */
#define DEFAULT_DARK_THEME      true  /* dark by default */

/* ── 可选服务器主机列表（按优先级排列） ──────────────────────────────────── */
typedef struct {
    const char *label;   /* 显示名 */
    const char *host;    /* 主机名，用于派生 ws/https URL */
} server_host_t;

extern const server_host_t g_server_hosts[];
extern const int           g_server_hosts_count;

esp_err_t config_init(void);
esp_err_t config_get(app_config_t *cfg);
esp_err_t config_save(const app_config_t *cfg);
esp_err_t config_reset(void);
esp_err_t config_generate_device_id(char *id, size_t len);

/* 根据 cfg.server_url 的主机名派生 TTS 流地址 "https://<host>/stream?text="。
 * 返回写入字节数（不含 \0），失败返回 -1。 */
int  config_get_tts_base_url(char *out, size_t len);

/* 返回当前 server_url 主机在 g_server_hosts 中的索引，找不到返回 0。 */
int  config_get_server_host_idx(void);

/* 设置服务器主机（idx 指向 g_server_hosts），写入 server_url 并保存。 */
esp_err_t config_set_server_host(int idx);

#endif /* CONFIG_STORE_H */
