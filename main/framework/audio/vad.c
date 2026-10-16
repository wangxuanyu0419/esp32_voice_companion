#include "vad.h"
#include "esp_log.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "VAD";

/* Android AdaptiveEnergyVad observes 100 ms frames. ESP32 capture chunks
 * are 40 ms, so thirteen chunks provide the same approximately 0.5 s quiet
 * calibration window without adding a second capture buffer. */
#define CALIBRATION_CHUNKS       13
#define SPEECH_START_CHUNKS      5  /* 5 x 40 ms matches Android's 2 x 100 ms */
#define NOISE_MULTIPLIER_NUM     5
#define NOISE_MULTIPLIER_DEN     2
#define NOISE_MARGIN              40
#define MAX_AUTOMATIC_THRESHOLD   4000
#define RELEASE_RATIO_NUM         72
#define RELEASE_RATIO_DEN         100
#define RELEASE_MARGIN            20
/* Bootstrap (pre-calibration) onset bar.  This MUST NOT exceed the
 * confirmed-speech bar used once calibrated (manual_threshold, or the
 * noise-derived effective_threshold, whichever is higher) — otherwise
 * normal-volume speech spoken right after tapping to talk (before the
 * ~520 ms calibration window closes) never crosses the bootstrap bar,
 * gets folded into the "quiet" calibration samples instead, and the
 * session times out via VAD_NO_SPEECH_CHUNKS having never seen speech.
 * 1.0x keeps onset detection consistent from the very first frame. */
#define BOOTSTRAP_NUM             10
#define BOOTSTRAP_DEN             10

static vad_state_t   s_state;
static int           s_speech_frames;
static int           s_silence_frames;
static int32_t       s_energy;
static int           s_manual_threshold = 200;
static bool          s_automatic = true;
static int32_t       s_noise_floor;
static int32_t       s_effective_threshold = 200;
static int32_t       s_release_threshold = 144;
static int           s_calibration_count;
static int32_t       s_calibration[CALIBRATION_CHUNKS];
static bool          s_calibrated;
/* Android keeps a bootstrap onset candidate separate from the confirmed
 * speech state.  Without this bit, one noisy 40 ms frame could leave the C
 * state in SPEECH while calibration was still in progress. */
static bool          s_bootstrap_speech_active;
static vad_callback_t s_cb;
static void         *s_cb_arg;

static int32_t calc_mean_abs(const int16_t *frame, size_t len)
{
    if (!frame || len == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        int32_t sample = frame[i];
        sum += sample < 0 ? -(int64_t)sample : sample;
    }
    return (int32_t)(sum / (int64_t)len);
}

static int32_t clamp_threshold(int32_t value)
{
    if (value < 80) value = 80;
    if (value > 2000) value = 2000;
    return value;
}

static void recompute_thresholds(void)
{
    int32_t noise_term = (s_noise_floor * NOISE_MULTIPLIER_NUM) /
                         NOISE_MULTIPLIER_DEN + NOISE_MARGIN;
    if (!s_automatic) {
        s_effective_threshold = s_manual_threshold;
    } else {
        s_effective_threshold = s_manual_threshold > noise_term
                              ? s_manual_threshold : noise_term;
        if (s_effective_threshold > MAX_AUTOMATIC_THRESHOLD)
            s_effective_threshold = MAX_AUTOMATIC_THRESHOLD;
    }
    s_release_threshold = (s_effective_threshold * RELEASE_RATIO_NUM) /
                          RELEASE_RATIO_DEN - RELEASE_MARGIN;
    if (s_release_threshold < s_manual_threshold)
        s_release_threshold = s_manual_threshold;
}

static void calibrate_noise_floor(void)
{
    if (s_calibration_count <= 0) return;
    int32_t sorted[CALIBRATION_CHUNKS];
    memcpy(sorted, s_calibration, (size_t)s_calibration_count * sizeof(sorted[0]));
    for (int i = 1; i < s_calibration_count; i++) {
        int32_t value = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > value) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = value;
    }
    /* Low percentile, matching the Android implementation's quiet bias. */
    s_noise_floor = sorted[(s_calibration_count - 1) / 4];
    s_calibrated = true;
    recompute_thresholds();
    ESP_LOGI(TAG, "Noise floor calibrated=%" PRId32 " threshold=%" PRId32
             " release=%" PRId32 " auto=%d",
             s_noise_floor, s_effective_threshold, s_release_threshold,
             s_automatic ? 1 : 0);
}

