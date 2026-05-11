# ESP32 Voice Companion — 架构审查报告

**审查日期**：2026-05-02  
**审查范围**：`/Users/agjacob17/esp32_voice_companion`  
**固件版本**：Phase 2（WebSocket 文本协议，已完成）；Phase 3（音频）待实现  

---

## 一、目录结构总览

```
esp32_voice_companion/
├── CMakeLists.txt                        # 项目顶层
├── sdkconfig / sdkconfig.defaults        # ESP-IDF 配置
├── partitions.csv                        # Flash 分区表
├── idf_project.yml                      # (IDF Component Registry 配置)
├── dependencies.lock                     # 依赖锁定文件
│
├── main/                                 # 主应用（xiaozhi 固件为基础）
│   ├── CMakeLists.txt                    # 列出 40+ .cc/.c 源文件（xiaozhi 本体）
│   ├── main.cc                           # 入口点（调用 openclaw_bridge_start()）
│   ├── app_main.c                        # Phase-1 STT 录音会话管理（自研）
│   ├── app_state.c/h                     # 状态机（INIT/IDLE/LISTENING/THINKING/SPEAKING/SLEEP/ERROR）
│   ├── 01_wifi/wifi_manager.c/h          # WiFi 连接管理
│   ├── 02_websocket/ws_client.c/h        # WS 客户端（esp_websocket_client）
│   ├── 02_websocket/ws_protocol.c/h      # 协议解析 + stt_audio base64 编码
│   ├── 03_audio/audio_manager.c/h        # ES8311 + I2S + 40ms chunk queue
│   ├── 04_stt/vad_detector.c/h            # 能量检测 VAD（简单实现）
│   ├── 05_ui_avatar/avatar_ui.c/h         # LVGL 头像 UI
│   ├── 06_button/button_handler.c/h      # BOOT 按键回调
│   ├── 07_config/config_store.c/h         # NVS 配置存储
│   └── 08_led/led_indicator.c/h          # LED 状态指示
│
├── components/                           # 外部组件（openclaw_bridge）
│   └── openclaw_bridge/                  # OpenClaw 网桥（Phase 1/2 自研）
│       ├── CMakeLists.txt
│       ├── openclaw_bridge.c/h           # 主任务：WS 握手 + JSON 解析 + 状态机
│       ├── openclaw_ws_client.c/h        # 底层 TCP socket（Phase 2 plain TCP）
│       ├── openclaw_display.cpp/h        # C++ 显示桩函数
│       ├── cJSON.c/h                      # 嵌入式 JSON 解析器
│       └── openclaw_bridge_local.h        # 服务器地址/Token 配置
│
├── openclaw_bridge/                      # ⚠️ 重复副本！与 components/openclaw_bridge 内容相同
│   └── （同上的 9 个文件）
│
└── resources/avatar/                     # 头像 PNG 资源
    └── avatar_idle.png / avatar_speaking.png / emotion_*.png
```

**发现：存在两个相同的 `openclaw_bridge` 目录**，分别在 `components/openclaw_bridge/` 和 `repo根目录/openclaw_bridge/`。构建系统只引用了 `components/` 下的版本，根目录副本是冗余的。

---

## 二、文档现状核查

| 文档 | 最后更新 | 与代码实际符合度 |
|------|---------|---------------|
| `DEVELOPMENT_STATUS.md`（根目录） | 2026-04-29 | ✅ 基本准确，Phase 2 已完成，Phase 3 待实现 |
| `docs/DEVELOPMENT_STATUS.md` | 2026-04-29 | ✅ 同上（重复副本） |
| `PROGRESS.md` | 2026-04-28 | ⚠️ 部分过时：描述的架构与实际不符（如 ws_protocol.c 声称"待添加 cJSON 依赖"但实际已用 mbedtls_base64） |
| README.md | — | ❌ 文件不存在 |

**TODO 核查**（从 DEVELOPMENT_STATUS.md）：

