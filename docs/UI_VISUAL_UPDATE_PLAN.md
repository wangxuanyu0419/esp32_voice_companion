# UI Visual Update Plan — Xiaozhi-Style Fonts, Icons & Colors

**Goal:** Update every screen in the project to match the Xiaozhi ESP32 project's visual style — primarily fonts (CJK-capable text + icon font), color themes, status bar icons, and WeChat-style chat bubbles.

**Reference:** [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) — specifically `main/display/lcd_display.cc`, `main/display/lvgl_display/lvgl_theme.h`, and the Waveshare AMOLED 1.8 board config.

**Target hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 SH8601 QSPI panel).

---

## 1. Centralised Theme System (`ui_theme.h` / `ui_theme.c`)

Create `main/framework/display/ui_theme.h` and `ui_theme.c` to replace all per-file hardcoded colour `#define`s.

### 1.1 Theme struct

```c
typedef struct {
    lv_color_t bg;                 // screen background
    lv_color_t text;               // primary text
    lv_color_t chat_bg;            // chat area background
    lv_color_t user_bubble;        // user message bubble
    lv_color_t assistant_bubble;   // assistant message bubble
    lv_color_t system_bubble;      // system/status message bubble
    lv_color_t system_text;        // system message text
    lv_color_t border;             // general border/separator
    lv_color_t low_battery;        // low-battery warning colour
} ui_theme_t;
```

### 1.2 Built-in themes (from Xiaozhi)

**Dark theme** (default for AMOLED):

| Property           | Hex        | Xiaozhi source                |
|--------------------|------------|-------------------------------|
| bg                 | `0x000000` | `dark_theme->background`      |
| text               | `0xFFFFFF` | `dark_theme->text`            |
| chat_bg            | `0x1F1F1F` | `dark_theme->chat_background` |
| user_bubble        | `0x00FF00` | `dark_theme->user_bubble`     |
| assistant_bubble   | `0x222222` | `dark_theme->assistant_bubble`|
| system_bubble      | `0x333333` | (Xiaozhi default)             |
| system_text        | `0x999999` | (Xiaozhi default)             |
| border             | `0x333333` | (Xiaozhi default)             |
| low_battery        | `0xFF0000` | (Xiaozhi default)             |

**Light theme** (optional, for future toggle):

| Property           | Hex        |
|--------------------|------------|
| bg                 | `0xFFFFFF` |
| text               | `0x000000` |
| chat_bg            | `0xE0E0E0` |
| user_bubble        | `0x00FF00` |
| assistant_bubble   | `0xDDDDDD` |
| system_bubble      | `0xCCCCCC` |
| system_text        | `0x666666` |
| border             | `0xCCCCCC` |
| low_battery        | `0xFF0000` |

### 1.3 API

```c
void            ui_theme_init(void);              // call once at boot
const ui_theme_t *ui_theme_get(void);             // returns active theme
void            ui_theme_set_dark(bool dark);      // switch theme
bool            ui_theme_is_dark(void);
```

### 1.4 Migration steps

- Every file that currently has colour `#define`s (`launcher_app.c`, `settings_app.c`, `scene_avatar.c`) must replace them with calls to `ui_theme_get()->field`.
- The launcher's existing `C_CARD`, `C_ACCENT`, `C_HINT` etc. have no direct Xiaozhi equivalent. Map them as follows:
  - `C_BG` → `theme->bg`
  - `C_TEXT` / `C_TIME` → `theme->text`
  - `C_TEXT_DIM` / `C_HINT` → `theme->system_text`
  - `C_CARD` → derive from `theme->chat_bg` (slightly lighter for cards)
  - `C_ACCENT` → keep `0x18DFF2` as a project-specific accent, or add an `accent` field to the theme struct
  - `C_HAIRLINE` → `theme->border`

---

## 2. Font Migration — Montserrat → PuHuiTi + Font Awesome

### 2.1 Why

Xiaozhi uses two custom fonts:

- **`font_puhui`** — Alibaba PuHuiTi (supports full CJK + Latin), used for all text. Xiaozhi ships several sizes: `font_puhui_basic_20_4` (default text), others as needed.
- **`font_awesome`** — FontAwesome subset (icons: WiFi, battery, volume, mute, etc.), used for status bar and emoji. Sizes: `font_awesome_20_4` (status bar), `font_awesome_30_4` (large icons).

