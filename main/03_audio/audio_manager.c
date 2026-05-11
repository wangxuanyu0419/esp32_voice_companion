/**
 * 音频管理器 - 实现（ES8311 + I2S）
 */

#include "audio_manager.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_dsp.h"
#include "ws_client.h"

static const char* TAG = "AUDIO_MANAGER";

// I2S 配置
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_IO      (GPIO_NUM_15)   // BCK
#define I2S_WS_IO       (GPIO_NUM_16)   // WS/LRCLK
#define I2S_DO_IO       (GPIO_NUM_17)   // Data Out (to ES8311)
#define I2S_DI_IO       (GPIO_NUM_18)   // Data In (from MIC)

#define DMA_BUFFER_SIZE     1024
#define DMA_BUFFER_COUNT    8

// ES8311 I2C 地址
#define ES8311_ADDR         0x18

// 音频状态
static bool s_capturing = false;
static bool s_playing = false;
static int s_volume = 80;
static bool s_initialized = false;

// 音频缓冲
static int16_t s_audio_buffer[AUDIO_BUFFER_SIZE];
static size_t s_buffer_pos = 0;

// 录音回调
static audio_capture_cb_t s_capture_cb = NULL;
static void* s_capture_cb_arg = NULL;

// Semaphore for thread safety
static SemaphoreHandle_t s_audio_mutex = NULL;

// DMA 缓冲区
static int16_t s_dma_buffer[DMA_BUFFER_SIZE];

// Phase-1 STT audio: chunk queue (40ms chunks pushed by capture task)
static QueueHandle_t s_chunk_queue = NULL;
static bool s_chunk_queue_initialized = false;

// Chunk accumulator: accumulate DMA buffers into 40ms chunks
static int16_t s_chunk_accum[AUDIO_CHUNK_SAMPLES];
static size_t s_chunk_accum_count = 0;

// Chunk-building helper: called from capture task with DMA buffer samples
static void chunk_accumulator_push(const int16_t* samples, size_t count)
{
    if (!s_chunk_queue_initialized || !s_capturing) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        s_chunk_accum[s_chunk_accum_count++] = samples[i];

        if (s_chunk_accum_count >= AUDIO_CHUNK_SAMPLES) {
            // Full 40ms chunk ready — push to queue (non-blocking)
            BaseType_t woken = pdFALSE;
            int16_t chunk[AUDIO_CHUNK_SAMPLES];
            memcpy(chunk, s_chunk_accum, sizeof(chunk));
            s_chunk_accum_count = 0;

            // Try to send without blocking; if queue full, drop oldest chunk
            if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
                // Queue full — drop this chunk and try to make room
                int16_t dummy;
                xQueueReceiveFromISR(s_chunk_queue, &dummy, &woken);
                xQueueSendFromISR(s_chunk_queue, chunk, &woken);
            }
        }
    }
}

// I2S 初始化
static esp_err_t i2s_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S...");
    
    // I2S 标准模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .duty_cycle = I2S_DUTY_CYCLE_16,
            .zero_bar = I2S_STD_MSB_SHIFT_MODE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
        },
        .slot_cfg = {
            .mode = I2S_SL_MODE_STANDALONE,
            .slot_mask = I2S_STD_SLOT_MONO,
            .slot_width = I2S_STD_SLOT_WIDTH_16BIT,
            .ws_width = I2S_STD_SLOT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
        },
        .gpio_cfg = {
            .clk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .clk_inv = false,
                .ws_inv = false,
            }
        }
    };
    
    // 安装 I2S
    ESP_ERROR_CHECK(i2s_std_init(I2S_NUM, &std_cfg));
    
    // 配置 DMA
    ESP_ERROR_CHECK(i2s_std_set_clk(I2S_NUM, AUDIO_SAMPLE_RATE, I2S_STD_SLOT_WIDTH_16BIT, I2S_STD_SLOT_MONO));
    
    ESP_LOGI(TAG, "I2S initialized");
    return ESP_OK;
}