| TODO 项 | 状态 | 备注 |
|--------|------|------|
| 音频 I2S 引脚验证 | ❌ 未解决 | 代码中 GPIO 15/16/17/18 硬编码，未与硬件 schematic 核对 |
| Phase 3 代码：音频采集 + stt_audio 发送 | 🟡 部分实现 | audio_manager.c 有 chunk queue，但 `audio_play_tts()` 是空壳 |
| Phase 3 代码：TTS 播放 | ❌ 未实现 | `audio_play_tts()` 只有 `vTaskDelay(2000)` 模拟 |
| Phase 3 代码：LED 状态 | ❌ 未实现 | `led_indicator.c/h` 存在但未实际控制 GPIO |
| Phase 3 代码：真实显示 HW | ❌ 未实现 | `openclaw_display.cpp` 只有 ESP_LOG 桩 |
| Phase 3 代码：mbedtls TLS (wss://) | ❌ 未实现 | `openclaw_ws_client.c` 用 plain TCP，`openclaw_bridge_local.h` 定义 `CLAWCHAT_WS_SECURE=1` 但未实现 |
| Phase 4：VAD 免提模式 | ❌ 未实现 | `vad_detector.c` 存在但能量阈值硬编码，未与录音流程集成 |
| Phase 4：部分转写 UX | ❌ 未实现 | |
| Phase 4：打断正在说话 | ❌ 未实现 | `ws_client_send_interrupt()` 存在但 app_main.c 未调用 |

---

## 三、架构问题与优化建议（7项）

---

### 问题 1：两套并行的 WebSocket 客户端实现——架构冲突

**问题描述**  
项目中存在**两套独立且功能重叠的 WS 客户端**：

1. **`main/02_websocket/ws_client.c/h`** — 使用 `esp_websocket_client`（官方 IDF 组件，支持 SSL/TLS），被 `app_main.c` 调用，用于 Phase-1 STT 音频发送
2. **`components/openclaw_bridge/openclaw_bridge.c`** — 纯手写 TCP socket，自己解析 WebSocket 帧（opcode 0x81），用于 Phase-2 "OpenClaw Bridge"（独立于 xiaozhi 运行）

两套完全独立，各有各的连接逻辑、帧解析、和 hello 消息格式。它们连接到**同一个服务器**（`clawchat.xuanyu.uk`），但使用不同的设备 ID、消息格式、和 TLS 实现。

**自我批评**  
这是典型的"渐进式原型开发"遗留问题：先快速在 `main/` 里实现一版，后来为了整合 OpenClaw 又写了一套独立的 bridge。代码里没有任何抽象层来统一这两个 WS 客户端，导致维护困难、行为不一致（比如 `main/` 版本用 esp_websocket_client 的心跳定时器，bridge 版本用 select 超时）。

**替代方案**
- **方案 A**（推荐）：保留 `ws_client.c/h`（成熟、官方维护、TLS 支持），删除 `openclaw_bridge/openclaw_ws_client.c` 的独立实现，让 `openclaw_bridge.c` 通过 `ws_client.c` 的 API 接入
- **方案 B**：保留 bridge 的独立实现，删除 `ws_client.c/h`（简化依赖，但失去 esp_websocket_client 的稳定性）
- **方案 C**：用适配器模式统一两层，下层为通用 WS client，上层分叉为"STT 专用"和"Bridge 专用"

**最优解**  
采用方案 A：`openclaw_bridge.c` 应作为调用方而非重复实现者。将 bridge 的 WS 需求作为 `ws_client.c/h` 的扩展（如 `ws_client_send_custom(type, json)`），统一用 `esp_websocket_client` 处理 TLS。重写 `openclaw_bridge.c`，不再直接持有 socket fd，改为注册回调函数接收服务器消息。

**状态**：待解决

---

### 问题 2：Phase 2 的 openclaw_bridge 根本不支持 TLS（严重）

**问题描述**  
`openclaw_bridge_local.h` 定义了：
```c
#define CLAWCHAT_WS_PORT 443
#define CLAWCHAT_WS_SECURE 1
```
但 `openclaw_ws_client.c` 的 `openclaw_ws_connect()` 完全忽略 `tls` 参数，只做 plain TCP connect，返回一个 raw socket fd。这意味着它实际连接到的是 `clawchat.xuanyu.uk:443` 的 **plain TCP**，而不是 TLS/WSS。服务器端如果只接受 TLS，这个连接会在握手阶段直接失败或返回非 WS 响应。

同时，Phase 2 的 WS 握手请求通过**明文 HTTP Upgrade** 发送，但端口 443 通常需要 TLS。代码里的 WS 握手包含 `Authorization: Bearer <token>` 以明文方式传输，安全性极差。

**自我批评**  
Phase 2 代码注释说"TLS requires esp_tls component which is not available in MINIMAL_BUILD"，这是合理的约束，但代码并未正确处理这一约束——它没有 fallback 到 HTTP (port 80)，而是继续用 port 443 plain TCP，这是自欺欺人。如果 Phase 2 能 work，说明服务器同时监听 443 plain TCP（几乎不可能）或 DTR 问题导致根本没连上。

**替代方案**
- **方案 A**：服务器同时在 HTTP (80) 和 HTTPS (443) 提供 WS，用 `esp_websocket_client` 处理 TLS（`WEBSOCKET_TRANSPORT_OVER_SSL`），它会自动处理证书验证和重连
- **方案 B**：在 `openclaw_ws_client.c` 中手动实现 mbedtls TLS（复杂度高，但可行）
- **方案 C**：将 bridge 也迁移到使用 `esp_websocket_client`，由其统一处理 TLS

**最优解**  
采用方案 A + C 结合：统一 WS 客户端层，`esp_websocket_client` 内置 TLS 支持，配置 `.transport = WEBSOCKET_TRANSPORT_OVER_SSL`。删除 `openclaw_ws_client.c` 的手写 socket 实现。

**状态**：待解决

---

### 问题 3：macOS USB-JTAG DTR 问题——文档与实现存在断层

**问题描述**  
`DEVELOPMENT_STATUS.md` 记录了 macOS USB-JTAG driver 在打开 `/dev/cu.usbmodem1101` 时将 DTR 拉高，导致 ESP32-S3 进入 ROM download mode 而非启动 app。文档提供了 workaround（`--after no_reset`），但：

1. **esptool flash 流程并未应用 `--after no_reset`**，实际烧录时会触发 ROM download mode，导致固件无法正常启动
2. **启动后无法获取日志输出**：文档说"Windows 上用 idf.py monitor 直接可用"，但没有给出 macOS 上可靠的日志获取方案
3. **没有找到确认 ESP32 实际运行了哪个固件的可靠方法**

**自我批评**  
文档记录了问题但没有给出完整可用的 macOS 开发方案。目前的状态是：macOS 用户可以烧录（使用 `--after no_reset`），但无法验证固件是否正确运行。这是一个实质性的开发体验障碍。

**替代方案**
- **方案 A**：使用 CH340 USB-UART 适配器（低成本，但需要额外硬件）
- **方案 B**：通过 WiFi 日志远程调试（ESP_LOG 通过 UDP 输出）
- **方案 C**：使用 WSL/Windows VM 进行开发（最可靠但额外开销大）
- **方案 D**：在 ESP32 启动时通过 GPIO 输出状态码到 LED（如启动成功闪3下，WS连接成功闪5下）

**最优解**  
综合方案：在 `DEVELOPMENT_STATUS.md` 中添加完整的 macOS 开发指南，包括：（1）使用 `--after no_reset` 烧录 + 断电重启的完整流程；（2）ESP32 启动时的 LED 状态指示（需要在 `app_main.c` 启动早期添加 LED 反馈）；（3）WiFi connected 后通过 UDP log 远程查看日志的方法。

**状态**：部分解决（已知问题，文档存在但流程不完整）

---

### 问题 4：组件分层混乱——openclaw_bridge 与 xiaozhi 的边界不清

**问题描述**  
`openclaw_bridge` 的定位是"独立于 xiaozhi 运行"的组件，文档声称"不修改 xiaozhi 已有流程"。但实际情况：

1. `main/main.cc`（xiaozhi 入口）调用 `openclaw_bridge_start()`，但没有等待其初始化完成
2. `openclaw_bridge` 和 `app_main.c`（Phase-1 STT）各自维护独立的状态机，存在冲突风险（如两个任务同时读写 display）
3. `openclaw_bridge.c` 的 `bridge_task` 运行在 **core 1**（与 app_main 共享），`record_session_task` 运行在 **core 0**——跨核通信通过 volatile 标志 `g_button_pressed_flag` 和 `g_recording_active`，但没有 FreeRTOS queue 或 semaphore 保护
4. `openclaw_display.cpp` 中的函数是 stub（ESP_LOG only），但 `openclaw_bridge.c` 和 `app_main.c` 都调用显示函数——未来升级到真实 HW 时需要统一协调

**自我批评**  
架构设计时过度强调"不修改 xiaozhi"，导致 bridge 和 app_main 形成两个独立的子系统，没有共同的抽象层。特别是 Core 0/1 之间的 volatile 标志通信缺乏同步原语，是潜在的竞争条件。

**替代方案**
- **方案 A**：将 bridge 作为 app_main 的子模块而非独立组件，统一状态机
- **方案 B**：定义通用的 `display interface`（函数指针结构），统一由 avatar_ui.c 实现
- **方案 C**：Bridge 只负责 WS 通信，UI 更新全部通过 app_main 的事件队列

**最优解**  
采用方案 B + C：定义 `display_ops_t` 接口结构，指向 `avatar_ui.c/h` 的实现函数。Bridge 和 app_main 都通过这个接口更新显示，消除重复的桩函数。跨核通信改用 `xQueue` 而非 volatile 轮询。

**状态**：待解决

---

### 问题 5：ES8311 音频驱动是空壳——Phase 3 核心功能缺失

**问题描述**  
`audio_manager.c` 中：
- `es8311_init()` 只有 `ESP_LOGI("ES8311 init (simplified)")`，没有实际的 I2C 寄存器配置
- `audio_play_tts()` 只有 `vTaskDelay(2000)` 模拟，没有真实的 TTS 获取和解码播放
- I2S 引脚（GPIO 15/16/17/18）是硬编码的，未与硬件 schematic 核对
- 没有实现 ADC 采集（录音）的开始/停止时序

代码中大量 `TODO: 实现` 和 `// TODO: 与 STT 模块集成` 的注释，表明 Phase 3 的核心音频功能尚未实现。

**自我批评**  
Phase 2 的 WS 文本协议完成度不错（90%），但 Phase 3 的音频部分几乎没有实质进展。ES8311 的 I2C 配置是芯片正常工作的前提，没有这个后续功能都是空中楼阁。代码里已经建立了 chunk queue 和 accumulator 框架，但实际的 I2S DMA 读取和 ES8311 配置还是 stub。

**替代方案**
- **方案 A**：从 waveshare 示例程序移植完整的 ES8311 I2C 配置代码
- **方案 B**：使用 ESP-ADF（Audio Development Framework）中的 ES8311 驱动组件
- **方案 C**：先验证 I2S 引脚连接，用纯 I2S DMA 循环测试确认硬件通，再逐步实现音频功能

**最优解**  
采用方案 A + C：先用示波器或逻辑分析仪验证 I2S 引脚接线，从 waveshare 示例提取 `es8311_codec_init()` 实现移植到 `audio_manager.c` 的 `es8311_init()`。TTS 播放流程：HTTP GET TTS API → 解码 → I2S DMA 输出。

**状态**：待解决

---

### 问题 6：VAD 与录音流程未集成——Phase 4 功能缺失

**问题描述**  
- `vad_detector.c` 实现了简单的能量检测 VAD（阈值 500/1500），但从未在录音流程中被调用
- `app_main.c` 的 `record_session_task` 没有使用 VAD，是纯按钮驱动的"按多久录多久"模式
- VAD 的 `sensitivity` 参数（0-3）没有任何运行时配置机制
- Phase 4 计划"VAD-based hands-free mode"还未开始

**自我批评**  
VAD 已经写好但没有被使用，这说明录音会话的设计是"按钮驱动"而非"VAD 驱动"。如果要走免提路线，需要重新设计录音的触发和结束逻辑，而不仅仅是把 VAD 嵌进去。

**替代方案**
- **方案 A**：在按钮按下时启动 VAD，检测到语音结束后自动停止（混合模式）
- **方案 B**：完全免提——始终运行 VAD，检测到语音段后自动开始录音
- **方案 C**：保持纯按钮驱动，删除 VAD 模块以简化代码（如果 UX 不需要免提）

**最优解**  
方案 A（混合模式）：按钮按下开始录音 + VAD 检测静音自动结束。这样兼顾可靠性和免提潜力，是最实际的折中方案。

**状态**：待解决

---

### 问题 7：ws_protocol_build_stt_audio 中的 Base64 编码存在安全风险

**问题描述**  
在 `ws_protocol.c` 的 `ws_protocol_build_stt_audio()` 中：
```c
char b64_buf[2048];
int b64_len_out = base64_encode(..., b64_buf, sizeof(b64_buf));
```
固定 2048 字节的栈缓冲区。640 samples × 2 bytes = 1280 bytes → base64 编码后约 1707 bytes，可以容纳。但如果采样率或 chunk 大小改变，这里会溢出。

更严重的是，这个函数在 `audio_manager.c` 的实时录音循环中调用，栈空间在 Core 0 的 `audio_capture_task`（4096 bytes 栈）中。这个栈同时还承载 I2S DMA 读取和其他处理，2048 字节的栈分配增加了栈溢出风险。

**自我批评**  
这是一个"看起来能 work 但不健壮"的实现。1280 字节的 PCM16 base64 后确实在 2048 范围内，但硬编码的大小限制了未来的扩展性。如果未来改为 16kHz 以上的采样率或更大的 chunk，栈溢出是必然的。

**替代方案**
- **方案 A**：使用堆分配（`malloc`）动态分配 base64 缓冲区，通过 `free()` 释放
- **方案 B**：使用静态全局缓冲区（避免栈问题，但需要注意重入）
- **方案 C**：预分配固定大小的 buffer pool（8 × 2048，轮转使用）

**最优解**  
采用方案 C（静态 buffer pool）+ 在编译期通过静态断言确保 buffer 大小足够：
```c
_Static_assert(AUDIO_CHUNK_SAMPLES * 4 / 3 + 64 < B64_BUF_SIZE, "...");
```
在实时音频路径上避免 malloc/free（避免碎片化和分配失败）。

**状态**：待解决

---

## 四、架构评分总览

| 维度 | 评分（1-5）| 说明 |
|------|-----------|------|
| **分层清晰度** | 2 | 两套 WS 客户端并存，bridge 与 xiaozhi 边界不清 |
| **模块独立性** | 3 | openclaw_bridge 可独立运行，但与 app_main 共享硬件资源 |
| **内存管理** | 2 | chunk queue 固定 8×640 合理，但 Base64 栈风险存在 |
| **文档准确性** | 3 | DEVELOPMENT_STATUS 基本准确，PROGRESS.md 有过时内容 |
| **Phase 3 准备度** | 2 | 框架搭建完好，但核心音频（ES8311/TTS）是空壳 |
| **macOS 开发支持** | 2 | 问题已知但可用性不足，缺少完整的验证方案 |
| **安全性** | 2 | Bearer token 明文传输，port 443 plain TCP |

---

## 五、TODO 汇总

### 已完成 ✅
- Phase 1：代码框架搭建（所有模块的 .c/.h 文件存在）
- Phase 2：WebSocket 文本协议（ws_client.c/h 完整，bridge 能 parse JSON）
- 状态机定义（app_state.c/h）
- LED 桩函数和按键回调框架

### 进行中 🟡
- `openclaw_bridge.c` Phase 2 WS 连接（能 work 但 TLS 是假的）
- `ws_protocol_build_stt_audio()` 功能存在但未与录音流程完全集成

### 待实现 ❌（共 9 项）
1. 音频 I2S 引脚与硬件 schematic 核对（阻塞 Phase 3 所有功能）
2. ES8311 完整 I2C 初始化（从 waveshare 移植）
3. TTS HTTP API 获取 + 解码 + I2S 播放
4. `openclaw_display.cpp` 真实显示 HW 集成（替代 stub）
5. mbedtls TLS / WSS 支持（当前是 plain TCP 伪实现）
6. 跨核通信改用 `xQueue`（替换 volatile 轮询标志）
7. VAD 与录音流程集成（混合模式：按钮+VAD 自动停止）
8. LED 真实 GPIO 控制（替代桩）
9. ws_protocol.c Base64 buffer 栈安全问题修复

---

## 六、优先修复建议

**第一优先级（阻塞项）**：
1. **统一 WS 客户端层** — 消除 `openclaw_ws_client.c` 的重复实现，让 bridge 使用 `ws_client.c/h`
2. **实现 ES8311 I2C 配置** — 这是所有音频功能的前提，阻塞 Phase 3 全部功能

**第二优先级（稳定性）**：
3. 修复 Base64 栈缓冲区风险
4. 跨核 volatile 标志改为 FreeRTOS queue

**第三优先级（功能性）**：
5. TTS 播放流程（HTTP → decode → I2S）
6. VAD 集成（混合模式）
7. LED 真实 GPIO 控制

---

*本报告为架构审查性质，发现的问题需要结合实际硬件验证。ES8311 引脚配置和音频功能需要在硬件测试后才能最终确认方案可行性。*