# ESP32 Voice Companion - 项目开发进度

> 最后更新：2026-04-28

---

## 当前进度

### ✅ 已完成

| 模块 | 文件 | 说明 |
|------|------|------|
| **项目配置** | `CMakeLists.txt` | ESP-IDF 项目入口 |
| | `sdkconfig.defaults` | ESP-IDF 默认配置 |
| | `partitions.csv` | Flash 分区表 |
| **核心** | `main/app_main.c` | 主入口，初始化+状态机 |
| | `main/app_state.c/h` | 状态定义+转换 |
| **WiFi** | `main/01_wifi/wifi_manager.c/h` | WiFi 连接+配网 |
| **WebSocket** | `main/02_websocket/ws_client.c/h` | WS 客户端 |
| | `main/02_websocket/ws_protocol.c/h` | 协议解析 |
| **音频** | `main/03_audio/audio_manager.c/h` | ES8311+I2S |
| **STT** | `main/04_stt/vad_detector.c/h` | 语音活动检测 |
| **UI** | `main/05_ui_avatar/avatar_ui.c/h` | LVGL 头像 |
| **按键** | `main/06_button/button_handler.c/h` | BOOT 短按/长按 |
| **配置** | `main/07_config/config_store.c/h` | NVS 配置存储 |
| **LED** | `main/08_led/led_indicator.c/h` | 状态 LED |

### ⏳ 待完成

| 模块 | 说明 | 优先级 |
|------|------|--------|
| **components/es8311/** | 从 waveshare 移植 ES8311 驱动 | 高 |
| **components/lvgl_port/** | 从 waveshare 移植 LVGL 适配层 | 高 |
| **resources/avatar/** | Q版头像图片（idle/listening/thinking/speaking） | 中 |
| **resources/audio/** | 提示音文件 | 中 |
| **ws_protocol.c** | 添加 `cJSON` 依赖或使用 `esp_json` | 高 |
| **编译测试** | `idf.py build` 验证编译通过 | 高 |

---

## 代码总览

```
esp32_voice_companion/
├── CMakeLists.txt                    # 项目入口
├── sdkconfig.defaults                 # 配置
├── partitions.csv                     # 分区
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c                    # 主入口 (300行)
│   ├── app_state.c/h                 # 状态机
│   ├── 01_wifi/wifi_manager.c/h      # WiFi 管理
│   ├── 02_websocket/
│   │   ├── ws_client.c/h             # WS 客户端
│   │   ├── ws_protocol.c/h           # 协议解析
│   │   └── CMakeLists.txt
│   ├── 03_audio/audio_manager.c/h   # 音频管理
│   ├── 04_stt/vad_detector.c/h       # VAD
│   ├── 05_ui_avatar/avatar_ui.c/h    # 屏幕 UI
│   ├── 06_button/button_handler.c/h  # 按键
│   ├── 07_config/config_store.c/h   # NVS 配置
│   └── 08_led/led_indicator.c/h     # LED
├── components/                        # (待创建)
│   ├── es8311/                        # ES8311 驱动
│   └── lvgl_port/                     # LVGL 适配
└── resources/                         # (待创建)
    ├── avatar/                        # 头像图片
    └── audio/                         # 提示音
```

---

## 架构流程图

```
┌──────────────────────────────────────────────────────────┐
│                         app_main.c                        │
│  ┌──────────────┐    ┌──────────────┐    ┌────────────┐ │
│  │  Core 0 Task  │    │  Core 1 Task  │    │  Event Q   │ │
│  │  WiFi + WS   │    │  LVGL + VAD   │    │            │ │
│  └──────┬───────┘    └──────┬───────┘    └──────┬────┘ │
└─────────┼─────────────────────┼────────────────────┼───────┘
          │                    │                    │
     ┌────▼────┐          ┌────▼────┐          ┌──▼───┐
     │  WiFi   │          │  LVGL   │          │Event │
     │Manager  │          │ Avatar  │          │Handler│
     └────┬────┘          └────┬────┘          └──┬───┘
          │                    │                   │
     ┌────▼────────────┐  ┌────▼────┐       ┌────▼────────┐
     │  ws_client      │  │  I2S    │       │ audio_manager│
     │  WebSocket     │  │  ES8311 │       │  音频播放    │
     └────┬────────────┘  └─────────┘       └─────────────┘
          │
          ▼
   clawchat-server.js
```

---

## 状态机

```
DEEPSLEEP ──[按键/触摸]──▶ IDLE ──[BOOT 短按]──▶ LISTENING
                              ▲                      │
                              │                      ▼
                    [超时/错误]│              THINKING ◀─┐
                              │                      │
                              │              [收到回复]│
                              │                      ▼
                    [播放完毕]─┴────────────────▶ SPEAKING
                                                  │
                                                  ▼
                                               IDLE
```

---

## 下一步

1. **下载 waveshare 示例程序**
   ```bash
   cd ~/esp32_voice_companion
   git clone <waveshare_repo> components/waveshare_examples
   ```

2. **移植 ES8311 驱动**
   - 从 `06_I2SCodec` 示例提取 `es8311_codec_init()`
   - 适配到 `main/03_audio/audio_manager.c`

3. **移植 LVGL 适配层**
   - 从 `05_LVGL_WITH_RAM` 示例提取显示驱动
   - 适配到 `main/05_ui_avatar/avatar_ui.c`

4. **准备资源文件**
   - Q版头像图片（5种状态 × PNG 格式）
   - 提示音 WAV 文件

5. **编译测试**
   ```bash
   cd ~/esp32_voice_companion
   source ~/esp/esp-idf/export.sh
   idf.py set-target esp32s3
   idf.py build
   ```

---

*主人，代码框架已经完成啦～明天可以继续完善组件和资源文件！* 😊