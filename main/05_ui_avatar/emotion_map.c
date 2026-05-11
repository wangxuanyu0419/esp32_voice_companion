/**
 * 情绪表情映射表 - 实现
 */

#include "emotion_map.h"

// 情绪配置文件
// emotion_id: 服务器发来的完整 ID
// family: 情绪家族
// level: 强度等级
// label: 中文标签
// file_path: SPIFFS 中的图片路径
// duration_ms: 持续时间（毫秒）

const emotion_config_t g_emotion_configs[EMOTION_COUNT] = {
    // AVATAR_IDLE - 待机状态（特殊，无 emotion_id）
    [AVATAR_IDLE] = {
        .emotion_id = "idle",
        .family = "idle",
        .level = 0,
        .label = "待机",
        .file_path = "/spiffs/avatar/avatar_idle.png",
        .duration_ms = 0
    },
    
    // AVATAR_HAPPY - 开心
    [AVATAR_HAPPY] = {
        .emotion_id = "happy_l2",
        .family = "happy",
        .level = 2,
        .label = "开心",
        .file_path = "/spiffs/avatar/emotion_happy.png",
        .duration_ms = 2500
    },
    
    // AVATAR_SAD - 难过
    [AVATAR_SAD] = {
        .emotion_id = "sad_l2",
        .family = "sad",
        .level = 2,
        .label = "难过",
        .file_path = "/spiffs/avatar/emotion_sad.png",
        .duration_ms = 2500
    },
    
    // AVATAR_ANGRY - 生气
    [AVATAR_ANGRY] = {
        .emotion_id = "angry_l2",
        .family = "angry",
        .level = 2,
        .label = "生气",
        .file_path = "/spiffs/avatar/emotion_angry.png",
        .duration_ms = 2500
    },
    
    // AVATAR_FEAR - 害怕
    [AVATAR_FEAR] = {
        .emotion_id = "fear_l2",
        .family = "fear",
        .level = 2,
        .label = "害怕",
        .file_path = "/spiffs/avatar/emotion_fear.png",
        .duration_ms = 2500
    },
    
    // AVATAR_DISGUST - 厌恶
    [AVATAR_DISGUST] = {
        .emotion_id = "disgust_l2",
        .family = "disgust",
        .level = 2,
        .label = "厌恶",
        .file_path = "/spiffs/avatar/emotion_disgust.png",
        .duration_ms = 2500
    },
    
    // AVATAR_SURPRISED - 惊讶
    [AVATAR_SURPRISED] = {
        .emotion_id = "surprised_l2",
        .family = "surprised",
        .level = 2,
        .label = "惊讶",
        .file_path = "/spiffs/avatar/emotion_surprised.png",
        .duration_ms = 2500
    },
    
    // AVATAR_SPEAKING - 说话中（特殊，无 emotion_id）
    [AVATAR_SPEAKING] = {
        .emotion_id = "speaking",
        .family = "speaking",
        .level = 0,
        .label = "说话",
        .file_path = "/spiffs/avatar/avatar_speaking.png",
        .duration_ms = 0
    }
};

// 从 emotion_id 提取家族名
static const char* extract_family(const char* emotion_id)
{
    if (emotion_id == NULL) return "";
    
    // 找到 "_l" 的位置
    const char* underscore = strchr(emotion_id, '_');
    if (underscore == NULL) return emotion_id;
    
    // 提取家族名（从开始到 _）
    size_t family_len = underscore - emotion_id;
    static char family[32] = {0};
    if (family_len < sizeof(family)) {
        memcpy(family, emotion_id, family_len);
        family[family_len] = '\0';
        return family;
    }
    return emotion_id;
}

// 根据 emotion_id 查找对应的 avatar_emotion_t
avatar_emotion_t emotion_id_to_avatar(const char* emotion_id)
{
    if (emotion_id == NULL || strlen(emotion_id) == 0) {
        return AVATAR_IDLE;
    }
    
    // 特殊情况
    if (strcmp(emotion_id, "speaking") == 0) {
        return AVATAR_SPEAKING;
    }
    
    // 提取家族名
    const char* family = extract_family(emotion_id);
    
    // 匹配家族
    if (strcmp(family, "happy") == 0) return AVATAR_HAPPY;
    if (strcmp(family, "sad") == 0) return AVATAR_SAD;
    if (strcmp(family, "angry") == 0) return AVATAR_ANGRY;
    if (strcmp(family, "fear") == 0) return AVATAR_FEAR;
    if (strcmp(family, "disgust") == 0) return AVATAR_DISGUST;
    if (strcmp(family, "surprised") == 0) return AVATAR_SURPRISED;
    
    // 默认返回 idle
    return AVATAR_IDLE;
}

// 根据 avatar_emotion_t 获取文件路径
const char* avatar_get_image_path(avatar_emotion_t emotion)
{
    if (emotion >= AVATAR_COUNT) {
        emotion = AVATAR_IDLE;
    }
    return g_emotion_configs[emotion].file_path;
}

// 获取情绪持续时间（根据等级）
int emotion_get_duration(int level)
{
    switch (level) {
        case 1: return 1500;   // 轻微
        case 2: return 2500;  // 中等
        case 3: return 3800;  // 强烈
        default: return 2500;
    }
}