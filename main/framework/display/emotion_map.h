#ifndef EMOTION_MAP_H
#define EMOTION_MAP_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EMOTION_HAPPY = 0,
    EMOTION_SAD,
    EMOTION_ANGRY,
    EMOTION_FEAR,
    EMOTION_DISGUST,
    EMOTION_SURPRISED,
    EMOTION_COUNT,
} emotion_family_t;

typedef enum {
    EMOTION_LEVEL_1 = 1,
    EMOTION_LEVEL_2 = 2,
    EMOTION_LEVEL_3 = 3,
} emotion_level_t;

typedef enum {
    AVATAR_IDLE = 0,
    AVATAR_HAPPY,
    AVATAR_SAD,
    AVATAR_ANGRY,
    AVATAR_FEAR,
    AVATAR_DISGUST,
    AVATAR_SURPRISED,
    AVATAR_SPEAKING,
    AVATAR_COUNT,
} avatar_emotion_t;

typedef struct {
    const char *emotion_id;
    const char *family;
    int         level;
    const char *label;
    const char *file_path;
    int         duration_ms;
} emotion_config_t;

extern const emotion_config_t g_emotion_configs[AVATAR_COUNT];

avatar_emotion_t emotion_id_to_avatar(const char *emotion_id);
const char      *avatar_get_image_path(avatar_emotion_t emotion);
int              emotion_get_duration(int level);

#endif /* EMOTION_MAP_H */
