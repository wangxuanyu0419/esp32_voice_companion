/**
 * 语音活动检测 (VAD) - 实现
 * 
 * 简单的能量检测 VAD
 */

#include "vad_detector.h"
#include "esp_log.h"
#include "esp_dsp.h"

static const char* TAG = "VAD";

// VAD 参数
#define VAD_SILENCE_THRESHOLD    500
#define VAD_SPEECH_THRESHOLD     1500
#define VAD_SPEECH_FRAMES        3       // 连续 3 帧语音判定
#define VAD_SILENCE_FRAMES       10      // 连续 10 帧静音判定语音结束
#define FRAME_SIZE               512      // 每帧样本数

// VAD 状态
static vad_state_t s_state = VAD_STATE_SILENCE;
static int s_speech_frames = 0;
static int s_silence_frames = 0;
static int32_t s_energy = 0;
static int s_sensitivity = 2;

// 回调
static vad_callback_t s_callback = NULL;
static void* s_callback_arg = NULL;

// 计算帧能量
static int32_t calc_frame_energy(const int16_t* frame, size_t len)
{
    int64_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += (int64_t)frame[i] * frame[i];
    }
    return (int32_t)(sum / len);
}

esp_err_t vad_init(void)
{
    ESP_LOGI(TAG, "VAD initializing...");
    vad_reset();
    return ESP_OK;
}

void vad_reset(void)
{
    s_state = VAD_STATE_SILENCE;
    s_speech_frames = 0;
    s_silence_frames = 0;
    s_energy = 0;
}

void vad_set_sensitivity(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    s_sensitivity = level;
    
    // 根据灵敏度调整阈值
    // level 0: 高灵敏度（更易检测到语音）
    // level 3: 低灵敏度（需要更大声音）
    switch (level) {
        case 0:
            // 最高灵敏度
            break;
        case 1:
            // 中高灵敏度
            break;
        case 2:
            // 中灵敏度（默认）
            break;
        case 3:
            // 低灵敏度
            break;
    }
}

vad_state_t vad_process_frame(const int16_t* pcm_frame, size_t frame_size)
{
    // 计算能量
    s_energy = calc_frame_energy(pcm_frame, frame_size);
    
    vad_state_t prev_state = s_state;
    
    switch (s_state) {
        case VAD_STATE_SILENCE:
            if (s_energy > VAD_SPEECH_THRESHOLD) {
                s_speech_frames++;
                if (s_speech_frames >= VAD_SPEECH_FRAMES) {
                    // 检测到语音开始
                    s_state = VAD_STATE_SPEECH;
                    s_silence_frames = 0;
                    ESP_LOGI(TAG, "VAD: Speech start detected (energy=%ld)", s_energy);
                    
                    if (s_callback) {
                        s_callback(VAD_STATE_SPEECH, s_callback_arg);
                    }
                }
            } else {
                s_speech_frames = 0;
            }
            break;
            
        case VAD_STATE_SPEECH:
            if (s_energy < VAD_SILENCE_THRESHOLD) {
                s_silence_frames++;
                if (s_silence_frames >= VAD_SILENCE_FRAMES) {
                    // 检测到语音结束
                    s_state = VAD_STATE_SILENCE;
                    s_speech_frames = 0;
                    ESP_LOGI(TAG, "VAD: Speech end detected");
                    
                    if (s_callback) {
                        s_callback(VAD_STATE_SILENCE, s_callback_arg);
                    }
                }
            } else {
                s_silence_frames = 0;
            }
            break;
    }
    
    return s_state;
}

int32_t vad_get_energy(void)
{
    return s_energy;
}

vad_state_t vad_get_state(void)
{
    return s_state;
}

const char* vad_get_text(void)
{
    // TODO: 与 STT 模块集成，返回识别的文本
    return "";
}

void vad_register_callback(vad_callback_t cb, void* arg)
{
    s_callback = cb;
    s_callback_arg = arg;
}