/*
 * scene_avatar.c — ClawChat main screen.
 *
 * Layout (368×448):
 *   [status bar 38px]
 *   [Nahida state image 200×200, centred]   ← tap → toggle recording
 *   [user speech bubble  — green, right-aligned, hidden until stt_final]
 *   [assistant text bubble — grey, left-aligned, hidden until assistant_text]
 *   [state label: 请说话... / 思考中... / ...]
 *
 * Call avatar_set_state() from any task to update the image + label.
 * All LVGL writes happen on the LVGL task via lv_async_call.
 */

#include "scene_avatar.h"
#include "img_assets.h"
#include "display_driver.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "font_manager.h"
#include "ui_status_bar.h"
#include "wifi_manager.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* LVGL's existing decoders are also used by media_player.  Keep image
 * decoding on the LVGL task and only retain the compressed source in PSRAM. */
void lv_split_jpeg_init(void);
void lv_png_init(void);

static const char *TAG = "SCENE_AVATAR";

/* ── UTF-8 emoji / unsupported-char stripper ──────────────────────────────── *
 * Returns a malloc'd copy of src with characters stripped that our fonts     *
 * cannot render (emoji, misc symbols, variation selectors).                  *
 * Kept characters: ASCII (< 0x80), full CJK block, CJK punctuation,         *
 *   Hiragana/Katakana, fullwidth forms.                                      *
 * Caller must free() the returned pointer.                                   *
 * -------------------------------------------------------------------------- */
static char *strip_unsupported(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) return strdup(src);   /* OOM fallback: return original */

    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;

    while (*s) {
        uint8_t b = *s;

        if (b < 0x80) {
            /* ASCII — always keep */
            *d++ = *s++;

        } else if ((b & 0xE0) == 0xC0) {
            /* 2-byte sequence (U+0080–U+07FF) — keep Latin ext / diacritics */
            if (s[1]) { *d++ = *s++; *d++ = *s++; } else { s++; }

        } else if ((b & 0xF0) == 0xE0) {
            /* 3-byte sequence (U+0800–U+FFFF) */
            if (s[1] && s[2]) {
                uint32_t cp = (uint32_t)(b & 0x0F) << 12
                            | (uint32_t)(s[1] & 0x3F) << 6
                            | (uint32_t)(s[2] & 0x3F);

                /* Strip: Misc Symbols U+2600–U+26FF, Dingbats U+2700–U+27FF,
                 *        Misc Symbols & Arrows U+2B00–U+2BFF,
                 *        Enclosed Alphanumerics U+2460–U+24FF,
                 *        Variation selectors U+FE00–U+FE0F,
                 *        Enclosed CJK U+3200–U+32FF,
                 *        Supplemental Arrows-B U+2900–U+297F */
                if ((cp >= 0x2460 && cp <= 0x24FF) ||
                    (cp >= 0x2600 && cp <= 0x27FF) ||
                    (cp >= 0x2900 && cp <= 0x297F) ||
                    (cp >= 0x2B00 && cp <= 0x2BFF) ||
                    (cp >= 0x3200 && cp <= 0x32FF) ||
                    (cp >= 0xFE00 && cp <= 0xFE0F)) {
                    s += 3; /* skip */
                } else {
                    *d++ = *s++; *d++ = *s++; *d++ = *s++;
                }
            } else { s++; }

        } else if ((b & 0xF8) == 0xF0) {
            /* 4-byte sequence (U+10000+) — ALL emoji live here; skip */
            if (s[1] && s[2] && s[3]) { s += 4; } else { s++; }

        } else {
            s++; /* invalid byte — skip */
        }
    }
    *d = '\0';
    return dst;
}

/* ── Tap callback (set by chat_app) ───────────────────────────────────────── */
static void (*s_tap_cb)(void) = NULL;
static void (*s_settings_cb)(void) = NULL;

void avatar_set_tap_callback(void (*cb)(void)) { s_tap_cb = cb; }
void avatar_set_settings_callback(void (*cb)(void)) { s_settings_cb = cb; }

