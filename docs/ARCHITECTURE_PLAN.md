# ESP32 Voice Companion — Architecture Plan

> Inspired by [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
> Target: ESP32-S3-Touch-AMOLED-1.8 + OpenClaw/Hermes server
> Date: 2026-05-08

---

## 1. Design Goals

1. **OS-like app framework** — a pluggable `app_t` interface so new apps (Chat, Weather, Settings, Clock...) can be added without touching the core.
2. **Clean 4-layer architecture** — Board HAL → System Services → Application Framework → Apps.
3. **Single WebSocket client** — eliminate the current dual-WS-client mess; one `esp_websocket_client` instance with TLS.
4. **Real hardware drivers** — replace all stubs (ES8311, LVGL display, LED) with working implementations.
5. **Stream-mode chat** — replicate the ClawChat Android app's streaming workflow: push-to-talk → STT audio upload → streaming LLM response → TTS playback.

---

## 2. Lessons from Xiaozhi

| Xiaozhi Pattern | What We Adopt | What We Adapt |
|---|---|---|
| **Application singleton** | Central orchestrator with `init()` / `run()` / state machine | Keep C (not C++); use a global struct + function pointers instead of a class |
| **Board HAL** | `board_t` struct with `get_display()`, `get_audio_codec()`, `get_touch()` | Single board target (Waveshare AMOLED 1.8), but keep the abstraction for testability |
| **Protocol interface** | Abstract `protocol_t` with `connect()`, `send_audio()`, `send_text()`, callbacks | We only need WebSocket (not MQTT+UDP), but the interface lets us swap later |
| **App framework** | Xiaozhi doesn't have pluggable apps — this is our addition | Define `app_t` with lifecycle hooks and an app launcher UI |
| **Event-driven main loop** | FreeRTOS `xEventGroupWaitBits()` for state coordination | Same approach; replace volatile flags with proper event groups |
| **Opus audio codec** | Opus encode/decode for efficient audio transport | Start with raw PCM16 (simpler), add Opus as Phase 2 optimization |
| **Kconfig board selection** | `menuconfig` to pick board type and options | Use Kconfig for server URL, WiFi creds, debug options |

---

## 3. Target Directory Structure

```
esp32_voice_companion/
├── CMakeLists.txt
├── Kconfig.projbuild                    # Board type, server URL, debug flags
├── partitions.csv
├── sdkconfig.defaults
│
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                           # Entry: board_init() → app_framework_start()
│   │
│   ├── board/                           # LAYER 1 — Hardware Abstraction
│   │   ├── board.h                      # board_t interface (function pointers)
│   │   ├── board_waveshare_amoled18.c   # Concrete impl for our hardware
│   │   ├── audio_codec/
│   │   │   ├── audio_codec.h            # audio_codec_t interface
│   │   │   └── es8311_codec.c           # ES8311 I2C init + I2S config
│   │   ├── display/
│   │   │   ├── display.h                # display_t interface
│   │   │   └── amoled_display.c         # SPI display + LVGL driver init
│   │   ├── touch/
│   │   │   ├── touch.h                  # touch_t interface
│   │   │   └── cst816_touch.c           # CST816 I2C touch driver
│   │   └── peripherals/
│   │       ├── button.h / button.c      # BOOT key driver
│   │       └── led.h / led.c            # Status LED driver
│   │
│   ├── system/                          # LAYER 2 — System Services
│   │   ├── wifi_manager.h / .c          # WiFi STA + SmartConfig
│   │   ├── config_store.h / .c          # NVS key-value storage
│   │   ├── event_bus.h / .c             # Typed event queue (cross-core safe)
│   │   ├── power_manager.h / .c         # Deep sleep + wake source config
│   │   └── ota_update.h / .c            # OTA firmware update (future)
│   │
│   ├── framework/                       # LAYER 3 — Application Framework
│   │   ├── application.h / .c           # Singleton: init, run, state machine
│   │   ├── app_state.h / .c             # State enum + transitions
│   │   ├── protocol/
│   │   │   ├── protocol.h               # protocol_t interface
│   │   │   └── ws_protocol.c            # WebSocket impl (esp_websocket_client)
│   │   ├── audio/
│   │   │   ├── audio_pipeline.h / .c    # Capture → encode → send; recv → decode → play
│   │   │   └── vad.h / .c              # Voice activity detection
│   │   ├── display/
│   │   │   ├── display_manager.h / .c   # Scene management, LVGL task
│   │   │   ├── scene_avatar.h / .c      # Avatar + emotion display
│   │   │   ├── scene_chat.h / .c        # Chat bubble UI (streaming text)
│   │   │   └── scene_launcher.h / .c    # App launcher grid
│   │   └── input/
│   │       └── input_manager.h / .c     # Button + touch → unified events
│   │
│   └── apps/                            # LAYER 4 — Pluggable Apps
│       ├── app_interface.h              # app_t: on_enter, on_exit, on_event, on_draw
│       ├── app_registry.h / .c          # Register & lookup apps by ID
│       ├── chat/
│       │   ├── chat_app.h / .c          # OpenClaw/Hermes chat app
│       │   └── chat_session.h / .c      # Conversation state, message history
│       └── settings/                    # (future)
│           └── settings_app.h / .c
│
├── components/                          # Reusable IDF components
│   ├── cjson/                           # cJSON library
│   └── opus/                            # Opus codec (future)
│
└── resources/
    ├── avatar/                          # Avatar PNGs (idle, speaking, emotions)
    ├── icons/                           # App launcher icons
    └── fonts/                           # LVGL fonts
```

---

## 4. Key Interfaces

### 4.1 Board HAL (`board_t`)

```c
// board/board.h
typedef struct {
    const char *name;
    esp_err_t (*init)(void);                    // Initialize all peripherals
    audio_codec_t* (*get_audio_codec)(void);    // ES8311 instance
    display_t* (*get_display)(void);            // AMOLED display instance
    touch_t* (*get_touch)(void);                // Touch panel instance
    void (*get_button_gpio)(int *boot_gpio);    // Button pin
    void (*get_led_gpio)(int *led_gpio);        // LED pin
} board_t;

// Only one board compiled in (selected via Kconfig)
const board_t* board_get_instance(void);
```

### 4.2 Protocol Interface (`protocol_t`)

```c
// framework/protocol/protocol.h
typedef struct {
    esp_err_t (*connect)(const char *url, const char *token);
    void (*disconnect)(void);
    bool (*is_connected)(void);

    // Outbound
    esp_err_t (*send_hello)(const char *device_id);
    esp_err_t (*send_audio)(const int16_t *samples, size_t count, bool is_last);
    esp_err_t (*send_text)(const char *text);
    esp_err_t (*send_interrupt)(void);

    // Inbound callbacks (set by Application)
    void (*on_tts_audio)(const uint8_t *data, size_t len);
    void (*on_tts_text)(const char *text);
    void (*on_assistant_text)(const char *text);
    void (*on_status)(bool listening, bool thinking, bool speaking);
    void (*on_emotion)(const char *name, int duration_ms);
    void (*on_connected)(void);
    void (*on_disconnected)(void);
} protocol_t;

protocol_t* protocol_ws_create(void);  // WebSocket implementation
```

### 4.3 App Interface (`app_t`)

```c
// apps/app_interface.h
typedef struct {
    const char *id;              // "chat", "settings", "clock"
    const char *name;            // Display name
    const void *icon;            // LVGL image descriptor

    esp_err_t (*on_enter)(void);         // Called when app becomes active
    void (*on_exit)(void);               // Called when app loses focus
    void (*on_event)(app_event_t *evt);  // Button press, touch, WS message...
    void (*on_audio_in)(const int16_t *samples, size_t count);  // Mic data
    void (*on_tick)(uint32_t dt_ms);     // Periodic update
} app_t;
```

### 4.4 Event Bus

```c
// system/event_bus.h
typedef enum {
    EVT_WIFI_CONNECTED, EVT_WIFI_DISCONNECTED,
    EVT_WS_CONNECTED, EVT_WS_DISCONNECTED,
    EVT_BUTTON_PRESS, EVT_BUTTON_RELEASE, EVT_BUTTON_LONG_PRESS,
    EVT_TOUCH_TAP, EVT_TOUCH_SWIPE,
    EVT_AUDIO_TTS_END,
    EVT_STATE_CHANGED,
    EVT_APP_SWITCH,
} event_type_t;

typedef struct {
    event_type_t type;
    union { /* event-specific payload */ } data;
} event_t;

esp_err_t event_bus_init(void);
esp_err_t event_bus_post(const event_t *evt);              // ISR-safe version available
esp_err_t event_bus_subscribe(event_type_t type, event_handler_fn fn, void *ctx);
```

---

## 5. Application Lifecycle

```
Power On
  │
  ▼
board_init()          ← LAYER 1: ES8311, Display, Touch, Button, LED
  │
  ▼
system_init()         ← LAYER 2: WiFi, NVS, EventBus, PowerMgr
  │
  ▼
application_init()    ← LAYER 3: Protocol, AudioPipeline, DisplayMgr, InputMgr
  │
  ▼
app_registry_init()   ← LAYER 4: Register ChatApp, SettingsApp, ...
  │
  ▼
application_run()     ← Main event loop (Core 0)
  │
  ├─ Wait for WiFi connected
  ├─ Protocol connect (WSS)
  ├─ Show app launcher (or auto-launch ChatApp)
  │
  └─ Event loop:
       ├─ LVGL timer handler (display refresh)
       ├─ Event bus dispatch (button, touch, WS messages)
       ├─ Active app's on_tick()
       ├─ Audio pipeline tick (if recording/playing)
       └─ Power manager check (idle → deep sleep)
```

---

## 6. ChatApp Flow (Stream Mode)

This replicates the ClawChat Android app's streaming workflow:

```
User presses BOOT button
  │
  ▼
InputMgr → EVT_BUTTON_PRESS → ChatApp.on_event()
  │
  ▼
ChatApp enters LISTENING state
  ├─ AudioPipeline.start_capture()
  ├─ DisplayMgr.show_scene(scene_avatar, STATE_LISTENING)
  └─ LED: breathing blue
  │
  ▼
AudioPipeline reads 40ms chunks from I2S DMA
  ├─ Optional: VAD checks energy level
  └─ Protocol.send_audio(chunk, is_last=false)
  │
  ▼
User releases BOOT button
  │
  ▼
InputMgr → EVT_BUTTON_RELEASE → ChatApp.on_event()
  ├─ Protocol.send_audio(last_chunk, is_last=true)
  ├─ AudioPipeline.stop_capture()
  └─ ChatApp enters THINKING state
  │
  ▼
Server sends streaming response:
  ├─ on_status(thinking=true)   → avatar shows "thinking" animation
  ├─ on_assistant_text(chunk)   → scene_chat appends text (streaming)
  ├─ on_emotion("happy", 2000)  → avatar shows emotion briefly
  ├─ on_tts_audio(pcm_data)    → AudioPipeline.play(pcm_data)
  └─ on_status(speaking=true)   → ChatApp enters SPEAKING state
  │
  ▼
TTS playback finishes
  └─ EVT_AUDIO_TTS_END → ChatApp enters IDLE state
```

---

## 7. Task Pinning & Memory

| Task | Core | Stack | Purpose |
|---|---|---|---|
| `app_main_task` | 0 | 8KB | Application.run() event loop + LVGL |
| `audio_capture_task` | 1 | 4KB | I2S DMA read → chunk queue |
| `audio_playback_task` | 1 | 4KB | Chunk queue → I2S DMA write |
| `wifi_task` | 0 | 4KB | WiFi event handling (ESP-IDF internal) |
| `ws_task` | 0 | 6KB | WebSocket client (esp_websocket_client internal) |

Key change from current code: LVGL runs on Core 0 in the main loop (not a separate task). Audio capture/playback are the only tasks on Core 1, avoiding contention.

---

## 8. Migration Plan (from current code)

### Phase A: Restructure (no new features)

1. Create the directory structure above
2. Move `wifi_manager.c/h` → `main/system/`
3. Move `config_store.c/h` → `main/system/`
4. Move `app_state.c/h` → `main/framework/`
5. Create `board.h` interface, wrap existing stub code into `board_waveshare_amoled18.c`
6. Create `event_bus.c/h` to replace the current `g_event_queue` + volatile flags
7. Create `protocol.h` interface, consolidate `ws_client.c` + `openclaw_bridge.c` into `ws_protocol.c`
8. Delete duplicate `openclaw_bridge/` at repo root
9. Verify it still compiles with `idf.py build`

### Phase B: Real Hardware Drivers

1. Port ES8311 driver from Waveshare examples → `board/audio_codec/es8311_codec.c`
2. Port LVGL display driver → `board/display/amoled_display.c`
3. Port CST816 touch driver → `board/touch/cst816_touch.c`
4. Implement real LED GPIO control
5. Verify audio capture + playback on hardware

### Phase C: App Framework + ChatApp

1. Implement `app_t` interface and `app_registry`
2. Implement `scene_launcher` (app grid UI)
3. Refactor current chat logic into `apps/chat/chat_app.c`
4. Implement `scene_chat` with streaming text bubbles
5. Implement `audio_pipeline` with proper encode/decode path
6. End-to-end test: press button → record → server → streaming response → TTS

### Phase D: Polish & Extend

1. Add Opus codec support (smaller audio packets)
2. Implement VAD-based auto-stop (hybrid mode)
3. Add OTA update support
4. Add Settings app (WiFi config, server URL, volume)
5. Add more apps (Clock, Weather, ...)

---

## 9. Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Language | C (not C++) | Consistent with ESP-IDF ecosystem; xiaozhi uses C++ but we don't need STL. Function pointers give us polymorphism. |
| WS Client | `esp_websocket_client` | Official IDF component, TLS built-in, reconnect support. Eliminates the hand-rolled TCP socket. |
| Audio format | PCM16 first, Opus later | PCM16 is simpler to debug. Opus adds compression but also complexity. |
| Display framework | LVGL | Already chosen; matches the hardware (SPI display + touch). |
| App model | Single active app | One app at a time (no multitasking). Launcher is a special "home" screen. |
| State machine | In Application, not in each app | Application owns global state (IDLE/LISTENING/THINKING/SPEAKING). Apps react to state changes. |

---

*This plan supersedes PROGRESS.md and should be used as the reference architecture going forward.*
