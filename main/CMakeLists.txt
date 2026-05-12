# main/CMakeLists.txt — Phase A restructure
# Pure C build; only includes files from the new 4-layer architecture.

set(SRCS
    # Entry point
    "main.c"

    # Layer 1 — Board HAL
    "board/board_waveshare_amoled18.c"
    "board/peripherals/button.c"
    "board/peripherals/led.c"
    "board/peripherals/display_driver.c"

    # Layer 2 — System services
    "system/wifi_manager.c"
    "system/config_store.c"
    "system/event_bus.c"

    # Layer 3 — Application framework
    "framework/app_state.c"
    "framework/application.c"
    "framework/sleep_manager.c"
    "framework/protocol/ws_protocol.c"
    "framework/audio/audio_pipeline.c"
    "framework/audio/vad.c"
    "framework/display/scene_avatar.c"
    "framework/display/emotion_map.c"
    "framework/display/ui_theme.c"
    "framework/display/ui_status_bar.c"
    "framework/display/scene_chat.c"

    # Layer 4 — Apps
    "apps/app_registry.c"
    "apps/chat/chat_app.c"
    "apps/launcher/launcher_app.c"
    "apps/settings/settings_app.c"

    # Assets — pre-rendered LVGL image C arrays
    "assets/img_chat.c"
    "assets/img_settings.c"

    # Assets — fonts (PuHui + FontAwesome subsets)
    "assets/fonts/font_puhui_16_4.c"
    "assets/fonts/font_puhui_20_4.c"
    "assets/fonts/font_puhui_24_4.c"
    "assets/fonts/font_awesome_20_4.c"
    "assets/fonts/font_awesome_30_4.c"
)

# Make every header findable by basename (no path prefix needed in #include)
# NOTE: "." is intentionally omitted to prevent old root-level headers
#       (app_state.h, app_main.c, etc.) shadowing the new framework ones.
set(INCLUDE_DIRS
    "board"
    "board/peripherals"
    "system"
    "framework"
    "framework/protocol"
    "framework/audio"
    "framework/display"
    "apps"
    "apps/chat"
    "apps/launcher"
    "apps/settings"
    "assets"
    "assets/fonts"
)

idf_component_register(
    SRCS            ${SRCS}
    INCLUDE_DIRS    ${INCLUDE_DIRS}
    PRIV_REQUIRES
        # IDF built-ins
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_system
        driver
        mbedtls
        freertos
        log
        lwip
        # Local components
        openclaw_bridge
        # Managed components (names as registered by idf_component_manager)
        espressif__esp-websocket-client
        lvgl__lvgl
        espressif__esp_lcd_sh8601
        espressif__esp_lcd_touch_ft5x06
        espressif__esp_io_expander_tca9554
        esp_lcd
)
