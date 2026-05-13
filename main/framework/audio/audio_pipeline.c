/*
 * audio_pipeline.c — ES8311 + I2S full-duplex audio (Waveshare AMOLED-1.8).
 *
 * Hardware:
 *   ES8311 codec on I2C0 (SDA=GPIO15, SCL=GPIO14) at address 0x18
 *   I2S0 full-duplex: MCLK=GPIO16, BCK=GPIO9, WS=GPIO45, DO=GPIO8, DI=GPIO10
 *   PA enable: GPIO46 (active HIGH)
 *
 * Audio format: 16 kHz, 16-bit, mono @ 16 kHz
 *   I2S runs in stereo mode (ES8311 standard I2S); left channel = mic/speaker.
 *   Capture task de-interleaves stereo → mono before queueing.
 *
 * MCLK = 16000 × 256 = 4.096 MHz  → coeff entry {4096000, 16000, ...}
 */

#include "audio_pipeline.h"
#include "display_driver.h"
#include "ws_protocol.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "AUDIO";

/* ── Pin definitions ──────────────────────────────────────────────────────── */
#define I2S_MCLK_IO   GPIO_NUM_16
#define I2S_BCK_IO    GPIO_NUM_9
#define I2S_WS_IO     GPIO_NUM_45
#define I2S_DO_IO     GPIO_NUM_8   /* ESP → ES8311 DAC (playback) */
#define I2S_DI_IO     GPIO_NUM_10  /* ES8311 ADC → ESP (capture)  */
#define PA_EN_IO      GPIO_NUM_46  /* Power amplifier enable, active HIGH */

/* ── ES8311 register addresses ────────────────────────────────────────────── */
#define ES8311_ADDR             0x18
#define ES8311_REG_RESET        0x00
#define ES8311_REG_CLK1         0x01
#define ES8311_REG_CLK2         0x02
#define ES8311_REG_CLK3         0x03  /* ADC osr */
#define ES8311_REG_CLK4         0x04  /* DAC osr */
#define ES8311_REG_CLK5         0x05  /* ADC/DAC clock dividers */
#define ES8311_REG_CLK6         0x06  /* BCLK divider + invert */
#define ES8311_REG_CLK7         0x07  /* LRCK divider high byte */
#define ES8311_REG_CLK8         0x08  /* LRCK divider low byte */
#define ES8311_REG_SDPIN        0x09  /* SDP In (DAC) */
#define ES8311_REG_SDPOUT       0x0A  /* SDP Out (ADC) */
#define ES8311_REG_SYS0D        0x0D  /* Analog power */
#define ES8311_REG_SYS0E        0x0E  /* ADC modulator */
#define ES8311_REG_SYS12        0x12  /* DAC power */
#define ES8311_REG_SYS13        0x13  /* Output driver */
#define ES8311_REG_SYS14        0x14  /* MIC / PGA select */
#define ES8311_REG_ADC17        0x17  /* ADC volume */
#define ES8311_REG_ADC1C        0x1C  /* ADC equalizer */
#define ES8311_REG_DAC32        0x32  /* DAC volume */
#define ES8311_REG_DAC37        0x37  /* DAC equalizer */

/* ── I2S handles ──────────────────────────────────────────────────────────── */
static i2s_chan_handle_t s_tx_handle = NULL;  /* playback */
static i2s_chan_handle_t s_rx_handle = NULL;  /* capture  */

/* ── ES8311 I2C device handle ─────────────────────────────────────────────── */
static i2c_master_dev_handle_t s_es8311_dev = NULL;

/* ── Capture state ────────────────────────────────────────────────────────── */
static bool             s_capturing   = false;
static bool             s_playing     = false;
static int              s_volume      = 80;
static bool             s_initialized = false;

static audio_capture_cb_t s_capture_cb     = NULL;
static void              *s_capture_cb_arg  = NULL;

static SemaphoreHandle_t  s_audio_mutex      = NULL;

/* Chunk queue — AUDIO_CHUNK_SAMPLES mono int16_t per slot */
static QueueHandle_t      s_chunk_queue      = NULL;
static bool               s_chunk_queue_init = false;
static int16_t            s_chunk_accum[AUDIO_CHUNK_SAMPLES];
static size_t             s_chunk_accum_count = 0;

