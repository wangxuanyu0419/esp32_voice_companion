/*
 * scene_chat.h — WeChat-style chat bubble screen.
 *
 * Shows up to MAX_CHAT_MESSAGES messages in a scrollable list.
 * Each message is a rounded bubble: user (right/green), assistant (left/dark),
 * or system (centred/dim).
 *
 * Usage:
 *   scene_chat_init();
 *   lv_scr_load(scene_chat_get_screen());
 *   scene_chat_add_message(CHAT_ROLE_ASSISTANT, "Hello!");
 *   scene_chat_update_last_message("Hello, how can I help?");  // streaming
 */
#pragma once
#include "lvgl.h"
#include "esp_err.h"

typedef enum {
    CHAT_ROLE_USER,
    CHAT_ROLE_ASSISTANT,
    CHAT_ROLE_SYSTEM,
} chat_role_t;

#define MAX_CHAT_MESSAGES 20

esp_err_t  scene_chat_init(void);
lv_obj_t  *scene_chat_get_screen(void);
void       scene_chat_add_message(chat_role_t role, const char *text);
void       scene_chat_update_last_message(const char *text);   /* for streaming */
void       scene_chat_clear(void);
void       scene_chat_set_state_text(const char *text);  /* notification overlay */