/* ── Widget handles ───────────────────────────────────────────────────────── */
static bool      s_initialized    = false;
static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_status_bar     = NULL;
static lv_obj_t *s_settings_btn   = NULL;
static lv_obj_t *s_state_img      = NULL;   /* Nahida image 200×200 */
static lv_obj_t *s_msg_list       = NULL;   /* scrollable chat history (user+assistant bubbles) */
static lv_obj_t *s_last_assistant_bubble = NULL; /* newest assistant bubble container, for streaming updates */
static lv_obj_t *s_last_assistant_lbl    = NULL; /* label inside s_last_assistant_bubble */
static bool       s_assistant_turn_open  = true; /* true => next assistant text opens a NEW bubble */
static lv_timer_t *s_msg_expiry_timer = NULL;    /* periodic sweep for MSG_EXPIRE_US-old bubbles */
static lv_obj_t *s_state_lbl      = NULL;   /* 请说话... / 思考中... */

/* Forward declaration: apply_remote_image_lvgl (below) appends a caption
 * bubble to the message list before create_bubble's own definition. */
static lv_obj_t *create_bubble(bool is_user, const char *text, lv_obj_t **out_label);
static lv_obj_t *s_remote_img     = NULL;   /* transient server image */
static uint8_t  *s_remote_data    = NULL;
static lv_img_dsc_t s_remote_dsc;
static lv_timer_t *s_remote_timer = NULL;
static bool        s_decoders_ready = false;
static bool        s_remote_visible = false;
static avatar_emotion_t s_current_emotion = AVATAR_IDLE;
static lv_timer_t *s_emotion_timer = NULL;
static avatar_emotion_t s_emotion_return = AVATAR_IDLE;

/* Avatar size & Y position */
#define AVATAR_SIZE   200
#define STATUS_H       38
#define AVATAR_GAP      4   /* gap between status bar and avatar */
/* Avatar top edge */
#define AVATAR_TOP    (STATUS_H + AVATAR_GAP)
/* Avatar bottom edge */
#define AVATAR_BOT    (AVATAR_TOP + AVATAR_SIZE)   /* = 242 */

/* Bubble layout below the avatar */
#define BUBBLE_MARGIN  16   /* horizontal padding from display edge */
#define BUBBLE_W       (DISPLAY_H_RES - 2 * BUBBLE_MARGIN)

/* Scrollable message history: from just below the avatar down to the
 * state label at the bottom of the screen. */
#define MSG_LIST_TOP   (AVATAR_BOT + 4)
#define MSG_LIST_H     (DISPLAY_V_RES - MSG_LIST_TOP - 28)

/* Messages older than this are swept off the list automatically. */
#define MSG_EXPIRE_US        (5LL * 60 * 1000000)  /* 5 minutes */
#define MSG_EXPIRY_PERIOD_MS 10000                 /* sweep every 10 s */
#define MAX_HISTORY_MESSAGES 30

/* ── State → image + label ────────────────────────────────────────────────── */
typedef struct { const lv_img_dsc_t *img; const char *label; } state_entry_t;

static const state_entry_t k_state_table[] = {
    [APP_STATE_IDLE]      = { &img_state_idle,      ""                                                   },
    [APP_STATE_LISTENING] = { &img_state_listening, "\xe8\xaf\xb7\xe8\xaf\xb4\xe8\xaf\x9d..."           },  /* 请说话... */
    [APP_STATE_THINKING]  = { &img_state_thinking,  "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad..."           },  /* 思考中... */
    [APP_STATE_SPEAKING]  = { &img_state_speaking,  "\xe6\x92\xad\xe6\x94\xbe\xe4\xb8\xad..."           },  /* 播放中... */
    [APP_STATE_ERROR]     = { &img_state_error,     "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x96\xad\xe5\xbc\x80" },  /* 连接断开 */
    [APP_STATE_SLEEP]     = { &img_state_idle,      ""                                                   },
};
#define STATE_TABLE_SIZE  (sizeof(k_state_table) / sizeof(k_state_table[0]))

