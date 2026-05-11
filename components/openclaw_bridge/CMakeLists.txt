idf_component_register(
    SRCS "openclaw_bridge.c"
         "openclaw_ws_client.c"
         "openclaw_display.cpp"
         "cJSON.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
             esp_timer
             esp_netif
)