/* Stereo I2S read buffer: 2× chunk size (one stereo frame = 2 × int16_t) */
static int16_t  s_stereo_buf[AUDIO_CHUNK_SAMPLES * 2];
/* Mono DMA buffer used by capture callback and chunk accumulator */
static int16_t  s_dma_buffer[AUDIO_CHUNK_SAMPLES];

/* ── Chunk accumulator ────────────────────────────────────────────────────── */
static void chunk_accumulator_push(const int16_t *samples, size_t count)
{
    if (!s_chunk_queue_init || !s_capturing) return;

    for (size_t i = 0; i < count; i++) {
        s_chunk_accum[s_chunk_accum_count++] = samples[i];
        if (s_chunk_accum_count >= AUDIO_CHUNK_SAMPLES) {
            int16_t chunk[AUDIO_CHUNK_SAMPLES];
            memcpy(chunk, s_chunk_accum, sizeof(chunk));
            s_chunk_accum_count = 0;
            if (xQueueSend(s_chunk_queue, chunk, 0) != pdTRUE) {
                /* Drop oldest chunk to make room */
                int16_t dummy[AUDIO_CHUNK_SAMPLES];
                xQueueReceive(s_chunk_queue, dummy, 0);
                xQueueSend(s_chunk_queue, chunk, 0);
            }
        }
    }
}

/* ── ES8311 helpers ───────────────────────────────────────────────────────── */
static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    if (!s_es8311_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_es8311_dev, buf, 2, 50);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val)
{
    if (!s_es8311_dev || !val) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = i2c_master_transmit(s_es8311_dev, &reg, 1, 50);
    if (ret != ESP_OK) return ret;
    return i2c_master_receive(s_es8311_dev, val, 1, 50);
}

/* ── ES8311 init (16 kHz, 16-bit, I2S slave, MCLK=4.096 MHz) ─────────────── */
static esp_err_t es8311_init(void)
{
    uint8_t regv;

    /* Reset */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_RESET, 0x1F), TAG, "ES8311 reset");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_RESET, 0x00), TAG, "ES8311 reset release");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_RESET, 0x80), TAG, "ES8311 power-on");

    /*
     * Clock setup for MCLK=4.096 MHz, fs=16 kHz (coeff table entry):
     *   {4096000, 16000, pre_div=1, pre_multi=0, adc_div=1, dac_div=1,
     *    fs_mode=0, lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x10}
     */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK1, 0x3F), TAG, "CLK1"); /* all clocks on, MCLK from pin */

    /* REG02: pre_div-1=0 in [7:5], pre_multi=0 in [4:3], preserve [2:0] */
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_REG_CLK2, &regv), TAG, "CLK2 rd");
    regv = (regv & 0x07);   /* keep reserved bits, clear div fields */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK2, regv), TAG, "CLK2");

    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK3, 0x10), TAG, "CLK3"); /* fs_mode=0, adc_osr=0x10 */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK4, 0x10), TAG, "CLK4"); /* dac_osr=0x10 */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK5, 0x00), TAG, "CLK5"); /* adc_div=1, dac_div=1 */

    /* REG06: preserve [7:5], bclk_div-1 = 3 in [4:0] */
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_REG_CLK6, &regv), TAG, "CLK6 rd");
    regv = (regv & 0xE0) | 0x03;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK6, regv), TAG, "CLK6");

    /* REG07: preserve [7:6], lrck_h=0x00 in [5:0] */
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_REG_CLK7, &regv), TAG, "CLK7 rd");
    regv = (regv & 0xC0) | 0x00;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK7, regv), TAG, "CLK7");

    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_CLK8, 0xFF), TAG, "CLK8"); /* lrck_l */

    /* I2S slave mode: clear bit6 of REG00 */
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_REG_RESET, &regv), TAG, "REG00 rd");
    regv &= 0xBF;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_RESET, regv), TAG, "REG00 slave");

    /* 16-bit I2S standard mode for both DAC input and ADC output */
    /* ES8311_RESOLUTION_16 → bit field (3<<2) = 0x0C */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SDPIN,  0x0C), TAG, "SDPIN");   /* DAC: 16-bit */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SDPOUT, 0x0C), TAG, "SDPOUT");  /* ADC: 16-bit */

    /* Power up analog circuitry */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SYS0D, 0x01), TAG, "SYS0D");  /* analog power */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SYS0E, 0x02), TAG, "SYS0E");  /* PGA + ADC mod */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SYS12, 0x00), TAG, "SYS12");  /* DAC power up */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SYS13, 0x10), TAG, "SYS13");  /* HP output enable */

    /* ADC: analog mic, max PGA gain */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_SYS14, 0x1A), TAG, "SYS14");  /* analog mic, PGA */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_ADC17, 0xC8), TAG, "ADC17");  /* ADC digital gain */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_ADC1C, 0x6A), TAG, "ADC1C");  /* ADC equalize bypass */

    /* DAC: set volume to 80%, bypass equalizer */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_DAC32, 0xCB), TAG, "DAC32");  /* 80% vol */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_DAC37, 0x08), TAG, "DAC37");  /* DAC EQ bypass */

    ESP_LOGI(TAG, "ES8311 init OK (16kHz/16bit/I2S slave)");
    return ESP_OK;
}