Our project currently uses LVGL built-in `lv_font_montserrat_*` which is Latin-only — no Chinese character support.

### 2.2 Font file generation

Use **LVGL's font converter** (`lv_font_conv`) to generate C source files:

```bash
# PuHuiTi text font — include ASCII + common CJK
lv_font_conv \
  --font AlibabaPuHuiTi-Regular.ttf \
  --bpp 4 --size 20 \
  --range 0x20-0x7F \
  --symbols "就绪请说话思考中播放连接错误休眠已未设置亮度超时分秒开关版本固件地址状态" \
  --format lvgl \
  -o font_puhui_20_4.c

# Also generate size 16 (labels) and 24 (headings)
# Same --range and --symbols, different --size

# FontAwesome icon font
lv_font_conv \
  --font FontAwesome5-Solid.otf \
  --bpp 4 --size 20 \
  --range 0xF001-0xF8FF \
  --format lvgl \
  -o font_awesome_20_4.c
```

**Alternatively**, copy Xiaozhi's pre-built font `.c` files directly from `xiaozhi-esp32/main/display/lvgl_display/fonts/`. They already have the right glyph sets.

### 2.3 Where to place font files

```
main/assets/fonts/
  font_puhui_16_4.c
  font_puhui_20_4.c
  font_puhui_24_4.c
  font_awesome_20_4.c
  font_awesome_30_4.c
```

Create `main/assets/fonts/ui_fonts.h`:

```c
#pragma once
#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_24_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

#define UI_FONT_TEXT_SM    &font_puhui_16_4
#define UI_FONT_TEXT       &font_puhui_20_4
#define UI_FONT_TEXT_LG    &font_puhui_24_4
#define UI_FONT_ICON       &font_awesome_20_4
#define UI_FONT_ICON_LG    &font_awesome_30_4
```

### 2.4 Font replacement map

| Current usage                    | Current font               | New font          |
|----------------------------------|----------------------------|-------------------|
| Launcher clock                   | `lv_font_montserrat_18`    | `UI_FONT_TEXT`    |
| Launcher tile labels             | `lv_font_montserrat_16`    | `UI_FONT_TEXT_SM` |
| Launcher hint                    | `lv_font_montserrat_14`    | `UI_FONT_TEXT_SM` |
| Launcher empty-slot plus         | `lv_font_montserrat_24`    | `UI_FONT_TEXT_LG` |
| Launcher WiFi icon (`LV_SYMBOL_WIFI`) | `lv_font_montserrat_16` | `UI_FONT_ICON` (use FA glyph `\xEF\x87\xAB` = `0xF1EB`) |
| Settings title                   | `lv_font_montserrat_24`    | `UI_FONT_TEXT_LG` |
| Settings row key/value           | `lv_font_montserrat_18`    | `UI_FONT_TEXT`    |
| Settings dropdown label          | `lv_font_montserrat_20`    | `UI_FONT_TEXT`    |
| Avatar status/message text       | LVGL default               | `UI_FONT_TEXT`    |
| Chat messages (future bubbles)   | —                          | `UI_FONT_TEXT`    |
| Status bar icons (battery, mute) | —                          | `UI_FONT_ICON`   |

### 2.5 Icon glyph mapping (FontAwesome 5 codepoints)

Replace LVGL built-in symbols with FontAwesome glyphs:

| Icon     | FA5 codepoint | UTF-8 bytes          | Usage             |
|----------|---------------|----------------------|-------------------|
| WiFi     | `0xF1EB`      | `\xEF\x87\xAB`      | Status bar        |
| Battery full | `0xF240`  | `\xEF\x89\x80`      | Status bar        |
| Battery half | `0xF242`  | `\xEF\x89\x82`      | Status bar        |
| Battery low  | `0xF243`  | `\xEF\x89\x83`      | Status bar        |
| Volume up    | `0xF028`  | `\xEF\x80\xA8`      | Status bar        |
| Volume mute  | `0xF6A9`  | `\xEF\x9A\xA9`      | Status bar        |
| Gear     | `0xF013`      | `\xEF\x80\x93`      | Settings tile     |
| Comment  | `0xF075`      | `\xEF\x81\xB5`      | Chat tile         |
| Plus     | `0xF067`      | `\xEF\x81\xA7`      | Empty slot        |

### 2.6 CMake integration

