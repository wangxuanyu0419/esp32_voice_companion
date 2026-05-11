/**
 * 头像 UI 管理器 - 实现
 * 
 * 功能：
 * - 屏幕初始化 (LVGL + SH8601)
 * - Q 版头像显示（8种情绪状态）
 * - 触摸检测（屏幕中心区域）
 * - 情绪切换动画
 */

#include "avatar_ui.h"
#include "emotion_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl/lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "AVATAR_UI";

static bool s_initialized = false;
static avatar_emotion_t s_current_emotion = AVATAR_IDLE;

// LVGL 组件
static lv_obj_t* s_screen = NULL;
static lv_obj_t* s_avatar_img = NULL;
static lv_obj_t* s_message_label = NULL;
static lv_obj_t* s_status_label = NULL;

// 定时器（用于情绪恢复）
static esp_timer_handle_t s_emotion_timer = NULL;

// 临时显示消息
static const int MESSAGE_DISPLAY_MS = 3000;

// 触摸区域（屏幕中心，avatar 所在位置）
// 屏幕 368x448，中心约 184x224
#define TOUCH_X_CENTER  184
#define TOUCH_Y_CENTER  224
#define TOUCH_RADIUS    80  // 半径

/**
 * 加载并显示头像图片
 */
static esp_err_t load_avatar_image(avatar_emotion_t emotion)
{
    const char* path = avatar_get_image_path(emotion);
    
    ESP_LOGI(TAG, "Loading avatar: emotion=%d, path=%s", emotion, path);
    
    // 尝试从 SPIFFS 加载图片
    // 注意：实际需要先挂载 SPIFFS 并确保文件存在
    // 这里使用占位符（实际应使用 lv_img_set_src）
    
    // 示例代码（实际需要文件系统支持）：
    // if (fs_exists(path)) {
    //     lv_img_set_src(s_avatar_img, path);
    // } else {
    //     ESP_LOGW(TAG, "Avatar file not found: %s", path);
    // }
    
    return ESP_OK;
}

/**
 * 触摸回调
 */
static void touch_callback(lv_event_t* e)
{
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == NULL) return;
    
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    
    // 检查是否在中心区域
    int dx = point.x - TOUCH_X_CENTER;
    int dy = point.y - TOUCH_Y_CENTER;
    int dist_sq = dx * dx + dy * dy;
    
    if (dist_sq <= TOUCH_RADIUS * TOUCH_RADIUS) {
        ESP_LOGI(TAG, "Avatar touch detected at (%d, %d)", point.x, point.y);
        // 触发录音（通过事件队列，实际在 app_main.c 处理）
    }
}

/**
 * 情绪恢复定时器回调
 */
static void emotion_timer_callback(void* arg)
{
    ESP_LOGI(TAG, "Emotion timer expired, returning to idle");
    avatar_set_emotion(AVATAR_IDLE);
}

/**
 * LVGL 显示刷新回调（需要适配实际屏幕驱动）
 */
static void display_flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, 
                                   lv_color_t* color_p)
{
    // TODO: 实现 SH8601 屏幕刷屏
    // 这里需要调用 display_draw_pixels() 将 color_p 写入屏幕
    
    lv_disp_flush_ready(drv);
}

/**
 * 触摸读取回调（需要适配 FT3168）
 */
static void touch_read_callback(lv_indev_drv_t* drv, lv_indev_data_t* data)
{
    // TODO: 实现 FT3168 触摸读取
    // 需要读取 I2C 触摸坐标并填充 data->point.x/y
}

