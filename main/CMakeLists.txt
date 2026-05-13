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
    "board/peripherals/media_storage.c"

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
    "framework/audio/tts_player.c"
    "framework/display/scene_avatar.c"
    "framework/display/font_manager.c"
    "framework/display/emotion_map.c"
    "framework/display/ui_theme.c"
    "framework/display/ui_status_bar.c"
    "framework/display/scene_chat.c"

    # Layer 4 — Apps
    "apps/app_registry.c"
    "apps/chat/chat_app.c"
    "apps/launcher/launcher_app.c"
    "apps/settings/settings_app.c"
    "apps/color_test/color_test.c"
    "apps/media_player/media_player_app.c"

    # Assets — pre-rendered LVGL image C arrays
    "assets/img_chat.c"
    "assets/img_settings.c"
    "assets/img_state_booting.c"
    "assets/img_state_idle.c"
    "assets/img_state_listening.c"
    "assets/img_state_thinking.c"
    "assets/img_state_speaking.c"
    "assets/img_state_error.c"
    "assets/img_state_wifiok.c"
    "assets/img_state_wififail.c"

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
    "apps/color_test"
    "apps/media_player"
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
        esp_http_client
        spiffs
        fatfs
        sdmmc
        esp_driver_sdmmc
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

# Build a SPIFFS image from spiffs_data/ and flash it alongside the app.
# The label "spiffs" matches the partition table entry.
spiffs_create_partition_image(spiffs ../spiffs_data FLASH_IN_PROJECT)