/* ── Bounded remote image validation/display ─────────────────────────────── */
#define REMOTE_IMAGE_MAX_BYTES (512U * 1024U)
#define REMOTE_IMAGE_MAX_DIM   2048U

typedef struct {
    uint8_t *data;
    size_t   len;
    char     caption[];
} image_arg_t;

static bool image_dimensions(const uint8_t *data, size_t len,
                             uint16_t *out_w, uint16_t *out_h)
{
    if (!data || len == 0 || len > REMOTE_IMAGE_MAX_BYTES || !out_w || !out_h) {
        return false;
    }

    if (len >= 24 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        uint32_t w = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                     ((uint32_t)data[18] << 8) | data[19];
        uint32_t h = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                     ((uint32_t)data[22] << 8) | data[23];
        if (w == 0 || h == 0 || w > REMOTE_IMAGE_MAX_DIM || h > REMOTE_IMAGE_MAX_DIM) {
            return false;
        }
        *out_w = (uint16_t)w;
        *out_h = (uint16_t)h;
        return true;
    }

    if (len >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        size_t i = 2;
        while (i + 3 < len) {
            if (data[i] != 0xFF) { i++; continue; }
            while (i < len && data[i] == 0xFF) i++;
            if (i >= len) break;
            uint8_t marker = data[i++];
            if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
                (marker >= 0xD0 && marker <= 0xD7)) continue;
            if (i + 2 > len) break;
            uint16_t seg_len = ((uint16_t)data[i] << 8) | data[i + 1];
            if (seg_len < 2 || (size_t)seg_len > len - i) break;
            bool sof = ((marker >= 0xC0 && marker <= 0xC3) ||
                        (marker >= 0xC5 && marker <= 0xC7) ||
                        (marker >= 0xC9 && marker <= 0xCB) ||
                        (marker >= 0xCD && marker <= 0xCF));
            if (sof && seg_len >= 7) {
                uint32_t h = ((uint32_t)data[i + 3] << 8) | data[i + 4];
                uint32_t w = ((uint32_t)data[i + 5] << 8) | data[i + 6];
                if (w == 0 || h == 0 || w > REMOTE_IMAGE_MAX_DIM ||
                    h > REMOTE_IMAGE_MAX_DIM) return false;
                *out_w = (uint16_t)w;
                *out_h = (uint16_t)h;
                return true;
            }
            i += seg_len;
        }
    }
    return false;
}

static void clear_remote_image_lvgl(void)
{
    if (s_remote_img) lv_obj_add_flag(s_remote_img, LV_OBJ_FLAG_HIDDEN);
    if (s_remote_data) {
        lv_img_cache_invalidate_src(&s_remote_dsc);
        free(s_remote_data);
        s_remote_data = NULL;
    }
    memset(&s_remote_dsc, 0, sizeof(s_remote_dsc));
    s_remote_visible = false;
    /* Restore the Nahida state image every time the remote image is
     * cleared, from whichever caller triggers it (next tap, a new image
     * arriving, or a screen reset) so callers stay consistent. */
    if (s_state_img) lv_obj_clear_flag(s_state_img, LV_OBJ_FLAG_HIDDEN);
}

