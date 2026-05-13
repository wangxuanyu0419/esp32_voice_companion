#include "media_player_app.h"

#include "app_registry.h"
#include "audio_pipeline.h"
#include "display_driver.h"
#include "media_storage.h"
#include "ui_fonts.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "MEDIA_PLAYER";

void lv_split_jpeg_init(void);
void lv_png_init(void);

#define MEDIA_ROOT         "/sdcard"
#define MEDIA_DIR          "/sdcard/media"
#define LVGL_SD_PREFIX     "S:"
#define MAX_MEDIA_FILES    32
#define PATH_MAX_LEN       128
#define STATUS_BAR_H       38
#define IMAGE_W            DISPLAY_H_RES
#define IMAGE_H            (DISPLAY_V_RES - STATUS_BAR_H)
#define WAV_BUF_SAMPLES    1024

typedef struct {
    char images[MAX_MEDIA_FILES][PATH_MAX_LEN];
    char wavs[MAX_MEDIA_FILES][PATH_MAX_LEN];
    char videos[MAX_MEDIA_FILES][PATH_MAX_LEN];
    int  image_count;
    int  wav_count;
    int  video_count;
    int  image_index;
    int  wav_index;
} media_list_t;

typedef struct {
    char     path[PATH_MAX_LEN];
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_info_t;

static lv_obj_t      *s_screen;
static lv_obj_t      *s_status_bar;
static lv_obj_t      *s_img;
static lv_obj_t      *s_title;
static lv_obj_t      *s_subtitle;
static lv_timer_t    *s_slide_timer;
static media_list_t   s_media;
static TaskHandle_t   s_player_task;
static volatile bool  s_stop_playback;
static volatile bool  s_is_playing;
static bool           s_image_decoders_ready;

static bool ends_with_ci(const char *name, const char *ext)
{
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if (nl < el) return false;
    name += nl - el;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void set_message(const char *title, const char *subtitle)
{
    if (s_title) lv_label_set_text(s_title, title ? title : "");
    if (s_subtitle) lv_label_set_text(s_subtitle, subtitle ? subtitle : "");
}

static void media_scan_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s: errno=%d", dir_path, errno);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[PATH_MAX_LEN];
        int n = snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if ((ends_with_ci(ent->d_name, ".jpg") ||
             ends_with_ci(ent->d_name, ".jpeg") ||
             ends_with_ci(ent->d_name, ".png")) &&
            s_media.image_count < MAX_MEDIA_FILES) {
            strlcpy(s_media.images[s_media.image_count++], full, PATH_MAX_LEN);
        } else if (ends_with_ci(ent->d_name, ".wav") && s_media.wav_count < MAX_MEDIA_FILES) {
            strlcpy(s_media.wavs[s_media.wav_count++], full, PATH_MAX_LEN);
        } else if (ends_with_ci(ent->d_name, ".mp4") && s_media.video_count < MAX_MEDIA_FILES) {
            strlcpy(s_media.videos[s_media.video_count++], full, PATH_MAX_LEN);
        }
    }
    closedir(dir);
}

static void media_scan(void)
{
    memset(&s_media, 0, sizeof(s_media));
    mkdir(MEDIA_DIR, 0775);

    media_scan_dir(MEDIA_DIR);
    media_scan_dir(MEDIA_ROOT);

    ESP_LOGI(TAG, "Media scan: %d images, %d wav, %d mp4",
             s_media.image_count, s_media.wav_count, s_media.video_count);
}

static void show_current_image(void)
{
    if (!s_img) return;

    if (s_media.image_count <= 0) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        set_message("No images", "/media/*.JPG or *.PNG");
        return;
    }

    if (s_media.image_index >= s_media.image_count) s_media.image_index = 0;

    static char lv_path[PATH_MAX_LEN + 3];
    snprintf(lv_path, sizeof(lv_path), "%s%s", LVGL_SD_PREFIX,
             s_media.images[s_media.image_index]);

    lv_img_header_t header;
    uint16_t zoom = LV_IMG_ZOOM_NONE;
    const char *fit = "native";
    lv_res_t info_res = lv_img_decoder_get_info(lv_path, &header);
    if (info_res == LV_RES_OK && header.w > 0 && header.h > 0) {
        uint32_t zoom_x = ((uint32_t)IMAGE_W * LV_IMG_ZOOM_NONE) / header.w;
        uint32_t zoom_y = ((uint32_t)IMAGE_H * LV_IMG_ZOOM_NONE) / header.h;
        uint32_t fit_zoom = zoom_x < zoom_y ? zoom_x : zoom_y;
        if (fit_zoom < LV_IMG_ZOOM_NONE) {
            if (fit_zoom == 0) fit_zoom = 1;
            zoom = (uint16_t)fit_zoom;
            fit = "fit";
        }
    } else {
        ESP_LOGW(TAG, "Image decode info failed: %s", lv_path);
    }

    lv_img_set_src(s_img, lv_path);
    lv_img_set_zoom(s_img, zoom);
    lv_img_set_antialias(s_img, true);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_img);

    char title[64];
    snprintf(title, sizeof(title), "%d/%d  %s",
             s_media.image_index + 1, s_media.image_count,
             base_name(s_media.images[s_media.image_index]));
    char subtitle[80];
    snprintf(subtitle, sizeof(subtitle), "%s%s%s%s",
             info_res == LV_RES_OK ? (s_is_playing ? "Playing WAV" : "Tap image: next  Play: WAV") : "Decode failed",
             strcmp(fit, "fit") == 0 ? "  scaled to fit" : "",
             info_res == LV_RES_OK ? "" : "  check format",
             s_media.video_count > 0 ? "  MP4 ignored" : "");
    set_message(title, subtitle);
}