Add the font `.c` files to the `main` component's `SRCS` list in `main/CMakeLists.txt`, or create a dedicated `fonts` component with its own `CMakeLists.txt`.

---

## 3. Status Bar — Xiaozhi-Style Layered Bar

### 3.1 Current state

The launcher has an inline status bar (clock left, WiFi icon + WS dot right) at the top 38px. Other apps have no status bar.

### 3.2 Target (Xiaozhi pattern)

Xiaozhi uses a **shared two-layer status bar** across all screens:

- **`top_bar`** (height ~36px, 50% opacity background): network icon (left), mute icon + battery icon (right). Uses `font_awesome_20_4`.
- **`status_bar`** (transparent overlay on top of content): centered notification text that shows transient messages like "Listening…", "Connecting…".

### 3.3 Implementation plan

Create `main/framework/display/ui_status_bar.h` / `.c`:

```c
esp_err_t ui_status_bar_create(lv_obj_t *parent);   // attach to any screen
void      ui_status_bar_set_wifi(bool connected);
void      ui_status_bar_set_ws(bool connected);
void      ui_status_bar_set_battery(int percent);
void      ui_status_bar_set_mute(bool muted);
void      ui_status_bar_set_notification(const char *text); // centered overlay
void      ui_status_bar_clear_notification(void);
```

- Each app screen calls `ui_status_bar_create(screen)` to get a consistent bar.
- The bar uses `UI_FONT_ICON` for WiFi/battery/mute glyphs and `UI_FONT_TEXT_SM` for notification text.
- Colours come from `ui_theme_get()`.
- This replaces the custom status-bar code in `launcher_app.c` (the `s_time_lbl`, `s_wifi_icon`, `s_ws_dot` widgets).

---

## 4. Chat Bubble UI — WeChat-Style Messages

### 4.1 Current state

`chat_app.c` uses `avatar_show_message()` which just sets a single centered label. No chat history, no bubbles.

### 4.2 Target (Xiaozhi WeChat style)

Xiaozhi's `SetChatMessage()` implementation:

- Scrollable content area with `chat_background_color`
- Message roles: `user` (right-aligned, green bubble), `assistant` (left-aligned, `assistant_bubble_color`), `system` (centered, `system_bubble_color`)
- Bubble style: `bg_opa = 70%`, `radius = 8`, `pad_all = 8`, `max_width = 80%` of screen width
- Max 20 messages; oldest removed when exceeded
- Auto-scroll to bottom on new message
- Text wrapping enabled, font = `UI_FONT_TEXT`

### 4.3 Implementation plan

Create `main/framework/display/scene_chat.h` / `scene_chat.c`:

```c
typedef enum { CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_SYSTEM } chat_role_t;

esp_err_t scene_chat_init(void);
lv_obj_t *scene_chat_get_screen(void);
void      scene_chat_add_message(chat_role_t role, const char *text);
void      scene_chat_update_last_message(const char *text);  // for streaming
void      scene_chat_clear(void);
```

Layout structure (on the chat screen):

```
┌─────────────────────────────┐
│ [status_bar via ui_status_bar_create] │  h=36
│─────────────────────────────│
│                             │
│  ┌──────────────┐           │  ← assistant bubble (left)
│  │ Hello!       │           │
│  └──────────────┘           │
│           ┌──────────────┐  │  ← user bubble (right, green)
│           │ Hi there     │  │
│           └──────────────┘  │
│                             │
│  scrollable content area    │
│─────────────────────────────│
│ 🎤 Hold to talk            │  h=50 (optional bottom bar)
└─────────────────────────────┘
```

- The chat screen replaces `scene_avatar` as the active display for `chat_app.c`.
- `scene_avatar` can remain as an alternative display mode (e.g., for idle/emotion display), togglable.
- `chat_app.c` calls `scene_chat_add_message()` instead of `avatar_show_message()`.
- Streaming: `chat_app.c` calls `scene_chat_add_message(CHAT_ROLE_ASSISTANT, "")` when a response starts, then `scene_chat_update_last_message()` as chunks arrive.

### 4.4 Bubble rendering details (from Xiaozhi)

