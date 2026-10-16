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
#include "config_store.h"
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
#include <math.h>

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

/* Signalled by audio_capture_task right before it deletes itself, so
 * audio_stop_capture() can wait for the in-flight i2s_channel_read() to
 * actually return before the power gate disables the RX channel underneath
 * it (see audio_power_release() below). */
static SemaphoreHandle_t  s_capture_done_sem = NULL;

/* ── Audio hardware power gate ────────────────────────────────────────────
 * Reference-counted: the ES8311 codec, the I2S TX/RX channels, and the
 * speaker PA (GPIO46) are only powered while >=1 consumer (mic capture, TTS
 * playback, PCM-stream playback, or the media player's raw-I2S WAV playback)
 * holds the gate.  Protected by a mutex so acquire/release from different
 * tasks (recording, tts_player, pcm_stream_player, media_player) can never
 * race, and release() refuses to go below zero instead of underflowing. */
static SemaphoreHandle_t  s_power_mutex   = NULL;
static int                s_power_refcount = 0;  /* 0 == hardware gated off */

/* Settle time budget, split the same way audio_init() already proved safe:
 *   - 10 ms after re-enabling the I2S channels, before touching the codec
 *     over I2C, for MCLK/BCLK to stabilise.
 *   - 10 ms after restoring the codec's analog/DAC power registers, before
 *     re-enabling the HP/output driver register and the external PA, so the
 *     analog stage settles and we don't audibly pop the speaker.
 * Total worst-case power-up latency: ~20 ms, paid only by the FIRST consumer
 * to acquire (subsequent acquires while already powered are refcount-only
 * and add no delay). */
#define AUDIO_POWER_I2S_SETTLE_MS     10
#define AUDIO_POWER_ANALOG_SETTLE_MS  10

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

    /* Start at the legacy level; audio_init() applies the persisted volume
     * after config_store has performed its one-time migration. */
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_REG_DAC32, 0xCB), TAG, "DAC32");
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

/* ── ES8311 power gate register sequences ─────────────────────────────────
 * These touch exactly the four registers audio_init()/es8311_init() sets to
 * bring the codec up (CLK1, SYS0D, SYS12, SYS13); every other register
 * (I2S format, mic/PGA select, ADC/DAC volume, EQ bypass) is configured once
 * at boot and left alone — it does not need to be re-applied on power-up.
 * The power-down values are the documented power-up values reversed (a
 * cleared enable bit / cleared clock-enable field), derived from the same
 * comments already in es8311_init(); this is NOT independently verified
 * against the ES8311 datasheet beyond what that function already encodes,
 * so the actual idle-current drop and pop-free-ness of these writes still
 * need to be confirmed on hardware (see report). */
static void es8311_registers_power_down(void)
{
    /* Mute/disable the output driver first so nothing downstream pops while
     * the analog rails and clocks are subsequently removed. */
    es8311_write(ES8311_REG_SYS13, 0x00);  /* HP/output driver disable (was 0x10 enable) */
    es8311_write(ES8311_REG_SYS12, 0x01);  /* DAC power down (was 0x00 power up)         */
    es8311_write(ES8311_REG_SYS0D, 0x00);  /* analog power down (was 0x01 power up)      */
    es8311_write(ES8311_REG_CLK1,  0x00);  /* gate all internal clocks (was 0x3F all on) */
}

static void es8311_registers_power_up(void)
{
    es8311_write(ES8311_REG_CLK1,  0x3F);  /* all clocks on, MCLK from pin (matches init) */
    es8311_write(ES8311_REG_SYS0D, 0x01);  /* analog power up */
    es8311_write(ES8311_REG_SYS12, 0x00);  /* DAC power up */
    vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_ANALOG_SETTLE_MS));
    es8311_write(ES8311_REG_SYS13, 0x10);  /* HP/output driver enable, last, to avoid a pop */
}

/* Hardware transition helpers.  Callers must hold s_power_mutex. */
static void audio_hw_power_down(void)
{
    gpio_set_level(PA_EN_IO, 0);
    es8311_registers_power_down();
    if (s_tx_handle) i2s_channel_disable(s_tx_handle);
    if (s_rx_handle) i2s_channel_disable(s_rx_handle);
    ESP_LOGI(TAG, "Audio HW power DOWN (codec standby, I2S disabled, PA off)");
}

static void audio_hw_power_up(void)
{
    if (s_tx_handle) i2s_channel_enable(s_tx_handle);
    if (s_rx_handle) i2s_channel_enable(s_rx_handle);
    vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_I2S_SETTLE_MS));
    es8311_registers_power_up();
    vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_ANALOG_SETTLE_MS));
    gpio_set_level(PA_EN_IO, 1);
    ESP_LOGI(TAG, "Audio HW power UP (codec+I2S+PA), settle=%dms",
             AUDIO_POWER_I2S_SETTLE_MS + 2 * AUDIO_POWER_ANALOG_SETTLE_MS);
}

