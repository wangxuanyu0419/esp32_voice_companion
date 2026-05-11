#include "vad.h"
#include "esp_log.h"

static const char *TAG = "VAD";

#define VAD_SILENCE_THRESHOLD  500
#define VAD_SPEECH_THRESHOLD   1500
#define VAD_SPEECH_FRAMES      3
#define VAD_SILENCE_FRAMES     10

static vad_state_t   s_state          = VAD_STATE_SILENCE;
static int           s_speech_frames  = 0;
static int           s_silence_frames = 0;
static int32_t       s_energy         = 0;
static vad_callback_t s_cb            = NULL;
static void         *s_cb_arg         = NULL;

static int32_t calc_energy(const int16_t *frame, size_t len)
{
    int64_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += (int64_t)frame[i] * frame[i];
    return (int32_t)(sum / (int64_t)len);
}

esp_err_t vad_init(void)
{
    vad_reset();
    ESP_LOGI(TAG, "VAD ready");
    return ESP_OK;
}

void vad_reset(void)
{
    s_state          = VAD_STATE_SILENCE;
    s_speech_frames  = 0;
    s_silence_frames = 0;
    s_energy         = 0;
}

void vad_set_sensitivity(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
}

vad_state_t vad_process_frame(const int16_t *pcm_frame, size_t frame_size)
{
    s_energy = calc_energy(pcm_frame, frame_size);

    if (s_state == VAD_STATE_SILENCE) {
        if (s_energy > VAD_SPEECH_THRESHOLD) {
            if (++s_speech_frames >= VAD_SPEECH_FRAMES) {
                s_state          = VAD_STATE_SPEECH;
                s_silence_frames = 0;
                ESP_LOGI(TAG, "Speech start (energy=%" PRId32 ")", s_energy);
                if (s_cb) s_cb(VAD_STATE_SPEECH, s_cb_arg);
            }
        } else {
            s_speech_frames = 0;
        }
    } else {
        if (s_energy < VAD_SILENCE_THRESHOLD) {
            if (++s_silence_frames >= VAD_SILENCE_FRAMES) {
                s_state         = VAD_STATE_SILENCE;
                s_speech_frames = 0;
                ESP_LOGI(TAG, "Speech end");
                if (s_cb) s_cb(VAD_STATE_SILENCE, s_cb_arg);
            }
        } else {
            s_silence_frames = 0;
        }
    }
    return s_state;
}

int32_t     vad_get_energy(void)   { return s_energy; }
vad_state_t vad_get_state(void)    { return s_state;  }
const char *vad_get_text(void)     { return "";       }

void vad_register_callback(vad_callback_t cb, void *arg)
{
    s_cb     = cb;
    s_cb_arg = arg;
}
