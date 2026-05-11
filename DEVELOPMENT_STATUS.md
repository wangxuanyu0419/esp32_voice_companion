# ESP32 OpenClaw Bridge — Development Status

## Last Updated
**2026-04-29**

---

## Project Overview

**Goal**: Connect ESP32-S3 (running xiaozhi firmware) to OpenClaw server via WebSocket, enabling voice input/output + Live2D avatar UI.

**Repo**: `https://github.com/wangxuanyu0419/esp32-voice-companion`
**Firmware Directory**: `05_firmware/firmware/`

---

## Architecture

```
ESP32-S3 (xiaozhi firmware)
  ├── app_main()                  ← modified to call openclaw_bridge_start()
  ├── xiaozhi components         ← Wi-Fi / Display / Audio / LVGL (unchanged)
  │
  └── components/openclaw_bridge/ ← NEW OpenClaw bridge component
        ├── openclaw_bridge.c     ← main loop: WS handshake, JSON parse, state machine
        ├── openclaw_ws_client.c  ← raw TCP socket client (Phase 2: plain TCP, Phase 3: mbedtls TLS)
        ├── openclaw_display.cpp  ← C++ display API (Phase 2: ESP_LOG stubs, Phase 3: real HW)
        ├── cJSON.c/h             ← embedded JSON parser
        └── CMakeLists.txt

OpenClaw Server (Mac: clawchat-server.js)
  └── wss://clawchat.xuanyu.uk/ws (or localhost:8081 for local dev)
        ├── stt_audio input (Phase 3)
        └── returns: assistant_text / tts_text / live2d / status
```

---

## Git Commits

| Commit | Message |
|--------|---------|
| `40b5a36` | docs: sync local esp32 prototype and add STT integration plan |
| `ee97b89` | feat(openclaw_bridge): add OpenClaw Bridge component, Phase 1 text control path |
| `59d0a24` | feat(openclaw_bridge): Phase 2 — real WS protocol + disconnect/reconnect loop |

**Latest**: `59d0a24` on `main` branch — Phase 2 built and flashed to device.

---

## Phase Progress

### Phase 1 ✅ — Codebase Setup
- [x] Sync local prototype to repo
- [x] Add openclaw_bridge component skeleton
- [x] Call `openclaw_bridge_start()` from `app_main()` alongside xiaozhi init
- [x] Plain TCP socket connect (Phase 1 test)
- [x] ESP_LOG-based display stubs
- [x] Build + flash

### Phase 2 ✅ — Real WS Protocol
- [x] WS frame format (opcode 0x81) parsing
- [x] hello → ready handshake
- [x] JSON parse: status / assistant_text / tts_text / live2d / error
- [x] Disconnect → reconnect loop (MAX 3 retries, then "Server unreachable")
- [x] `openclaw_ws_client.c` with proper connect/write/read/close API
- [x] Build + flash

### Phase 3 ⏳ — Audio (next)
- [ ] ES8311 I2S audio capture (PCM16 mono 16kHz)
- [ ] Button-driven recording (press to record, release to finalize)
- [ ] stt_audio message building (base64 PCM16 chunks)
- [ ] TTS playback path
- [ ] Real display HW integration (board.h display API)
- [ ] LED status indication