static void next_image(void)
{
    if (s_media.image_count <= 0) return;
    s_media.image_index = (s_media.image_index + 1) % s_media.image_count;
    show_current_image();
}

static void slide_cb(lv_timer_t *timer)
{
    (void)timer;
    next_image();
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t parse_wav(FILE *f, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return ESP_FAIL;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool have_fmt = false;
    bool have_data = false;
    while (!have_data) {
        uint8_t chdr[8];
        if (fread(chdr, 1, sizeof(chdr), f) != sizeof(chdr)) break;
        uint32_t size = rd32(chdr + 4);

        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
                return ESP_FAIL;
            }
            uint16_t audio_format = rd16(fmt);
            info->channels = rd16(fmt + 2);
            info->sample_rate = rd32(fmt + 4);
            info->bits_per_sample = rd16(fmt + 14);
            if (audio_format != 1 || info->bits_per_sample != 16 ||
                info->sample_rate != AUDIO_SAMPLE_RATE ||
                (info->channels != 1 && info->channels != 2)) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            have_fmt = true;
            if (size > sizeof(fmt)) fseek(f, size - sizeof(fmt), SEEK_CUR);
        } else if (memcmp(chdr, "data", 4) == 0) {
            info->data_offset = ftell(f);
            info->data_size = size;
            have_data = true;
        } else {
            fseek(f, size, SEEK_CUR);
        }

        if (size & 1) fseek(f, 1, SEEK_CUR);
    }

    return (have_fmt && have_data) ? ESP_OK : ESP_FAIL;
}

static void wav_player_task(void *arg)
{
    wav_info_t info;
    memcpy(&info, arg, sizeof(info));
    free(arg);

    FILE *f = fopen(info.path, "rb");
    if (!f) goto done;
    if (fseek(f, info.data_offset, SEEK_SET) != 0) goto close_done;

    i2s_chan_handle_t tx = audio_get_tx_handle();
    if (!tx) goto close_done;

    int16_t *in = heap_caps_malloc(WAV_BUF_SAMPLES * info.channels * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_malloc(WAV_BUF_SAMPLES * 2 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in || !out) {
        free(in);
        free(out);
        goto close_done;
    }

    s_is_playing = true;
    uint32_t left = info.data_size;
    while (!s_stop_playback && left > 0) {
        size_t want = WAV_BUF_SAMPLES * info.channels * sizeof(int16_t);
        if (want > left) want = left;
        size_t got = fread(in, 1, want, f);
        if (got == 0) break;
        left -= got;

        size_t frames = got / (info.channels * sizeof(int16_t));
        for (size_t i = 0; i < frames; i++) {
            int16_t l = in[i * info.channels];
            int16_t r = (info.channels == 2) ? in[i * 2 + 1] : l;
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }

        size_t written = 0;
        i2s_channel_write(tx, out, frames * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(1000));
    }

    free(in);
    free(out);

close_done:
    fclose(f);
done:
    s_is_playing = false;
    s_stop_playback = false;
    s_player_task = NULL;
    vTaskDelete(NULL);
}

static void stop_wav(void)
{
    s_stop_playback = true;
}

static void play_current_wav(void)
{
    if (s_media.wav_count <= 0) {
        set_message("No WAV files", "/media/*.WAV");
        return;
    }
    if (s_player_task) {
        stop_wav();
        return;
    }
    if (audio_is_capturing()) audio_stop_capture();

    wav_info_t *info = calloc(1, sizeof(*info));
    if (!info) return;
    strlcpy(info->path, s_media.wavs[s_media.wav_index], sizeof(info->path));

    FILE *f = fopen(info->path, "rb");
    if (!f) {
        free(info);
        return;
    }
    esp_err_t ret = parse_wav(f, info);
    fclose(f);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unsupported WAV: %s (%s)", info->path, esp_err_to_name(ret));
        set_message("Unsupported WAV", "Use PCM16 16kHz mono/stereo");
        free(info);
        return;
    }

    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "Playing %s", base_name(info->path));
    if (s_subtitle) lv_label_set_text(s_subtitle, subtitle);

    if (xTaskCreatePinnedToCore(wav_player_task, "wav_player", 4096, info,
                                5, &s_player_task, 1) != pdPASS) {
        free(info);
        s_player_task = NULL;
    }
}