static void apply_remote_image_lvgl(void *opaque)
{
    image_arg_t *arg = (image_arg_t *)opaque;
    if (!arg) return;

    uint16_t w = 0, h = 0;
    if (!s_initialized || !s_remote_img ||
        !image_dimensions(arg->data, arg->len, &w, &h)) {
        ESP_LOGW(TAG, "Rejected invalid remote image (%u bytes)",
                 (unsigned)(arg->len));
        free(arg->data);
        free(arg);
        return;
    }

    if (s_remote_timer) {
        lv_timer_del(s_remote_timer);
        s_remote_timer = NULL;
    }
    clear_remote_image_lvgl();
    s_remote_data = arg->data;
    arg->data = NULL;
    memset(&s_remote_dsc, 0, sizeof(s_remote_dsc));
    s_remote_dsc.header.always_zero = 0;
    s_remote_dsc.header.cf = LV_IMG_CF_RAW;
    s_remote_dsc.header.w = w;
    s_remote_dsc.header.h = h;
    s_remote_dsc.data_size = arg->len;
    s_remote_dsc.data = s_remote_data;

    lv_img_cache_invalidate_src(&s_remote_dsc);
    lv_img_set_src(s_remote_img, &s_remote_dsc);
    uint32_t zoom_x = (uint32_t)AVATAR_SIZE * LV_IMG_ZOOM_NONE / w;
    uint32_t zoom_y = (uint32_t)AVATAR_SIZE * LV_IMG_ZOOM_NONE / h;
    uint32_t zoom = zoom_x < zoom_y ? zoom_x : zoom_y;
    if (zoom > LV_IMG_ZOOM_NONE) zoom = LV_IMG_ZOOM_NONE;
    if (zoom < 16) zoom = 16;
    lv_img_set_zoom(s_remote_img, (uint16_t)zoom);
    lv_img_set_antialias(s_remote_img, true);
    lv_obj_align(s_remote_img, LV_ALIGN_TOP_MID, 0, AVATAR_TOP);
    lv_obj_clear_flag(s_remote_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_remote_img);
    lv_obj_add_flag(s_state_img, LV_OBJ_FLAG_HIDDEN);
    s_remote_visible = true;

    if (arg->caption[0]) {
        char *caption = strip_unsupported(arg->caption);
        if (caption) {
            lv_obj_t *lbl = NULL;
            s_last_assistant_bubble = create_bubble(false, caption, &lbl);
            s_last_assistant_lbl    = lbl;
            s_assistant_turn_open   = false;
            if (s_msg_list) lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
            free(caption);
        }
    }
    /* No auto-hide timer: the image stays on screen (replacing the Nahida
     * state image) until the user taps it again — see img_tap_cb(). */
    ESP_LOGI(TAG, "Remote image displayed %ux%u (%u bytes)",
             (unsigned)w, (unsigned)h, (unsigned)arg->len);
    free(arg);
}

/* ── Finite avatar expression mapping ───────────────────────────────────── */
/*
 * The current SPIFFS emotion files are legacy 184x184 colour placeholders
 * (a few hundred bytes each).  Feeding those files to LVGL produces the
 * grey/yellow blocks seen on the device.  Keep the old path table for future
 * real PNGs, but use the verified embedded Nahida descriptors until a real
 * expression pack is installed.  This is a finite, bounded fallback: no
 * arbitrary server path is opened and every emotion remains drawable.
 */
static const lv_img_dsc_t *embedded_emotion_fallback(avatar_emotion_t emotion)
{
    switch (emotion) {
        case AVATAR_SPEAKING:
            return &img_state_speaking;
        case AVATAR_SAD:
        case AVATAR_FEAR:
        case AVATAR_DISGUST:
            return &img_state_thinking;
        case AVATAR_HAPPY:
        case AVATAR_ANGRY:
        case AVATAR_SURPRISED:
        case AVATAR_IDLE:
        default:
            return &img_state_idle;
    }
}

static void apply_emotion_source(avatar_emotion_t emotion)
{
    if (!s_state_img) return;
    if ((unsigned)emotion >= AVATAR_COUNT) emotion = AVATAR_IDLE;
    const lv_img_dsc_t *fallback = embedded_emotion_fallback(emotion);
    lv_img_set_src(s_state_img, fallback);
    lv_img_set_zoom(s_state_img, LV_IMG_ZOOM_NONE);
    lv_img_set_antialias(s_state_img, true);
    s_current_emotion = emotion;
    ESP_LOGI(TAG, "Emotion %d uses embedded Nahida fallback (SPIFFS pack unavailable)",
             (int)emotion);
}

static void emotion_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    s_emotion_timer = NULL;
    apply_emotion_source(s_emotion_return);
}

typedef struct {
    avatar_emotion_t emotion;
    avatar_emotion_t return_emotion;
    int duration_ms;
} emotion_arg_t;