esp_err_t vad_init(void)
{
    s_manual_threshold = 200;
    s_automatic = true;
    vad_reset();
    ESP_LOGI(TAG, "VAD ready threshold=%" PRId32 " auto=%d",
             s_effective_threshold, s_automatic ? 1 : 0);
    return ESP_OK;
}

void vad_configure(int manual_threshold, bool automatic)
{
    s_manual_threshold = (int)clamp_threshold(manual_threshold);
    s_automatic = automatic;
    vad_reset();
    ESP_LOGI(TAG, "VAD configured minimum=%d auto=%d",
             s_manual_threshold, s_automatic ? 1 : 0);
}

void vad_reset(void)
{
    s_state = VAD_STATE_SILENCE;
    s_speech_frames = 0;
    s_silence_frames = 0;
    s_energy = 0;
    s_calibration_count = 0;
    s_calibrated = !s_automatic;
    s_bootstrap_speech_active = false;
    s_noise_floor = (s_manual_threshold > NOISE_MARGIN)
                  ? ((s_manual_threshold - NOISE_MARGIN) * 2 / 5) : 0;
    recompute_thresholds();
}

void vad_set_sensitivity(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    /* Preserve the legacy API while giving each level the Android scale. */
    static const int thresholds[] = { 2000, 1000, 400, 200 };
    vad_configure(thresholds[level], s_automatic);
}

vad_state_t vad_process_frame(const int16_t *pcm_frame, size_t frame_size)
{
    s_energy = calc_mean_abs(pcm_frame, frame_size);

    if (s_automatic && !s_calibrated) {
        /* Match Android AdaptiveEnergyVad + its two-frame onset guard:
         * a clear first frame is only a candidate; a second compatible frame
         * confirms speech.  A one-frame click/noise spike is returned to the
         * quiet calibration path instead of opening an STT turn. */
        bool bootstrap_speech = s_bootstrap_speech_active
                              ? s_energy >= s_release_threshold
                              : s_energy >= (s_manual_threshold * BOOTSTRAP_NUM) /
                                            BOOTSTRAP_DEN;
        if (bootstrap_speech) {
            s_bootstrap_speech_active = true;
            if (++s_speech_frames >= SPEECH_START_CHUNKS) {
                s_state = VAD_STATE_SPEECH;
                s_silence_frames = 0;
                if (s_cb) s_cb(VAD_STATE_SPEECH, s_cb_arg);
            }
            return s_state;
        }
        s_bootstrap_speech_active = false;
        s_speech_frames = 0;
        s_state = VAD_STATE_SILENCE;
        if (s_calibration_count < CALIBRATION_CHUNKS)
            s_calibration[s_calibration_count++] = s_energy;
        if (s_calibration_count >= CALIBRATION_CHUNKS) calibrate_noise_floor();
        return s_state;
    }

    recompute_thresholds();
    int32_t threshold = s_state == VAD_STATE_SPEECH
                      ? s_release_threshold : s_effective_threshold;
    bool speech = s_energy >= threshold;
    if (s_state == VAD_STATE_SILENCE) {
        if (speech) {
            if (++s_speech_frames >= SPEECH_START_CHUNKS) {
                s_state = VAD_STATE_SPEECH;
                s_silence_frames = 0;
                if (s_cb) s_cb(VAD_STATE_SPEECH, s_cb_arg);
            }
        } else {
            s_speech_frames = 0;
            if (s_automatic) {
                int alpha_num = s_energy > s_noise_floor ? 6 : 25;
                s_noise_floor += ((s_energy - s_noise_floor) * alpha_num) / 100;
                recompute_thresholds();
            }
        }
    } else if (speech) {
        s_silence_frames = 0;
    } else if (++s_silence_frames >= 7) {
        s_state = VAD_STATE_SILENCE;
        s_speech_frames = 0;
        if (s_cb) s_cb(VAD_STATE_SILENCE, s_cb_arg);
    }
    return s_state;
}

int32_t vad_get_energy(void) { return s_energy; }
int32_t vad_get_effective_threshold(void) { return s_effective_threshold; }
vad_state_t vad_get_state(void) { return s_state; }
const char *vad_get_text(void) { return ""; }

void vad_register_callback(vad_callback_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
}
