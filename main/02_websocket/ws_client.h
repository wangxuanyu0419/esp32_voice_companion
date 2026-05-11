#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

/**
 * WebSocket 客户端 - ClawChat 协议
 * 
 * 消息格式（JSON）：
 * - ESP32 → Server: hello, user_text, stt_partial, stt_final, interrupt, ping
 * - Server → ESP32: ready, status, assistant_text, tts_text, live2d, audio_url, error, pong
 */

// 初始化 WebSocket 客户端
esp_err_t ws_client_init(void);

// 连接到服务器
esp_err_t ws_client_connect(const char* url);

// 断开连接
void ws_client_disconnect(void);

// 检查是否已连接
bool ws_client_is_connected(void);

// 发送握手消息
esp_err_t ws_client_send_hello(void);

// 发送用户文本
esp_err_t ws_client_send_text(const char* text);

// 发送 STT 中间结果
esp_err_t ws_client_send_stt_partial(const char* text);

// 发送 STT 最终结果
esp_err_t ws_client_send_stt_final(const char* text);

// 发送 STT 音频 chunk（phase-1 新增）
// pcm_data: PCM16 mono 16kHz audio samples
// sample_count: number of int16_t samples
// is_last: true on final chunk (button release)
esp_err_t ws_client_send_stt_audio(const int16_t* pcm_data, size_t sample_count, bool is_last);

// 发送中断信号
esp_err_t ws_client_send_interrupt(void);

// 发送心跳
esp_err_t ws_client_send_ping(void);

// 注册消息回调
typedef void (*ws_message_cb_t)(const char* type, const char* data, void* arg);
void ws_register_message_callback(ws_message_cb_t cb, void* arg);

#endif // WS_CLIENT_H