static void apply_emotion_lvgl(void *opaque)
{
    emotion_arg_t *arg = (emotion_arg_t *)opaque;
    if (!arg) return;
    if (s_emotion_timer) {
        lv_timer_del(s_emotion_timer);
        s_emotion_timer = NULL;
    }
    if ((unsigned)arg->emotion >= AVATAR_COUNT) arg->emotion = AVATAR_IDLE;
    if ((unsigned)arg->return_emotion >= AVATAR_COUNT) arg->return_emotion = AVATAR_IDLE;
    /* A remote image on screen must survive emotion updates (and the text/
     * TTS traffic that goes with them) until the user taps to dismiss it.
     * Still track the emotion state and refresh s_state_img's source so it
     * is correct whenever the image eventually closes — apply_emotion_source
     * never touches the HIDDEN flag, so this stays invisible while
     * s_remote_visible is true. */
    s_emotion_return = arg->return_emotion;
    apply_emotion_source(arg->emotion);
    if (arg->duration_ms > 0) {
        s_emotion_timer = lv_timer_create(emotion_timeout_cb, arg->duration_ms, NULL);
    }
    free(arg);
}

/* ── Internal apply (must be called on LVGL task) ─────────────────────────── */
static void apply_state_lvgl(void *arg)
{
    if (!s_initialized) return;
    app_state_t state = (app_state_t)(uintptr_t)arg;
    if ((size_t)state >= STATE_TABLE_SIZE) state = APP_STATE_IDLE;

    const state_entry_t *e = &k_state_table[state];
    lv_img_set_src(s_state_img, e->img);
    lv_label_set_text(s_state_lbl, e->label);

    /* Update status bar indicators */
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar,   ws_client_is_connected());
}

static void apply_text_lvgl(void *arg)
{
    if (!s_initialized || !arg) return;
    const char *raw  = (const char *)arg;
    char       *text = strip_unsupported(raw);
    free(arg);
    if (!text) return;

    if (text[0] == '\0') {
        /* Start-of-turn clear signal (chat_app calls this before every new
         * reply). Do not touch history — just mark that the next non-empty
         * call should open a NEW bubble instead of editing the previous
         * (now historical) reply. */
        s_assistant_turn_open = true;
        free(text);
        return;
    }

    if (s_assistant_turn_open || !s_last_assistant_lbl) {
        lv_obj_t *lbl = NULL;
        s_last_assistant_bubble = create_bubble(false, text, &lbl);
        s_last_assistant_lbl    = lbl;
        s_assistant_turn_open   = false;
    } else {
        /* Streaming update: same turn, same bubble. */
        lv_label_set_text(s_last_assistant_lbl, text);
    }
    if (s_msg_list) lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
    free(text);
}

typedef struct { char *text; bool is_user; } text_arg_t;

static void apply_user_text_lvgl(void *arg)
{
    if (!s_initialized || !arg) return;
    char *raw  = (char *)arg;
    char *text = strip_unsupported(raw);
    free(raw);
    if (!text) return;
    if (text[0] != '\0') {
        create_bubble(true, text, NULL);
        s_assistant_turn_open = true;  /* next assistant text opens a new bubble */
        if (s_msg_list) lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
    }
    free(text);
}

/* ── Scrollable message history ───────────────────────────────────────────── *
 * Each bubble carries its creation timestamp (esp_timer_get_time, us) as
 * heap-allocated LVGL user data, freed by bubble_delete_cb.  A periodic LVGL
 * timer (runs on the LVGL task, like sleep_manager's) sweeps bubbles older
 * than MSG_EXPIRE_US.  All of this only ever touches LVGL objects from the
 * LVGL task: creation happens inside lv_async_call callbacks, and sweeping
 * happens inside an lv_timer callback — the same convention already used by
 * sleep_manager.c, so no extra display_driver_lock() is required.          */
typedef struct { int64_t ts_us; } bubble_meta_t;