```c
// For each message:
lv_obj_t *bubble = lv_obj_create(content_area);
lv_obj_set_style_bg_opa(bubble, LV_OPA_70, 0);
lv_obj_set_style_radius(bubble, 8, 0);
lv_obj_set_style_pad_all(bubble, 8, 0);
lv_obj_set_style_border_width(bubble, 0, 0);
lv_obj_set_width(bubble, LV_PCT(80));  // max 80% width
lv_obj_set_style_bg_color(bubble, theme->user_bubble, 0);   // or assistant_bubble

// Alignment
if (role == USER)     lv_obj_set_style_align(bubble, LV_ALIGN_RIGHT_MID, 0); // flex right
if (role == ASSISTANT) lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);  // flex left
if (role == SYSTEM)   lv_obj_set_style_align(bubble, LV_ALIGN_CENTER, 0);

// Text label inside bubble
lv_obj_t *label = lv_label_create(bubble);
lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
lv_obj_set_width(label, LV_PCT(100));
lv_label_set_text(label, message_text);
lv_obj_set_style_text_font(label, UI_FONT_TEXT, 0);
lv_obj_set_style_text_color(label, theme->text, 0);
```

---

## 5. Per-App File Changes

### 5.1 `launcher_app.c`

| Area              | Change                                                                 |
|-------------------|------------------------------------------------------------------------|
| Colours           | Delete all `C_*` defines. Use `ui_theme_get()` for bg, text, border.   |
| Card bg           | Use `theme->chat_bg` or a derived shade. Keep accent `0x18DFF2` if desired (add to theme struct). |
| Fonts             | Replace all `lv_font_montserrat_*` → `UI_FONT_TEXT`, `UI_FONT_TEXT_SM`, `UI_FONT_TEXT_LG`. |
| WiFi icon         | Replace `LV_SYMBOL_WIFI` + Montserrat → FA glyph `\xEF\x87\xAB` + `UI_FONT_ICON`. |
| Plus icon         | Replace `LV_SYMBOL_PLUS` → FA glyph `\xEF\x81\xA7` + `UI_FONT_ICON`. |
| Status bar        | Remove inline clock/wifi/ws-dot code. Call `ui_status_bar_create(s_screen)` instead. |
| Tile icons        | Keep the 80×80 PNGs for now. Future: consider FA glyph tiles.          |

### 5.2 `settings_app.c`

| Area              | Change                                                                 |
|-------------------|------------------------------------------------------------------------|
| Colours           | Delete all `S_*` defines. Use `ui_theme_get()` throughout.             |
| Fonts             | Replace all `lv_font_montserrat_*` → `UI_FONT_TEXT`, `UI_FONT_TEXT_SM`, `UI_FONT_TEXT_LG`. |
| Status bar        | Add `ui_status_bar_create(s_screen)` at top of screen.                 |
| Back arrow        | Replace `LV_SYMBOL_LEFT` → FA glyph `\xEF\x81\x93` (chevron-left) + `UI_FONT_ICON`, or keep `LV_SYMBOL_LEFT` if FA subset doesn't include it. |
| Theme toggle      | Consider adding a "Theme" row with dark/light toggle that calls `ui_theme_set_dark()`. |

### 5.3 `scene_avatar.c`

| Area              | Change                                                                 |
|-------------------|------------------------------------------------------------------------|
| Background        | Replace `0x000000` → `theme->bg`.                                      |
| Text colours      | Replace `0xFFFFFF` → `theme->text`, `0x888888` → `theme->system_text`. |
| Status label      | Connected `0x00FF00` / disconnected `0xFF0000` → `theme->user_bubble` / `theme->low_battery`. |
| Fonts             | Use `UI_FONT_TEXT` for message and status labels.                      |
| Status bar        | Add `ui_status_bar_create(s_screen)`.                                  |

### 5.4 `chat_app.c`

| Area              | Change                                                                 |
|-------------------|------------------------------------------------------------------------|
| Display           | Switch from `scene_avatar` screen to `scene_chat` screen.             |
| Message display   | Replace `avatar_show_message()` → `scene_chat_add_message()`.         |
| Streaming         | Use `scene_chat_update_last_message()` for token-by-token updates.    |
| State display     | Use `ui_status_bar_set_notification()` for "Listening…", "Thinking…". |

---

## 6. Asset Updates

### 6.1 Tile icons (`img_assets.h`)

Keep `img_chat_icon` and `img_settings_icon` (80×80 PNGs) for now. They work fine as app tile icons.

Optionally, add new icons as more apps are added. Declare them in `img_assets.h`:

```c
LV_IMG_DECLARE(img_chat_icon);
LV_IMG_DECLARE(img_settings_icon);
// future:
// LV_IMG_DECLARE(img_weather_icon);
```

### 6.2 Emoji / emotional expressions

Xiaozhi uses a combination of:
- FontAwesome glyphs for simple emoji (rendered as text)
- GIF images from a `twemoji_64` collection for richer expressions, displayed in a floating layer

For Phase 1, skip GIF emoji. Use FontAwesome glyphs for basic emotional indicators in the avatar/chat. GIF emoji can be added in a later phase.

---

## 7. New Files Summary

| File                                          | Purpose                                   |
|-----------------------------------------------|-------------------------------------------|
| `main/framework/display/ui_theme.h`           | Theme struct + API declarations           |
| `main/framework/display/ui_theme.c`           | Dark/light theme instances + getter       |
| `main/framework/display/ui_status_bar.h`      | Shared status bar API                     |
| `main/framework/display/ui_status_bar.c`      | Status bar implementation (FA icons)      |
| `main/framework/display/scene_chat.h`         | Chat bubble scene API                     |
| `main/framework/display/scene_chat.c`         | WeChat-style bubble rendering             |
| `main/assets/fonts/ui_fonts.h`                | Font declarations + convenience macros    |
| `main/assets/fonts/font_puhui_16_4.c`         | PuHuiTi 16px 4bpp font data              |
| `main/assets/fonts/font_puhui_20_4.c`         | PuHuiTi 20px 4bpp font data              |
| `main/assets/fonts/font_puhui_24_4.c`         | PuHuiTi 24px 4bpp font data              |
| `main/assets/fonts/font_awesome_20_4.c`       | FontAwesome 20px 4bpp icon font           |
| `main/assets/fonts/font_awesome_30_4.c`       | FontAwesome 30px 4bpp icon font           |

## 8. Modified Files Summary

| File                                          | Changes                                    |
|-----------------------------------------------|--------------------------------------------|
| `main/apps/launcher/launcher_app.c`           | Theme colours, new fonts, shared status bar |
| `main/apps/settings/settings_app.c`           | Theme colours, new fonts, shared status bar |
| `main/apps/chat/chat_app.c`                   | Switch to scene_chat, bubble messages       |
| `main/framework/display/scene_avatar.c`       | Theme colours, new fonts, shared status bar |
| `main/assets/img_assets.h`                    | No change needed (keep PNGs)               |
| `main/CMakeLists.txt`                         | Add new `.c` source files                  |

---

## 9. Suggested Implementation Order

1. **Fonts first** — Generate/copy font `.c` files, create `ui_fonts.h`, update CMakeLists. Verify build compiles.
2. **Theme system** — Create `ui_theme.h/.c`. Wire up dark theme colours. No visual change yet.
3. **Status bar** — Create `ui_status_bar.h/.c` using new fonts + theme colours.
4. **Update launcher** — Remove old colour defines and inline status bar. Use theme + shared status bar + new fonts.
5. **Update settings** — Same treatment as launcher.
6. **Update scene_avatar** — Theme colours + fonts + status bar.
7. **Chat bubbles** — Create `scene_chat.h/.c` with WeChat-style bubbles.
8. **Wire chat_app** — Switch from scene_avatar to scene_chat for message display.
9. **Test & tune** — Verify all screens look consistent, adjust padding/sizing for 368×448.

---

## 10. Key Design Decisions for Claude Code

- **Keep the dark theme as default** — AMOLED panels look best with dark backgrounds and the Xiaozhi dark theme is well-suited.
- **Copy Xiaozhi's font files directly** if available — generating with `lv_font_conv` requires the correct TTF source and glyph ranges. Xiaozhi's pre-built fonts already have the right CJK subset.
- **The `--symbols` flag for `lv_font_conv`** — if generating fonts, include all Chinese strings used in the UI (status messages, settings labels, etc.) plus ASCII range. This keeps file size manageable.
- **Keep the launcher's accent colour** (`0x18DFF2`) — it's distinctive and works well on dark backgrounds. Add it as `accent` in the theme struct.
- **scene_avatar stays** — it becomes an alternative display mode (idle animation). The chat screen is the primary UI during conversations.
- **Max 20 messages** in the chat view (matching Xiaozhi) — prevents memory bloat on ESP32.
