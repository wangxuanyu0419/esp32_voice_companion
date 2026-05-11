/**
 * 情绪表情映射表
 * 
 * 对应 ClawChat 服务器的 SOUL 18-Emoji 体系
 * emotionId 格式: <family>_l<level>  (例如 happy_l2)
 */

#ifndef EMOTION_MAP_H
#define EMOTION_MAP_H

#include <stdint.h>
#include <stdbool.h>

// 情绪家族（6种）
typedef enum {
    EMOTION_HAPPY = 0,
    EMOTION_SAD,
    EMOTION_ANGRY,
    EMOTION_FEAR,
    EMOTION_DISGUST,
    EMOTION_SURPRISED,
    EMOTION_COUNT  // 总数
} emotion_family_t;

// 情绪等级
typedef enum {
    EMOTION_LEVEL_1 = 1,  // 轻微
    EMOTION_LEVEL_2 = 2,  // 中等
    EMOTION_LEVEL_3 = 3,  // 强烈
} emotion_level_t;

// ESP32 头像状态（8种）
typedef enum {
    AVATAR_IDLE = 0,           // 待机状态
    AVATAR_HAPPY,              // 开心
    AVATAR_SAD,                // 难过
    AVATAR_ANGRY,              // 生气
    AVATAR_FEAR,               // 害怕
    AVATAR_DISGUST,            // 厌恶
    AVATAR_SURPRISED,          // 惊讶
    AVATAR_SPEAKING,           // 说话中
    AVATAR_COUNT
} avatar_emotion_t;

// 情绪配置
typedef struct {
    const char* emotion_id;     // 服务器端 emotion ID (如 "happy_l2")
    const char* family;         // 家族名 (如 "happy")
    int level;                  // 等级 1-3
    const char* label;          // 显示标签 (如 "开心")
    const char* file_path;      // 图片路径
    int duration_ms;            // 持续时间
} emotion_config_t;

// 全局情绪配置表
extern const emotion_config_t g_emotion_configs[EMOTION_COUNT];

// 根据 emotion_id 查找对应的 avatar_emotion_t
avatar_emotion_t emotion_id_to_avatar(const char* emotion_id);

// 根据 avatar_emotion_t 获取文件路径
const char* avatar_get_image_path(avatar_emotion_t emotion);

// 获取情绪持续时间（根据等级）
int emotion_get_duration(int level);

#endif // EMOTION_MAP_H