/* ── Public power gate API ────────────────────────────────────────────────── */
esp_err_t audio_power_acquire(const char *reason)
{
    if (!s_power_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_power_mutex, portMAX_DELAY);
    int prev = s_power_refcount++;
    ESP_LOGI(TAG, "Audio power ACQUIRE (%s): refcount %d -> %d",
             reason ? reason : "?", prev, s_power_refcount);
    if (prev == 0) {
        audio_hw_power_up();
    }
    xSemaphoreGive(s_power_mutex);
    return ESP_OK;
}

esp_err_t audio_power_release(const char *reason)
{
    if (!s_power_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_power_mutex, portMAX_DELAY);
    if (s_power_refcount <= 0) {
        /* Never go negative — a caller bug here must not corrupt the gate
         * into thinking hardware is needed forever (or turn it off under a
         * still-active consumer). */
        ESP_LOGW(TAG, "Audio power RELEASE (%s) with refcount already 0 — ignored",
                 reason ? reason : "?");
        xSemaphoreGive(s_power_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int prev = s_power_refcount--;
    ESP_LOGI(TAG, "Audio power RELEASE (%s): refcount %d -> %d",
             reason ? reason : "?", prev, s_power_refcount);
    if (s_power_refcount == 0) {
        audio_hw_power_down();
    }
    xSemaphoreGive(s_power_mutex);
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
    if (s_capture_done_sem) xSemaphoreGive(s_capture_done_sem);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t audio_init(void)
{
    if (s_initialized) return ESP_OK;

    s_audio_mutex = xSemaphoreCreateMutex();
    s_power_mutex = xSemaphoreCreateMutex();
    s_capture_done_sem = xSemaphoreCreateBinary();
    if (!s_audio_mutex || !s_power_mutex || !s_capture_done_sem) {
        ESP_LOGE(TAG, "Audio mutex/semaphore creation failed");
        return ESP_ERR_NO_MEM;
    }

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

    s_initialized = true;

    /* Apply the migrated/user volume to the actual ES8311 DAC register.  This
     * is a plain I2C write to a digital volume register (DAC32), independent
     * of the four power registers the gate below controls, so it is safe to
     * apply before the codec goes to standby. */
    app_config_t cfg = {0};
    if (config_get(&cfg) == ESP_OK) {
        audio_set_volume(cfg.volume);
    } else {
        audio_set_volume(DEFAULT_VOLUME);
    }

    /* No consumer is active yet at boot (refcount==0) — go straight to the
     * gated-off state instead of momentarily enabling the PA and then
     * powering back down, which would risk an audible click.  PA_EN_IO is
     * already low from the "avoid pop" setup above. */
    audio_hw_power_down();

    ESP_LOGI(TAG, "Audio pipeline ready (gated off, refcount=0)");

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

    /* Power the codec/I2S/PA on (no-op if another consumer already holds the
     * gate) before starting the capture task that reads the RX channel. */
    audio_power_acquire("capture");
    if (s_capture_done_sem) xSemaphoreTake(s_capture_done_sem, 0);  /* clear stale signal */

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

    bool was_capturing = s_capturing;

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

    if (was_capturing) {
        /* Wait for audio_capture_task to actually exit before releasing the
         * power gate: if this is the last consumer, release() disables the
         * RX I2S channel, and doing that while i2s_channel_read() is still
         * blocked on it (up to its 80 ms timeout) would race the driver.
         * 200 ms leaves comfortable margin over that 80 ms read timeout. */
        if (s_capture_done_sem) xSemaphoreTake(s_capture_done_sem, pdMS_TO_TICKS(200));
        audio_power_release("capture");
    }

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

/* ── PCM playback ─────────────────────────────────────────────────────────── *
 * The I2S bus runs in STEREO 16-bit mode (ES8311 standard I2S).  Mono PCM is
 * duplicated to both L/R slots.  Writes block until the DMA has accepted the
 * data, which paces playback to real time.                                    */
#define PLAY_BLOCK_SAMPLES  256   /* mono samples per write block */
static int16_t s_play_stereo[PLAY_BLOCK_SAMPLES * 2];

/* The ES8311 register gives a useful coarse range, but a low slider value
 * still sounded too hot with full-scale TTS PCM.  Keep the persisted slider
 * and codec mapping intact, then add a bounded digital trim only in the
 * bottom fifth of the range.  At 20 and above this is unity; below 20 it is
 * quadratic, making the overall low end perceptually softer without changing
 * sample rate, I2S framing, or the capture path. */
static int32_t playback_pcm_gain_q15(void)
{
    int volume = s_volume;
    if (volume <= 0) return 0;
    if (volume >= 20) return 32768;
    int64_t low = (int64_t)volume * 32768 / 20;
    return (int32_t)((low * low + 16384) / 32768);
}

esp_err_t audio_write_pcm(const int16_t *mono, size_t samples, int timeout_ms)
{
    if (!s_initialized || !s_tx_handle || !mono) return ESP_ERR_INVALID_STATE;

    size_t done = 0;
    const int32_t gain_q15 = playback_pcm_gain_q15();
    while (done < samples) {
        size_t n = samples - done;
        if (n > PLAY_BLOCK_SAMPLES) n = PLAY_BLOCK_SAMPLES;

        for (size_t i = 0; i < n; i++) {
            int32_t sample = ((int32_t)mono[done + i] * gain_q15) >> 15;
            if (sample > INT16_MAX) sample = INT16_MAX;
            if (sample < INT16_MIN) sample = INT16_MIN;
            s_play_stereo[i * 2]     = (int16_t)sample;  /* L */
            s_play_stereo[i * 2 + 1] = (int16_t)sample;  /* R */
        }

        size_t written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_handle, s_play_stereo,
                                          n * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(timeout_ms));
        if (ret != ESP_OK) return ret;
        if (written != n * 2 * sizeof(int16_t)) {
            ESP_LOGW(TAG, "I2S short write: %u/%u bytes",
                     (unsigned)written, (unsigned)(n * 2 * sizeof(int16_t)));
            return ESP_ERR_TIMEOUT;
        }
        done += n;
    }
    return ESP_OK;
}

/* ── Test tone — proves the I2S TX → ES8311 DAC → PA path produces sound ──── */
esp_err_t audio_play_test_tone(int freq_hz, int duration_ms)
{
    if (!s_initialized || !s_tx_handle) return ESP_ERR_INVALID_STATE;

    const int    rate   = AUDIO_SAMPLE_RATE;
    const size_t total  = (size_t)rate * duration_ms / 1000;
    const float  step   = 2.0f * 3.14159265f * (float)freq_hz / (float)rate;
    const int16_t amp   = 8000;   /* ~25% full scale, comfortable level */

    ESP_LOGI(TAG, "Playing test tone: %d Hz, %d ms", freq_hz, duration_ms);
    audio_power_acquire("test_tone");
    s_playing = true;

    int16_t block[PLAY_BLOCK_SAMPLES];
    float   phase = 0.0f;
    size_t  done  = 0;
    while (done < total && s_playing) {
        size_t n = total - done;
        if (n > PLAY_BLOCK_SAMPLES) n = PLAY_BLOCK_SAMPLES;
        for (size_t i = 0; i < n; i++) {
            block[i] = (int16_t)(amp * sinf(phase));
            phase += step;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        audio_write_pcm(block, n, 200);
        done += n;
    }

    s_playing = false;
    audio_power_release("test_tone");
    ESP_LOGI(TAG, "Test tone done");
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_es8311_dev) {
        /* Treat the old 80 setting (DAC32=0xCB, +6 dB) as unity.  Convert
         * the requested percentage to a linear amplitude ratio, then use the
         * ES8311's 0.5 dB DAC step.  This makes 24 exactly 30% amplitude of
         * the old fixed level without mapping it to an effectively silent
         * register code. */
        int reg32 = 0;
        float gain_db = -95.5f;
        if (volume > 0) {
            const float ratio = (float)volume / (float)LEGACY_DEFAULT_VOLUME;
            gain_db = 6.0f + 20.0f * log10f(ratio);
            if (gain_db < -95.5f) gain_db = -95.5f;
            if (gain_db > 32.0f) gain_db = 32.0f;
            reg32 = (int)lroundf(0xBF + (gain_db * 2.0f));
            if (reg32 < 0) reg32 = 0;
            if (reg32 > 0xFF) reg32 = 0xFF;
        }
        esp_err_t ret = es8311_write(ES8311_REG_DAC32, (uint8_t)reg32);
        uint8_t actual_reg32 = 0;
        esp_err_t read_ret = (ret == ESP_OK)
                           ? es8311_read(ES8311_REG_DAC32, &actual_reg32)
                           : ret;
        ESP_LOGI(TAG, "Volume applied: %d/100 DAC32=0x%02X readback=0x%02X gain=%.1fdB pcm_gain=%ld/32768%s",
                 volume, reg32, actual_reg32, gain_db,
                 (long)playback_pcm_gain_q15(),
                 ret == ESP_OK && read_ret == ESP_OK ? "" : " (codec I/O failed)");
        if (ret != ESP_OK) return ret;
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
