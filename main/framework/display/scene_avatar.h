#ifndef SCENE_AVATAR_H
#define SCENE_AVATAR_H

#include "app_state.h"
#include "emotion_map.h"
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t  avatar_init(void);
lv_obj_t  *avatar_get_screen(void);   /* returns the avatar lv_obj screen */
esp_err_t  avatar_set_state(app_state_t state);
esp_err_t avatar_set_emotion(avatar_emotion_t emotion);
esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t emotion, int duration_ms);
esp_err_t avatar_show_message(const char *msg);
esp_err_t avatar_clear_message(void);
esp_err_t avatar_set_connection_status(bool connected);
avatar_emotion_t avatar_get_current_emotion(void);

#endif /* SCENE_AVATAR_H */