// ES8311 初始化（简化版，实际需要完整的 I2C 初始化）
static esp_err_t es8311_init(void)
{
    ESP_LOGI(TAG, "ES8311 init (simplified)...");
    // TODO: ES8311 I2C 寄存器配置
    // 参考 waveshare 示例程序中的 es8311_codec_init()
    
    // 基本配置
    // - 使能 I2S 接口
    // - 设置采样率 16kHz
    // - 设置 16-bit 位宽
    // - 配置 ADC 和 DAC
    
    return ESP_OK;
}

// 录音任务
static void audio_capture_task(void* arg)
{
    ESP_LOGI(TAG, "Audio capture task started");
    size_t bytes_read = 0;
    
    while (s_capturing) {
        // 读取 I2S 数据
        esp_err_t ret = i2s_read(I2S_NUM, s_dma_buffer, sizeof(s_dma_buffer), 
                                  &bytes_read, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int16_t);
            
            // 调用回调（如果有）
            if (s_capture_cb) {
                s_capture_cb(s_dma_buffer, samples, s_capture_cb_arg);
            }
            
            // Phase-1: accumulate DMA samples into 40ms chunks for WS
            chunk_accumulator_push(s_dma_buffer, samples);
            
            // 复制到环形缓冲（用于 VAD）
            memcpy(s_audio_buffer + s_buffer_pos, s_dma_buffer, 
                   MIN(bytes_read, (AUDIO_BUFFER_SIZE - s_buffer_pos) * sizeof(int16_t)));
            s_buffer_pos = (s_buffer_pos + samples) % AUDIO_BUFFER_SIZE;
        }
    }
    
    ESP_LOGI(TAG, "Audio capture task stopped");
    vTaskDelete(NULL);
}

// 播放任务
static void audio_playback_task(void* arg)
{
    const int16_t* audio_data = (const int16_t*)arg;
    size_t bytes_to_write = 0;
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    // 播放音频数据（简化版）
    // 实际需要从 arg 获取音频数据和长度
    // 这里暂时只打印日志
    
    while (s_playing && bytes_to_write > 0) {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_write(I2S_NUM, audio_data, bytes_to_write, 
                                   &bytes_written, pdMS_TO_TICKS(100));
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
            break;
        }
        
        bytes_to_write -= bytes_written;
        audio_data += bytes_written / sizeof(int16_t);
    }
    
    s_playing = false;
    ESP_LOGI(TAG, "Audio playback task finished");
    vTaskDelete(NULL);
}

esp_err_t audio_init(void)
{
    if (s_initialized) return ESP_OK;
    
    ESP_LOGI(TAG, "Audio Manager initializing...");
    
    // 创建互斥锁
    s_audio_mutex = xSemaphoreCreateMutex();
    
    // Phase-1: create chunk queue (holds up to 8 x 640-sample chunks)
    s_chunk_queue = xQueueCreate(8, AUDIO_CHUNK_SIZE_BYTES);
    if (!s_chunk_queue) {
        ESP_LOGE(TAG, "Failed to create chunk queue");
    } else {
        s_chunk_queue_initialized = true;
        ESP_LOGI(TAG, "Chunk queue created");
    }
    
    // 初始化 I2S
    ESP_ERROR_CHECK(i2s_init());
    
    // 初始化 ES8311
    ESP_ERROR_CHECK(es8311_init());
    
    s_initialized = true;
    ESP_LOGI(TAG, "Audio Manager initialized");
    return ESP_OK;
}