esp_err_t avatar_init(void)
{
    if (s_initialized) return ESP_OK;
    
    ESP_LOGI(TAG, "Avatar UI initializing...");
    
    // 1. 初始化 LVGL
    lv_init();
    
    // 2. 初始化显示驱动（需要从 waveshare 示例移植）
    // lv_disp_t* disp = lv_display_create(368, 448);
    // lv_disp_set_flush_cb(disp, display_flush_callback);
    
    // 3. 初始化触摸驱动（需要从 waveshare 示例移植）
    // lv_indev_t* indev = lv_indev_create();
    // lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    // lv_indev_set_read_cb(indev, touch_read_callback);
    
    // 4. 创建屏幕
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    
    // 5. 创建头像图片
    s_avatar_img = lv_img_create(s_screen);
    lv_obj_center(s_avatar_img);
    lv_img_set_zoom(s_avatar_img, 256);  // 原始大小 1x
    
    // 加载默认头像
    load_avatar_image(AVATAR_IDLE);
    
    // 6. 创建消息标签
    s_message_label = lv_label_create(s_screen);
    lv_label_set_text(s_message_label, "");
    lv_obj_align(s_message_label, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_text_color(s_message_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_message_label, &lv_font_montserrat_16, 0);
    
    // 7. 创建状态标签
    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, "初始化中...");
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    
    // 8. 设置触摸回调
    lv_obj_add_event_cb(s_screen, touch_callback, LV_EVENT_CLICKED, NULL);
    
    // 9. 创建情绪恢复定时器
    esp_timer_create_args_t timer_args = {
        .callback = emotion_timer_callback,
        .name = "emotion_timer",
    };
    esp_timer_create(&timer_args, &s_emotion_timer);
    
    // 10. 创建 LVGL 刷新任务
    // xTaskCreatePinnedToCore(lvgl_task, "lvgl", 4096, NULL, 3, NULL, 1);
    
    s_initialized = true;
    ESP_LOGI(TAG, "Avatar UI initialized (placeholder mode)");
    
    return ESP_OK;
}

esp_err_t avatar_set_state(app_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    const char* status_text;
    avatar_emotion_t emotion;
    
    switch (state) {
        case APP_STATE_IDLE:
            status_text = "就绪";
            emotion = AVATAR_IDLE;
            break;
        case APP_STATE_LISTENING:
            status_text = "请说话...";
            emotion = AVATAR_SPEAKING;  // 录音时显示说话中
            break;
        case APP_STATE_THINKING:
            status_text = "思考中...";
            emotion = AVATAR_SURPRISED;  // 思考用惊讶表情
            break;
        case APP_STATE_SPEAKING:
            status_text = "播放中...";
            emotion = AVATAR_SPEAKING;
            break;
        case APP_STATE_ERROR:
            status_text = "连接错误";
            emotion = AVATAR_SAD;
            break;
        case APP_STATE_SLEEP:
            status_text = "休眠";
            emotion = AVATAR_IDLE;
            // 息屏
            // display_off();
            break;
        default:
            status_text = "";
            emotion = AVATAR_IDLE;
            break;
    }
    
    lv_label_set_text(s_status_label, status_text);
    load_avatar_image(emotion);
    s_current_emotion = emotion;
    
    return ESP_OK;
}

esp_err_t avatar_set_emotion(avatar_emotion_t emotion)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "Setting emotion: %d (%s)", 
             emotion, g_emotion_configs[emotion].label);
    
    s_current_emotion = emotion;
    load_avatar_image(emotion);
    
    return ESP_OK;
}

esp_err_t avatar_set_emotion_with_duration(avatar_emotion_t emotion, int duration_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    // 设置情绪
    avatar_set_emotion(emotion);
    
    // 如果有持续时间，延迟后恢复 idle
    if (duration_ms > 0) {
        // 停止之前的定时器
        esp_timer_stop(s_emotion_timer);
        
        // 启动新的定时器
        esp_timer_start_once(s_emotion_timer, duration_ms * 1000);  // us
        ESP_LOGI(TAG, "Emotion timer set: %d ms", duration_ms);
    }
    
    return ESP_OK;
}

esp_err_t avatar_show_message(const char* msg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    lv_label_set_text(s_message_label, msg);
    
    // 延迟清除
    // 注意：这里简化处理，实际应该用定时器
    // 可以实现：vTaskDelay(pdMS_TO_TICKS(MESSAGE_DISPLAY_MS)); 
    // 然后清除消息
    
    return ESP_OK;
}

esp_err_t avatar_clear_message(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    lv_label_set_text(s_message_label, "");
    return ESP_OK;
}

esp_err_t avatar_set_connection_status(bool connected)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    if (connected) {
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x00FF00), 0);
        lv_label_set_text(s_status_label, "已连接");
    } else {
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(s_status_label, "未连接");
    }
    
    return ESP_OK;
}

// 获取当前情绪
avatar_emotion_t avatar_get_current_emotion(void)
{
    return s_current_emotion;
}