static void bubble_delete_cb(lv_event_t *e)
{
    lv_obj_t *bubble = lv_event_get_target(e);
    bubble_meta_t *meta = (bubble_meta_t *)lv_obj_get_user_data(bubble);
    if (meta) free(meta);
    if (bubble == s_last_assistant_bubble) {
        /* The bubble a streaming reply was writing into just got evicted
         * (cap or expiry) — force the next assistant text to start fresh
         * instead of writing through a dangling label pointer. */
        s_last_assistant_bubble = NULL;
        s_last_assistant_lbl    = NULL;
        s_assistant_turn_open   = true;
    }
}

static lv_obj_t *create_bubble(bool is_user, const char *text, lv_obj_t **out_label)
{
    if (!s_msg_list) return NULL;
    const ui_theme_t *th = ui_theme_get();

    if ((int)lv_obj_get_child_cnt(s_msg_list) >= MAX_HISTORY_MESSAGES) {
        lv_obj_t *oldest = lv_obj_get_child(s_msg_list, 0);
        if (oldest) lv_obj_del(oldest);
    }

    /* Plain text row — matches the original UI exactly (no visible bubble):
     * the old s_user_card/s_bubble_card were always LV_OPA_TRANSP ("hidden
     * until text", but nothing ever made them opaque), so users only ever
     * saw two plain text lines. Keep that look, just make each turn its own
     * scrollable/expiring row instead of a single overwritten line. */
    lv_obj_t *bubble = lv_obj_create(s_msg_list);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bubble, BUBBLE_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_align(bubble, is_user ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0);

    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, BUBBLE_W - 20);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, is_user ? lv_color_hex(0xFFFFFF) : th->text, 0);
    lv_obj_set_style_text_font(lbl, g_font_cn_20, 0);  /* comprehensive CJK font */
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    if (is_user) lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);

    bubble_meta_t *meta = malloc(sizeof(*meta));
    if (meta) {
        meta->ts_us = esp_timer_get_time();
        lv_obj_set_user_data(bubble, meta);
    }
    lv_obj_add_event_cb(bubble, bubble_delete_cb, LV_EVENT_DELETE, NULL);

    if (out_label) *out_label = lbl;
    return bubble;
}

static void msg_expiry_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_msg_list) return;
    int64_t now = esp_timer_get_time();
    uint32_t i = 0;
    while (i < lv_obj_get_child_cnt(s_msg_list)) {
        lv_obj_t *child = lv_obj_get_child(s_msg_list, i);
        bubble_meta_t *meta = (bubble_meta_t *)lv_obj_get_user_data(child);
        if (meta && (now - meta->ts_us) >= MSG_EXPIRE_US) {
            lv_obj_del(child);   /* fires bubble_delete_cb; do not advance i */
        } else {
            i++;
        }
    }
}

/* ── Touch handler ────────────────────────────────────────────────────────── */
static void img_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_remote_visible) {
        /* First tap on a displayed remote image dismisses it and restores
         * the Nahida state image. The same tap also falls through to the
         * normal recording toggle below (semantics "a") so the user never
         * spends a tap on dismissal alone. */
        clear_remote_image_lvgl();
    }
    if (s_tap_cb) s_tap_cb();
}

static void settings_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings gear clicked");
    if (s_settings_cb) s_settings_cb();
}