esp_err_t audio_start_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    
    if (s_capturing) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_OK;
    }
    
    // Reset chunk accumulator
    s_chunk_accum_count = 0;
    
    // Flush chunk queue
    if (s_chunk_queue) {
        int16_t dummy[AUDIO_CHUNK_SAMPLES];
        while (xQueueReceive(s_chunk_queue, dummy, 0) == pdTRUE) {}
    }
    
    s_capturing = true;
    s_buffer_pos = 0;
    
    // 启动录音任务
    xTaskCreate(audio_capture_task, "audio_capture", 4096, NULL, 10, NULL);
    
    xSemaphoreGive(s_audio_mutex);
    
    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_stop_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

    // Phase-1: flush remaining accumulator samples as last chunk
    if (s_capturing && s_chunk_accum_count > 0) {
        // Pad to full chunk and push
        while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES) {
            s_chunk_accum[s_chunk_accum_count++] = 0;
        }
        BaseType_t woken = pdFALSE;
        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        memcpy(chunk, s_chunk_accum, sizeof(chunk));
        s_chunk_accum_count = 0;
        if (s_chunk_queue) {
            if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
                int16_t dummy;
                xQueueReceiveFromISR(s_chunk_queue, &dummy, &woken);
                xQueueSendFromISR(s_chunk_queue, chunk, &woken);
            }
        }
    }

    s_capturing = false;
    xSemaphoreGive(s_audio_mutex);
    
    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

bool audio_is_capturing(void)
{
    return s_capturing;
}

esp_err_t audio_play_tts(const char* text)
{
    // TODO: 实现 TTS 播放
    // 1. 调用 HTTP API 获取 TTS 音频
    // 2. 解码音频
    // 3. 通过 I2S 播放
    
    ESP_LOGI(TAG, "Playing TTS: %s", text);
    
    s_playing = true;
    
    // 暂时模拟播放
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_playing = false;
    
    return ESP_OK;
}

esp_err_t audio_play_beep(audio_beep_type_t type)
{
    ESP_LOGI(TAG, "Playing beep: %d", type);
    
    // 提示音应该使用预存的短音频
    // TODO: 实现提示音播放
    
    return ESP_OK;
}

esp_err_t audio_stop_playback(void)
{
    s_playing = false;
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s_playing;
}

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    
    // TODO: 设置 ES8311 音量
    ESP_LOGI(TAG, "Volume set to %d", volume);
    return ESP_OK;
}

int audio_get_volume(void)
{
    return s_volume;
}

void audio_register_capture_callback(audio_capture_cb_t cb, void* arg)
{
    s_capture_cb = cb;
    s_capture_cb_arg = arg;
}

// ============================================================
// Phase-1 STT audio: chunked capture read
// ============================================================

bool audio_manager_read_chunk(int16_t* buf, size_t* count, int timeout_ms)
{
    if (!buf || !count) return false;
    *count = 0;

    if (!s_capturing || !s_chunk_queue_initialized) {
        return false;
    }

    // Wait for next 40ms chunk from queue
    BaseType_t ret = xQueueReceive(s_chunk_queue, buf, pdMS_TO_TICKS(timeout_ms));
    if (ret == pdTRUE) {
        *count = AUDIO_CHUNK_SAMPLES;
        return true;
    }
    return false;
}

esp_err_t audio_manager_send_interrupt_before_capture(void)
{
    ESP_LOGI(TAG, "Sending interrupt before new capture");
    return ws_client_send_interrupt();
}

/**
 * Flush any remaining samples in the chunk accumulator as the final chunk.
 * Call this when stopping capture to ensure no samples are lost.
 * Must be called while s_capturing is still true, before or after stop.
 */
void audio_manager_flush_accumulator(void)
{
    if (!s_chunk_queue_initialized || s_chunk_accum_count == 0) {
        return;
    }

    // Pad remainder with silence (0)
    while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES) {
        s_chunk_accum[s_chunk_accum_count++] = 0;
    }

    BaseType_t woken = pdFALSE;
    int16_t chunk[AUDIO_CHUNK_SAMPLES];
    memcpy(chunk, s_chunk_accum, sizeof(chunk));
    s_chunk_accum_count = 0;

    if (xQueueSendFromISR(s_chunk_queue, chunk, &woken) != pdTRUE) {
        int16_t dummy;
        xQueueReceiveFromISR(s_chunk_queue, &dummy, &woken);
        xQueueSendFromISR(s_chunk_queue, chunk, &woken);
    }
    ESP_LOGI(TAG, "Flushed final %d samples as last chunk", AUDIO_CHUNK_SAMPLES);
}