### Phase 4 📋 — Polish
- [ ] mbedtls TLS (wss://) — currently using plain HTTP (port 80)
- [ ] VAD-based auto-stop
- [ ] Partial transcript UX
- [ ] Interrupt while speaking

---

## Key Files

### Modified by OpenClaw Bridge
| File | Change |
|------|--------|
| `main/main.cc` | +`#include "openclaw_bridge.h"` + call `openclaw_bridge_start()` |
| `main/CMakeLists.txt` | Added `components` to INCLUDE_DIRS + `openclaw_bridge` to PRIV_REQUIRES |
| `components/openclaw_bridge/*` | 9 new files (see Architecture above) |

### Reference Files (xiaozhi, read-only)
| File | Purpose |
|------|---------|
| `main/main.cc` | app entry point |
| `main/application.cc/h` | xiaozhi init + run loop |
| `main/boards/bread-compact-wifi/compact_wifi_board.cc` | board init (Wi-Fi, display, audio) |
| `main/protocols/websocket_protocol.cc` | reference WS client implementation |
| `main/display/emote_display.cc/h` | display API (SetStatus, SetChatMessage, SetEmotion, ShowNotification) |

---

## Build & Flash (macOS)

```bash
# Source IDF
. ~/esp/esp-idf-v5.5.2/export.sh

# Build
cd 05_firmware/firmware
idf.py build

# Flash (all sections)
python3 ~/esp/esp-idf-v5.5.2/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 \
  write_flash \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin \
  0x20000 build/clawchat-esp32.bin \
  0x800000 build/generated_assets.bin
```

**Note**: On macOS, opening `/dev/cu.usbmodem1101` sets DTR HIGH, which puts ESP32-S3 into ROM download mode. Use `--after no_reset` to avoid this. See Serial Debug section below.

---

## Serial Debug (macOS Limitation)

**Problem**: macOS USB-Serial-JTAG driver holds DTR high when port opens → ESP32-S3 enters ROM download mode instead of booting app.

**Workarounds**:
1. **Windows**: Use `idf.py monitor` directly — works correctly
2. **screen**: `screen /dev/cu.usbmodem1101 115200` (may also show blank)
3. **esptool run**: `idf.py -p /dev/cu.usbmodem1101 flash` then `idf.py -p /dev/cu.usbmodem1101 monitor`

**Recommended**: Develop on Windows or use a USB-UART adapter (e.g. CH340) instead of the native USB-JTAG.

---

## Windows Setup (for continued development)

### Step 1: Clone repo
```bash
git clone https://github.com/wangxuanyu0419/esp32-voice-companion.git
cd esp32-voice-companion
```

### Step 2: Install ESP-IDF
Download from: https://dl.espressif.com/dl/esp-idf-windows-setup.exe
Or use ESP-IDF Tools installer for Windows

### Step 3: Checkout project
```bash
cd 05_firmware/firmware
idf.py set-target esp32s3
idf.py menuconfig   # (optional, for Wi-Fi SSID/password)
idf.py build
idf.py flash -p COM3  # replace COM3 with actual port
idf.py monitor       # ← this should work on Windows!
```

### Step 4: Configure Wi-Fi
In `menuconfig` or `sdkconfig.defaults`:
```
CONFIG_WIFI_SSID="YourSSID"
CONFIG_WIFI_PASSWORD="YourPassword"
```

---

## Server Connection

**Server**: `wss://clawchat.xuanyu.uk/ws` (public, no auth)
**Fallback (local Mac)**: `ws://localhost:8081/ws`

For local development on Mac, run:
```bash
cd ~/clawchat-companion
pm2 start ecosystem.config.js  # starts server on :8081
```

Then update `openclaw_bridge.c` to use `ws://YOUR_MAC_IP:8081/ws` during local dev.

---

## Phase 3 — Audio Implementation Notes

### Audio Format
- PCM16 mono 16kHz
- Chunk size: 40ms = 640 samples = 1280 bytes → ~1730 bytes base64

### stt_audio Message Format
```json
{
  "type": "stt_audio",
  "audio": "<base64 PCM16 data>",
  "isLast": false  // or true on release
}
```

### Server Side
- clawchat-companion `feature/stt-audio-buffer` branch handles `stt_audio` input
- Final transcript goes through existing `user_text` reply pipeline
- Returns: `status` / `assistant_text` / `tts_text` / `live2d`

### ES8311 I2S Pins (to verify from board schematic)
- I2S data pin: GPIO?? (TBD from hardware)
- I2S bclk: GPIO??
- I2S lrck: GPIO??
- I2C SDA: GPIO??
- I2C SCL: GPIO??

---

## Open Questions / TODOs

- [ ] Verify audio I2S pins from hardware schematic
- [ ] Phase 3 code: audio capture + stt_audio sending
- [ ] Phase 3 code: TTS playback (I2S to speaker)
- [ ] Phase 3 code: LED status (GPIO pin TBD)
- [ ] Phase 3 code: real display HW integration (board.h cascading deps issue)
- [ ] Phase 3 code: mbedtls TLS for wss://
- [ ] Phase 4: VAD-based hands-free mode
- [ ] Phase 4: partial transcript UX