/* ── Build ────────────────────────────────────────────────────────────────── */
esp_err_t avatar_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Avatar UI initialising...");

    if (!s_decoders_ready) {
        lv_split_jpeg_init();
        lv_png_init();
        s_decoders_ready = true;
    }

    const ui_theme_t *th = ui_theme_get();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, th->bg, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    s_status_bar = ui_status_bar_create(s_screen);
    ui_status_bar_set_wifi(s_status_bar, wifi_is_connected());
    ui_status_bar_set_ws(s_status_bar,   ws_client_is_connected());

    /* ClawChat is the only boot surface.  Keep settings reachable from the
     * same screen instead of exposing the old launcher/home grid. */
    lv_obj_t *settings_btn = lv_btn_create(s_screen);
    lv_obj_set_size(settings_btn, 36, 36);
    lv_obj_align(settings_btn, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_set_style_bg_color(settings_btn, th->card, 0);
    lv_obj_set_style_bg_opa(settings_btn, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(settings_btn, th->card_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(settings_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(settings_btn, 0, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_set_style_pad_all(settings_btn, 0, 0);
    lv_obj_add_flag(settings_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(settings_btn, 12);
    lv_obj_add_event_cb(settings_btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    s_settings_btn = settings_btn;

    lv_obj_t *settings_lbl = lv_label_create(settings_btn);
    lv_label_set_text(settings_lbl, FA_GEAR);
    lv_obj_set_style_text_color(settings_lbl, th->accent, 0);
    lv_obj_set_style_text_font(settings_lbl, UI_FONT_ICON, 0);
    lv_obj_set_style_bg_opa(settings_lbl, LV_OPA_TRANSP, 0);
    lv_obj_center(settings_lbl);

    /* State image — 200×200, centred below status bar, tappable */
    s_state_img = lv_img_create(s_screen);
    lv_img_set_src(s_state_img, &img_state_booting);
    lv_obj_set_size(s_state_img, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_state_img, LV_ALIGN_TOP_MID, 0, AVATAR_TOP);
    lv_obj_add_flag(s_state_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_state_img, img_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Hidden by default; remote_image reveals it temporarily and releases
     * the compressed buffer when the display interval expires. */
    s_remote_img = lv_img_create(s_screen);
    lv_obj_set_size(s_remote_img, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_remote_img, LV_ALIGN_TOP_MID, 0, AVATAR_TOP);
    lv_obj_add_flag(s_remote_img, LV_OBJ_FLAG_HIDDEN);
    /* lv_img is not clickable by default — without this flag, taps over the
     * remote image (which sits in front of s_state_img while visible) would
     * never reach img_tap_cb, so the image could not be dismissed. */
    lv_obj_add_flag(s_remote_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_remote_img, img_tap_cb, LV_EVENT_CLICKED, NULL);

    /* ── Scrollable chat history (touch swipe up/down, WeChat-style) ─────── *
     * Replaces the old single fixed user/assistant bubble pair: every user
     * utterance and assistant reply becomes its own bubble appended to this
     * flex-column list. LVGL objects are scrollable by default, so swipe is
     * free; new messages jump the view to the bottom (see create_bubble's
     * callers), and a periodic timer expires bubbles after MSG_EXPIRE_US. */
    s_msg_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_msg_list, DISPLAY_H_RES, MSG_LIST_H);
    lv_obj_set_pos(s_msg_list, 0, MSG_LIST_TOP);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msg_list, 0, 0);
    lv_obj_set_style_shadow_width(s_msg_list, 0, 0);
    lv_obj_set_style_pad_all(s_msg_list, BUBBLE_MARGIN, 0);
    lv_obj_set_style_pad_row(s_msg_list, 8, 0);
    lv_obj_set_style_radius(s_msg_list, 0, 0);
    lv_obj_set_layout(s_msg_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);
    lv_obj_add_flag(s_msg_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_AUTO);

    s_last_assistant_bubble = NULL;
    s_last_assistant_lbl    = NULL;
    s_assistant_turn_open   = true;

    if (!s_msg_expiry_timer) {
        s_msg_expiry_timer = lv_timer_create(msg_expiry_timer_cb,
                                             MSG_EXPIRY_PERIOD_MS, NULL);
    }

    /* State label (请说话... / 思考中...) — just above bottom edge */
    s_state_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_state_lbl, "");
    lv_obj_align(s_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_state_lbl, th->text_dim, 0);
    lv_obj_set_style_text_font(s_state_lbl, UI_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(s_state_lbl, LV_OPA_TRANSP, 0);

    /* Bubbles and labels are created after the gear. Keep the control in the
     * foreground and give it a small touch margin for the FT5x06 panel. */
    lv_obj_move_foreground(settings_btn);

    s_initialized = true;
    ESP_LOGI(TAG, "Avatar UI ready");
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

lv_obj_t *avatar_get_screen(void) { return s_screen; }

esp_err_t avatar_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_async_call(apply_state_lvgl, (void *)(uintptr_t)state);
    return ESP_OK;
}

void avatar_show_assistant_text(const char *text)
{
    if (!s_initialized || !text) return;
    char *copy = strdup(text);
    if (copy) lv_async_call(apply_text_lvgl, copy);
}

void avatar_show_user_text(const char *text)
{
    if (!s_initialized || !text) return;
    char *copy = strdup(text);
    if (copy) lv_async_call(apply_user_text_lvgl, copy);
}

void avatar_clear_assistant_text(void)
{
    avatar_show_assistant_text("");
}

/* Status bar helpers — safe to call from any task */
esp_err_t avatar_set_connection_status(bool connected)
{
    (void)connected;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    lv_async_call(apply_state_lvgl, (void *)(uintptr_t)APP_STATE_IDLE);
    return ESP_OK;
}

esp_err_t avatar_set_emotion_with_return(avatar_emotion_t emotion,
                                         int duration_ms,
                                         avatar_emotion_t return_emotion)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    emotion_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) return ESP_ERR_NO_MEM;
    arg->emotion = emotion;
    arg->duration_ms = duration_ms;
    arg->return_emotion = return_emotion;
    lv_async_call(apply_emotion_lvgl, arg);
    return ESP_OK;
}

esp_err_t avatar_set_emotion(avatar_emotion_t emotion)
{
    return avatar_set_emotion_with_return(emotion, 0, AVATAR_IDLE);
}

esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t emotion, int duration_ms)
{
    return avatar_set_emotion_with_return(emotion, duration_ms, AVATAR_IDLE);
}

void avatar_show_image_owned(uint8_t *data, size_t len, const char *caption)
{
    if (!data || len == 0) {
        free(data);
        return;
    }
    size_t cap_len = strlen(caption ? caption : "");
    image_arg_t *arg = malloc(sizeof(*arg) + cap_len + 1);
    if (!arg) {
        free(data);
        return;
    }
    arg->data = data;
    arg->len = len;
    memcpy(arg->caption, caption ? caption : "", cap_len + 1);
    if (!s_initialized) {
        free(arg->data);
        free(arg);
        return;
    }
    lv_async_call(apply_remote_image_lvgl, arg);
}

/* Legacy message names kept for compatibility. */
esp_err_t avatar_show_message(const char *msg)                            { avatar_show_assistant_text(msg); return ESP_OK; }
esp_err_t avatar_clear_message(void)                                      { avatar_clear_assistant_text(); return ESP_OK; }
avatar_emotion_t avatar_get_current_emotion(void)                         { return s_current_emotion; }

void avatar_reset_screen(void)
{
    if (s_emotion_timer) {
        lv_timer_del(s_emotion_timer);
        s_emotion_timer = NULL;
    }
    if (s_remote_timer) {
        lv_timer_del(s_remote_timer);
        s_remote_timer = NULL;
    }
    if (s_msg_expiry_timer) {
        lv_timer_del(s_msg_expiry_timer);
        s_msg_expiry_timer = NULL;
    }
    if (s_screen) {
        /* lv_obj_del(s_screen) cascades to s_msg_list's children, firing
         * bubble_delete_cb for each — no manual cleanup needed here. */
        if (s_remote_data) lv_img_cache_invalidate_src(&s_remote_dsc);
        lv_obj_del(s_screen);
        s_screen       = NULL;
        s_status_bar   = NULL;
        s_settings_btn = NULL;
        s_state_img    = NULL;
        s_msg_list     = NULL;
        s_last_assistant_bubble = NULL;
        s_last_assistant_lbl    = NULL;
        s_assistant_turn_open   = true;
        s_state_lbl    = NULL;
        s_remote_img   = NULL;
        s_initialized  = false;
    }
    free(s_remote_data);
    s_remote_data = NULL;
    memset(&s_remote_dsc, 0, sizeof(s_remote_dsc));
    s_remote_visible = false;
    s_current_emotion = AVATAR_IDLE;
}