/* ── I2S full-duplex init ─────────────────────────────────────────────────── */
static esp_err_t i2s_init(void)
{
    /* Create full-duplex channel (tx + rx share MCLK/BCK/WS) */
    i2s_chan_config_t chan_cfg = {
        .id                  = I2S_NUM_0,
        .role                = I2S_ROLE_MASTER,
        .dma_desc_num        = 4,
        .dma_frame_num       = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority       = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = I2S_STD_SLOT_BOTH,
            .ws_width       = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol         = false,
            .bit_shift      = true,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "i2s tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "i2s rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "i2s rx enable");

    ESP_LOGI(TAG, "I2S init OK (MCLK=%d BCK=%d WS=%d DO=%d DI=%d)",
             I2S_MCLK_IO, I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO);
    return ESP_OK;
}

/* ── Capture task ─────────────────────────────────────────────────────────── */
static void audio_capture_task(void *arg)
{
    ESP_LOGI(TAG, "Capture task started (Core %d)", xPortGetCoreID());

    while (s_capturing) {
        size_t bytes_read = 0;

        /*
         * I2S stereo: each frame = 2 × int16_t (L, R).
         * Read AUDIO_CHUNK_SAMPLES stereo frames → 2 × AUDIO_CHUNK_SAMPLES int16_t.
         * Left channel (index 0, 2, 4, ...) = microphone output.
         */
        esp_err_t ret = i2s_channel_read(
            s_rx_handle,
            s_stereo_buf,
            sizeof(s_stereo_buf),
            &bytes_read,
            pdMS_TO_TICKS(80));

        if (ret != ESP_OK || bytes_read == 0) continue;

        /* De-interleave: extract left channel → mono */
        size_t stereo_samples = bytes_read / sizeof(int16_t);
        size_t mono_count     = stereo_samples / 2;
        for (size_t i = 0; i < mono_count; i++) {
            s_dma_buffer[i] = s_stereo_buf[i * 2];  /* left channel */
        }

        /* Fire capture callback */
        if (s_capture_cb) s_capture_cb(s_dma_buffer, mono_count, s_capture_cb_arg);

        /* Push into chunk queue */
        chunk_accumulator_push(s_dma_buffer, mono_count);
    }

    ESP_LOGI(TAG, "Capture task stopped");
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t audio_init(void)
{
    if (s_initialized) return ESP_OK;

    s_audio_mutex = xSemaphoreCreateMutex();

    s_chunk_queue = xQueueCreate(8, AUDIO_CHUNK_SIZE_BYTES);
    if (s_chunk_queue) {
        s_chunk_queue_init = true;
        ESP_LOGI(TAG, "Chunk queue created (8 × %d bytes)", AUDIO_CHUNK_SIZE_BYTES);
    } else {
        ESP_LOGE(TAG, "Chunk queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    /* ── Attach ES8311 to the shared I2C bus ──────────────────────────────── */
    i2c_master_bus_handle_t i2c_bus = display_driver_get_i2c_bus();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not ready — call display_driver_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t es8311_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_ADDR,
        .scl_speed_hz    = 200000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(i2c_bus, &es8311_cfg, &s_es8311_dev),
        TAG, "ES8311 I2C attach");

    /* ── Configure PA GPIO before codec (avoid pop) ───────────────────────── */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = BIT64(PA_EN_IO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    gpio_set_level(PA_EN_IO, 0);   /* PA off during init to suppress pop */

    /* ── Init I2S (starts MCLK → ES8311 needs MCLK before accepting I2C) ─── */
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "I2S init");
    vTaskDelay(pdMS_TO_TICKS(10));  /* let MCLK stabilise */

    /* ── Init ES8311 via I2C ──────────────────────────────────────────────── */
    ESP_RETURN_ON_ERROR(es8311_init(), TAG, "ES8311 init");

    /* ── Enable PA (speaker) ──────────────────────────────────────────────── */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PA_EN_IO, 1);
    ESP_LOGI(TAG, "PA enabled (GPIO%d)", PA_EN_IO);

    s_initialized = true;
    ESP_LOGI(TAG, "Audio pipeline ready");
    return ESP_OK;
}

esp_err_t audio_start_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (s_capturing) { xSemaphoreGive(s_audio_mutex); return ESP_OK; }

    /* Drain stale chunks */
    s_chunk_accum_count = 0;
    if (s_chunk_queue) {
        int16_t dummy[AUDIO_CHUNK_SAMPLES];
        while (xQueueReceive(s_chunk_queue, dummy, 0) == pdTRUE) {}
    }

    s_capturing = true;
    xTaskCreate(audio_capture_task, "audio_cap", 4096, NULL, 10, NULL);
    xSemaphoreGive(s_audio_mutex);

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_stop_capture(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

    if (s_capturing && s_chunk_accum_count > 0) {
        /* Flush partial chunk with zero-padding */
        while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES)
            s_chunk_accum[s_chunk_accum_count++] = 0;
        int16_t chunk[AUDIO_CHUNK_SAMPLES];
        memcpy(chunk, s_chunk_accum, sizeof(chunk));
        s_chunk_accum_count = 0;
        if (s_chunk_queue) xQueueSend(s_chunk_queue, chunk, 0);
    }

    s_capturing = false;
    xSemaphoreGive(s_audio_mutex);
    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

bool audio_is_capturing(void) { return s_capturing; }

/*
 * audio_play_tts — stub; real playback handled by tts_player.c via i2s_channel_write.
 * Kept so old callers don't link-fail.
 */
esp_err_t audio_play_tts(const char *text)
{
    ESP_LOGI(TAG, "audio_play_tts (use tts_player instead): %s", text);
    return ESP_OK;
}

esp_err_t audio_play_beep(audio_beep_type_t type)
{
    ESP_LOGI(TAG, "Beep %d (stub)", type);
    return ESP_OK;
}

esp_err_t audio_stop_playback(void)  { s_playing = false; return ESP_OK; }
bool      audio_is_playing(void)     { return s_playing; }

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_es8311_dev) {
        int reg32 = (volume == 0) ? 0 : ((volume * 256 / 100) - 1);
        es8311_write(ES8311_REG_DAC32, (uint8_t)reg32);
    }
    return ESP_OK;
}

