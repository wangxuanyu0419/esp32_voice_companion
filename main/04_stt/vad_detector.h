#ifndef VAD_DETECTOR_H
#define VAD_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>

/**
 * 语音活动检测 (VAD)
 * 
 * 使用能量检测作为简单的 VAD
 * 阈值可配置
 */

// VAD 状态
typedef enum {
    VAD_STATE_SILENCE = 0,
    VAD_STATE_SPEECH,
} vad_state_t;

// 初始化 VAD
esp_err_t vad_init(void);

// 重置 VAD
void vad_reset(void);

// 处理一帧音频
vad_state_t vad_process_frame(const int16_t* pcm_frame, size_t frame_size);

// 获取当前能量
int32_t vad_get_energy(void);

// 设置灵敏度
void vad_set_sensitivity(int level);  // 0-3

// 获取当前 VAD 状态
vad_state_t vad_get_state(void);

// 获取已识别的文本（用于 stt_final）
const char* vad_get_text(void);

// 设置音频回调（识别到语音结束）
typedef void (*vad_callback_t)(vad_state_t state, void* arg);
void vad_register_callback(vad_callback_t cb, void* arg);

#endif // VAD_DETECTOR_H