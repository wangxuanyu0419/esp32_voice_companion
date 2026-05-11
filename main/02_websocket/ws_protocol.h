#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#include "emotion_map.h"

/**
 * WebSocket 协议解析
 * 
 * 处理 ClawChat 服务器消息：
 * - tts_text: TTS 文本，需要播放
 * - live2d: 虚拟形象指令（含 emotion 事件）
 * - status: 状态更新
 * - assistant_text: 文字回复
 * - ready: 连接就绪
 * - error: 错误
 * 
 * emotion 事件格式：
 * {"type": "live2d", "action": "emotion", "name": "happy_l2", "duration_ms": 2500}
 */

// 回调类型定义
typedef void (*ws_on_tts_cb_t)(const char* text, void* arg);
typedef void (*ws_on_live2d_cb_t)(const ws_live2d_event_t* event, void* arg);
typedef void (*ws_on_status_cb_t)(const ws_status_event_t* status, void* arg);
typedef void (*ws_on_text_cb_t)(const char* text, void* arg);

// Live2D 事件结构（与 ClawChat 服务器协议一致）
// 来自: {"type": "live2d", "action": "emotion", "name": "happy_l2", "duration_ms": 2500}
typedef struct {
    const char* action;      // "emotion" / "expression" / "motion"
    const char* name;       // emotion ID (e.g. "happy_l2")
    int duration_ms;         // 持续时间
    const char* return_to;  // 返回目标（如 "idle"）
} ws_live2d_event_t;

// Status 事件
typedef struct {
    bool listening;
    bool thinking;
    bool speaking;
} ws_status_event_t;

/**
 * 解析 WebSocket JSON 消息
 */
void ws_protocol_parse(const char* json_str);

/**
 * 注册消息回调
 */
void ws_protocol_register_callbacks(ws_on_tts_cb_t tts_cb,
                                    ws_on_live2d_cb_t live2d_cb,
                                    ws_on_status_cb_t status_cb,
                                    ws_on_text_cb_t text_cb,
                                    void* arg);

// ============================================================
// Phase-1 STT audio: stt_audio message builder
// ============================================================

/**
 * 构建 stt_audio JSON 消息
 * @param pcm_data     PCM16 mono 16kHz audio samples
 * @param sample_count number of int16_t samples
 * @param is_last      true for final chunk (button release)
 * @param out_buf      output buffer (must hold ~2100 bytes for 1280 samples)
 * @param buf_size     buffer size
 * @return length of JSON string written, or -1 on error
 */
int ws_protocol_build_stt_audio(const int16_t* pcm_data, size_t sample_count,
                                bool is_last, char* out_buf, size_t buf_size);

#endif // WS_PROTOCOL_H