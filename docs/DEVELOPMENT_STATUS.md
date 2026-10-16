# 当前开发状态

更新：**2026-10-16**。“已实现”表示源码已有逻辑；本次验证范围见 [验证记录](VALIDATION.md)。

## 同步基线

- 原远端 `main`：`a1caad0`（2026-06-07），扬声器测试音、PCM 路径和上行缓冲优化。
- 本地提交：`a15bbd2`（2026-07-05），移除默认启动器／色彩测试流程。
- 本次整理：迁移副本中的未提交固件、音频库、字体、头像，以及 9 月保留的音频兼容和串口工具。
- 原 OneDrive 目录仅有残留目录和少量脚本；完整开发副本在 `E:/AI/ESP32/esp32_voice_companion`。本次在独立副本整理，没有重置原开发副本的索引或改动。

## 功能状态

| 模块 | 已实现内容 | 源码 |
| --- | --- | --- |
| 架构 | Board / System / Framework / Apps 四层、C 入口 | `main/main.c`、`main/CMakeLists.txt` |
| 显示 | AMOLED、触摸、角色表情、聊天、主题和屏幕节能 | `main/board/`、`main/framework/display/` |
| 网络／配置 | Wi-Fi 列表轮换、手动切换、NVS、服务器选择 | `main/system/`、`main/apps/settings/` |
| 协议 | WSS 认证头、握手、心跳、JSON 分片重组、扩展消息 | `main/framework/protocol/ws_protocol.c` |
| 语音输入 | ES8311/I2S、16 kHz PCM16、40 ms 分块、自适应 VAD、前滚和超时 | `audio_pipeline.c`、`vad.c`、`chat_app.c` |
| 会话 | 半双工采音门控、流播放回调、中断及失败处理 | `main/apps/chat/chat_app.c` |
| PCM 播放 | 有界队列、序号／校验统计、采样率转换和 I2S | `main/framework/audio/pcm_stream_player.c` |
| HTTP 音频 | MP3/AAC/WAV/普通 M4A 解码、广播预取、停止 | `main/framework/audio/tts_player.c` |
| 音量／电源 | 实际 DAC 音量、低音量增益、旧音量迁移、电源引用计数 | `audio_pipeline.c`、`config_store.c` |
| 图片 | 同服务器 HTTPS 获取、JPEG/PNG 和 512 KiB 限制 | `remote_image.c`、`scene_avatar.c` |
| 字体／头像 | SPIFFS 中文字库、自定义二进制字体、表情资源 | `spiffs_data/`、`cbin_font.c` |
| TF 媒体 | JPG/WAV/MJPEG 代码和 MP4 转换工具，未在当前主入口注册 | `main/apps/media_player/`、`tools/mp4_to_mjpeg.py` |
| 发送端兼容 | fMP4 标记检测、FFmpeg copy-remux、spool 保留 | `tools/n100/audio_broadcast_compat.py` |

## 行为和限制

- 开机直接进入 chat，注册 settings；保留的 launcher／media player 文件不代表已启用。
- VAD 是能量／噪声底检测。参数含约 1 秒前滚、1 秒静音结束、5 秒无语音超时。
- 默认音量 12/100；一次性迁移识别历史默认值 80、24，并尽量保留显式自定义值。
- `audio_play_beep()` 和旧 `audio_play_tts()` 为兼容桩；真实播放走 tts_player／pcm_stream_player。
- fragmented MP4 需发送端转封装。copy-remux 不改变音频编码，不能让任意编码自动兼容。
- `tts_player.c` 遗留 `wav_handle_data()` 未使用，存在编译警告。
- 旧 `main/01_*` 至 `main/08_*`、根目录旧入口的删除随本地清理同步；历史设计计划仍保留。
- 本次没有烧录、生产服务连接或端到端语音／音频／图片验收。

## 下一步

1. 上板验证录音 → STT → 回复 → 播放 → 待机，记录后端版本与串口日志。
2. 实测普通 M4A 和 fMP4 转封装广播，检查丢包、I2S 错误、首包延迟和持续播放。
3. 回归图片、字体、服务器切换、重连、音量迁移及屏幕节能。
4. 完成新 clone、默认配置的完整构建；本次原开发环境检查为增量构建。
5. 决定是否恢复 TF 入口、实现提示音并清理未使用的旧组件／函数。
