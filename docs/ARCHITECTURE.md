# 当前架构与协议

更新：2026-10-16。旧 ARCHITECTURE_PLAN.md、ARCH_REVIEW.md、UI_VISUAL_UPDATE_PLAN.md 为历史设计参考；构建以 main/CMakeLists.txt 为准。

```text
main/main.c
  ├─ Board       按键、显示／触摸、LED、存储
  ├─ System      NVS 配置、Wi-Fi、事件队列
  ├─ Framework   状态、协议、音频、显示、休眠
  └─ Apps        chat / settings（当前注册）

ClawChat 后端（不在本仓库）
  ├─ WSS /ws                         控制、文字、PCM
  ├─ HTTPS /stream?text=...           文本转语音
  ├─ HTTPS /audio-stream/<stream_id>  广播音频
  └─ 同服务器 HTTPS 图片 URL
```

LVGL 主循环在 Core 0；网络初始化和事件处理在 Core 1。屏幕启动不等待 Wi-Fi。音频与图片通过任务／队列处理。聊天层在播放期间关闭采音，防止回录触发 VAD；ES8311/I2S 电源使用引用计数。PCM 队列及 HTTP 广播预取均有上限，首块被 I2S 接受后才回报播放开始。

## 音频硬件（来自 audio_pipeline.c）

| 接口 | GPIO |
| --- | --- |
| I2C SDA / SCL | 15 / 14，ES8311 地址 0x18 |
| I2S MCLK / BCLK / WS | 16 / 9 / 45 |
| I2S ESP→DAC / ADC→ESP | 8 / 10 |
| 扬声器 PA / BOOT | 46 / 0 |

采音 16 kHz、单声道 PCM16，常规块 40 ms。I2S 格式和重采样由音频层处理；换板需核对原理图。

## 消息

以 main/framework/protocol/ws_protocol.c 和 main/apps/chat/chat_app.c 为协议实现依据。此表描述客户端源码，不证明线上已验收。

| 入站消息 | 作用／字段 |
| --- | --- |
| ready / voice_session_ready | 握手、语音会话就绪 |
| assistant_text / stt_final / status / voice_turn_state | 文字、状态和回合推进 |
| tts_text / tts_sentence | 文本语音队列 |
| tts_audio_start | streamId、sampleRate、channels、encoding |
| WS 二进制帧 | 当前下行 PCM 流数据 |
| tts_audio_end | 按 streamId 结束，等待队列播放完毕 |
| audio_broadcast | stream_id、message；构造同服务器音频拉取 URL |
| image / image_message | url 或 image_url；caption 或 text |
| live2d | 表情动作映射为本地状态；不是板上完整 Live2D 引擎 |
| broadcast / agent_ack / ping / pong / error | 广播、代理确认、心跳、错误 |

图片只接受与当前 WSS 服务器同 authority 的 HTTPS URL、JPEG/PNG，最大 512 KiB。HTTP 广播接受服务端 stream ID，不接受任意播放 URL。

## 上行录音包

每包 **32 字节头 + PCM16-LE**。常规块 640 采样／1280 字节 PCM：

| 偏移 | 字节 | 内容 |
| --- | --- | --- |
| 0 | 4 | magic CC 56 43 01 |
| 4 | 4 | 小端 chunk index |
| 8 | 4 | 小端 sample rate，16000 |
| 12 | 1 | flags，bit0 为 isLast |
| 13 | 1 | channels，1 |
| 14 | 1 | bits per sample，16 |
| 15 | 1 | reserved，0 |
| 16 | 16 | opaque turn ID |

该头仅用于上行录音，不要将它附加到下行 PCM。完整会话控制需同时对照 chat_app.c 的发送流程。