static void img_click_cb(lv_event_t *e)
{
    (void)e;
    next_image();
}

static void play_click_cb(lv_event_t *e)
{
    (void)e;
    play_current_wav();
}

static void next_wav_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_media.wav_count <= 0) return;
    s_media.wav_index = (s_media.wav_index + 1) % s_media.wav_count;
    char msg[64];
    snprintf(msg, sizeof(msg), "Selected %s", base_name(s_media.wavs[s_media.wav_index]));
    if (s_subtitle) lv_label_set_text(s_subtitle, msg);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, lv_event_cb_t cb)
{
    const ui_theme_t *th = ui_theme_get();
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 88, 34);
    lv_obj_set_pos(btn, x, 38);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_bg_color(btn, th->card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, th->border, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, th->text, 0);
    lv_obj_set_style_text_font(lbl, UI_FONT_TEXT_SM, 0);
    lv_obj_center(lbl);
    return btn;
}

static void build_ui(void)
{
    const ui_theme_t *th = ui_theme_get();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_status_bar = ui_status_bar_create(s_screen);

    lv_obj_t *stage = lv_obj_create(s_screen);
    lv_obj_set_size(stage, IMAGE_W, IMAGE_H);
    lv_obj_set_pos(stage, 0, STATUS_BAR_H);
    lv_obj_set_style_bg_color(stage, lv_color_black(), 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_img_create(stage);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img, img_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *overlay = lv_obj_create(stage);
    lv_obj_set_size(overlay, IMAGE_W, 76);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(overlay);
    lv_label_set_text(s_title, "Media Player");
    lv_obj_set_style_text_color(s_title, th->text, 0);
    lv_obj_set_style_text_font(s_title, UI_FONT_TEXT_SM, 0);
    lv_obj_set_width(s_title, IMAGE_W - 20);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 10, 4);

    s_subtitle = lv_label_create(overlay);
    lv_label_set_text(s_subtitle, "");
    lv_obj_set_style_text_color(s_subtitle, th->text_dim, 0);
    lv_obj_set_style_text_font(s_subtitle, UI_FONT_TEXT_SM, 0);
    lv_obj_set_width(s_subtitle, IMAGE_W - 20);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_DOT);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_LEFT, 10, 25);

    make_button(overlay, "Play", 12, play_click_cb);
    make_button(overlay, "Next WAV", 110, next_wav_click_cb);
    make_button(overlay, "Next JPG", 208, img_click_cb);

    s_slide_timer = lv_timer_create(slide_cb, 5000, NULL);
    lv_timer_pause(s_slide_timer);
}

static esp_err_t media_on_enter(app_t *self)
{
    (void)self;
    if (!s_image_decoders_ready) {
        lv_split_jpeg_init();
        lv_png_init();
        s_image_decoders_ready = true;
    }
    if (!s_screen) build_ui();

    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());

    esp_err_t ret = media_storage_mount();
    if (ret != ESP_OK) {
        set_message("TF card mount failed", esp_err_to_name(ret));
    } else {
        media_scan();
        show_current_image();
        if (s_slide_timer) lv_timer_resume(s_slide_timer);
    }

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    return ESP_OK;
}

static void media_on_exit(app_t *self)
{
    (void)self;
    if (s_slide_timer) lv_timer_pause(s_slide_timer);
    stop_wav();
}

static void media_on_event(app_t *self, const event_t *evt)
{
    (void)self;
    if (!s_status_bar || !evt) return;
    switch (evt->type) {
        case EVT_WIFI_CONNECTED:
        case EVT_WIFI_DISCONNECTED:
            ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
            break;
        case EVT_WS_CONNECTED:
        case EVT_WS_DISCONNECTED:
            ui_status_bar_set_ws(s_status_bar, ws_client_is_connected());
            break;
        default:
            break;
    }
}

static app_t s_app = {
    .id       = "media",
    .name     = "Media",
    .on_enter = media_on_enter,
    .on_exit  = media_on_exit,
    .on_event = media_on_event,
    .priv     = NULL,
};

app_t *media_player_app_get(void)
{
    return &s_app;
}

esp_err_t media_player_app_init(void)
{
    return ESP_OK;
}
