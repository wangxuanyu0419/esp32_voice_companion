#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * 音频管理器 - ES8311 + I2S
 * 
 * 功能：
 * - I2S 音频采集（MIC）
 * - I2S 音频播放（Speaker）
 * - TTS 文本转语音播放
 */

// 音频参数
#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS        1

// 音频缓冲大小
#define AUDIO_BUFFER_SIZE     (16 * 1024)  // 16KB 环形缓冲

// 初始化音频子系统
esp_err_t audio_init(void);

// 开始录音（边录边发送）
esp_err_t audio_start_capture(void);

// 停止录音
esp_err_t audio_stop_capture(void);

// 检查是否正在录音
bool audio_is_capturing(void);

// 播放 TTS 文本（通过服务器 TTS API）
esp_err_t audio_play_tts(const char* text);

// 播放提示音
typedef enum {
    BEEP_BOOT_PRESS,       // 按键音
    BEEP_LISTENING_START,  // 开始录音提示
    BEEP_CONNECTION_OK,    // 连接成功
    BEEP_ERROR,            // 错误提示
} audio_beep_type_t;

esp_err_t audio_play_beep(audio_beep_type_t type);

// 停止当前播放
esp_err_t audio_stop_playback(void);

// 获取当前播放状态
bool audio_is_playing(void);

// 设置音量 (0-100)
esp_err_t audio_set_volume(int volume);

// 获取当前音量
int audio_get_volume(void);

// 音频数据回调（用于录音时实时发送）
typedef void (*audio_capture_cb_t)(const int16_t* pcm_data, size_t len, void* arg);
void audio_register_capture_callback(audio_capture_cb_t cb, void* arg);

// ============================================================
// Phase-1 STT audio: chunked capture read
// ============================================================

// Chunk parameters: 40ms @ 16kHz mono PCM16 = 640 samples = 1280 bytes
#define AUDIO_CHUNK_SAMPLES     640
#define AUDIO_CHUNK_SIZE_BYTES  (AUDIO_CHUNK_SAMPLES * sizeof(int16_t))

/**
 * Read one audio chunk (blocking with timeout if not yet available).
 * Call this repeatedly after audio_start_capture() to get 40ms chunks.
 * @param buf      output buffer (must hold at least AUDIO_CHUNK_SAMPLES)
 * @param count    output: actual number of samples read
 * @param timeout_ms  how long to wait for chunk
 * @return true if chunk was read, false on timeout or error
 */
bool audio_manager_read_chunk(int16_t* buf, size_t* count, int timeout_ms);

/**
 * Send interrupt to server before starting a new capture session.
 * Call this on button press before audio_start_capture().
 */
esp_err_t audio_manager_send_interrupt_before_capture(void);

#endif // AUDIO_MANAGER_H