int audio_get_volume(void) { return s_volume; }

/* ── Expose I2S TX handle for tts_player ──────────────────────────────────── */
i2s_chan_handle_t audio_get_tx_handle(void) { return s_tx_handle; }

void audio_register_capture_callback(audio_capture_cb_t cb, void *arg)
{
    s_capture_cb     = cb;
    s_capture_cb_arg = arg;
}

bool audio_manager_read_chunk(int16_t *buf, size_t *count, int timeout_ms)
{
    if (!buf || !count) return false;
    *count = 0;
    if (!s_capturing || !s_chunk_queue_init) return false;
    if (xQueueReceive(s_chunk_queue, buf, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *count = AUDIO_CHUNK_SAMPLES;
        return true;
    }
    return false;
}

esp_err_t audio_manager_send_interrupt_before_capture(void)
{
    return ws_client_send_interrupt();
}

void audio_manager_flush_accumulator(void)
{
    if (!s_chunk_queue_init || s_chunk_accum_count == 0) return;
    while (s_chunk_accum_count < AUDIO_CHUNK_SAMPLES)
        s_chunk_accum[s_chunk_accum_count++] = 0;
    int16_t chunk[AUDIO_CHUNK_SAMPLES];
    memcpy(chunk, s_chunk_accum, sizeof(chunk));
    s_chunk_accum_count = 0;
    if (s_chunk_queue) xQueueSend(s_chunk_queue, chunk, 0);
}
