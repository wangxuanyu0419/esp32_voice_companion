#include "emotion_map.h"
#include <string.h>
#include <stdlib.h>

const emotion_config_t g_emotion_configs[AVATAR_COUNT] = {
    [AVATAR_IDLE]      = {"idle",         "idle",      0, "待机", "/spiffs/avatar/avatar_idle.png",     0},
    [AVATAR_HAPPY]     = {"happy_l2",     "happy",     2, "开心", "/spiffs/avatar/emotion_happy.png",   2500},
    [AVATAR_SAD]       = {"sad_l2",       "sad",       2, "难过", "/spiffs/avatar/emotion_sad.png",     2500},
    [AVATAR_ANGRY]     = {"angry_l2",     "angry",     2, "生气", "/spiffs/avatar/emotion_angry.png",   2500},
    [AVATAR_FEAR]      = {"fear_l2",      "fear",      2, "害怕", "/spiffs/avatar/emotion_fear.png",    2500},
    [AVATAR_DISGUST]   = {"disgust_l2",   "disgust",   2, "厌恶", "/spiffs/avatar/emotion_disgust.png", 2500},
    [AVATAR_SURPRISED] = {"surprised_l2", "surprised", 2, "惊讶", "/spiffs/avatar/emotion_surprised.png", 2500},
    [AVATAR_SPEAKING]  = {"speaking",     "speaking",  0, "说话", "/spiffs/avatar/avatar_speaking.png", 0},
};

static const char *extract_family(const char *emotion_id)
{
    if (!emotion_id) return "";
    const char *sep = strchr(emotion_id, '_');
    if (!sep) return emotion_id;
    size_t len = (size_t)(sep - emotion_id);
    static char family[32];
    if (len < sizeof(family)) {
        memcpy(family, emotion_id, len);
        family[len] = '\0';
        return family;
    }
    return emotion_id;
}

avatar_emotion_t emotion_id_to_avatar(const char *emotion_id)
{
    if (!emotion_id || !*emotion_id) return AVATAR_IDLE;
    if (strcmp(emotion_id, "speaking") == 0) return AVATAR_SPEAKING;
    const char *f = extract_family(emotion_id);
    if (strcmp(f, "happy")     == 0) return AVATAR_HAPPY;
    if (strcmp(f, "sad")       == 0) return AVATAR_SAD;
    if (strcmp(f, "angry")     == 0) return AVATAR_ANGRY;
    if (strcmp(f, "fear")      == 0) return AVATAR_FEAR;
    if (strcmp(f, "disgust")   == 0) return AVATAR_DISGUST;
    if (strcmp(f, "surprised") == 0) return AVATAR_SURPRISED;
    return AVATAR_IDLE;
}

const char *avatar_get_image_path(avatar_emotion_t emotion)
{
    if (emotion >= AVATAR_COUNT) emotion = AVATAR_IDLE;
    return g_emotion_configs[emotion].file_path;
}

int emotion_get_duration(int level)
{
    switch (level) {
        case 1: return 1500;
        case 2: return 2500;
        case 3: return 3800;
        default: return 2500;
    }
}
