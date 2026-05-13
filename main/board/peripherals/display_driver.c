/*
 * display_driver.c — Real hardware driver for Waveshare ESP32-S3-Touch-AMOLED-1.8.
 *
 * Display:  SH8601  (368×448 AMOLED, QSPI via SPI2_HOST)
 * Touch:    FT5x06  (I2C0, addr 0x38)
 * Expander: TCA9554 (I2C0, addr 0x20) — controls display RST / power
 *
 * Pin map (confirmed from Waveshare BSP / pin_config.h):
 *   LCD  SCLK=11  CS=12  D0=4  D1=5  D2=6  D3=7  RST=N/A(TCA9554)
 *   TP   SDA=15   SCL=14 INT=21  RST=N/A
 */

#include "display_driver.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_io_expander_tca9554.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "DISPLAY_DRV";

/* -------------------------------------------------------------------------
 * Pin assignments (Waveshare ESP32-S3-Touch-AMOLED-1.8)
 * ------------------------------------------------------------------------- */
#define LCD_HOST            SPI2_HOST
#define I2C_HOST            I2C_NUM_0

#define PIN_LCD_SCLK        GPIO_NUM_11
#define PIN_LCD_CS          GPIO_NUM_12
#define PIN_LCD_D0          GPIO_NUM_4
#define PIN_LCD_D1          GPIO_NUM_5
#define PIN_LCD_D2          GPIO_NUM_6
#define PIN_LCD_D3          GPIO_NUM_7

#define PIN_TP_SDA          GPIO_NUM_15
#define PIN_TP_SCL          GPIO_NUM_14
#define PIN_TP_INT          GPIO_NUM_21

#define I2C_FREQ_HZ         (200 * 1000)

/* -------------------------------------------------------------------------
 * LVGL tunables
 * ------------------------------------------------------------------------- */
/* 20 lines of internal-SRAM DMA buffer — reliable, ~15KB each, no PSRAM issues */
#define LVGL_BUF_LINES      20
#define LVGL_TICK_PERIOD_US (2 * 1000)             /* 2 ms in µs for esp_timer */

/* -------------------------------------------------------------------------
 * SH8601 custom init sequence
 *
 * The default driver sequence is missing two critical commands:
 *   • 0x11 (SLPOUT)  — without this the panel stays in sleep after reset
 *   • 0x51 (WRDISBV) — without this brightness defaults to 0 (invisible)
 * ------------------------------------------------------------------------- */
static const sh8601_lcd_init_cmd_t s_sh8601_init_cmds[] = {
    /* Exit sleep mode — MUST be first, panel ignores all other cmds while sleeping */
    {0x11, NULL, 0, 120},
    /* Set tear scanline for V-blank sync */
    {0x44, (uint8_t[]){0x00, 0xC8}, 2, 0},
    /* Tearing Effect Line On (V-blank) */
    {0x35, (uint8_t[]){0x00}, 0, 0},
    /* WRCTRLD: enable brightness control block */
    {0x53, (uint8_t[]){0x20}, 1, 0},
    /* WRDISBV: set brightness to maximum (0xFF) */
    {0x51, (uint8_t[]){0xFF}, 1, 10},
};

/* -------------------------------------------------------------------------
 * Static state
 * ------------------------------------------------------------------------- */
static esp_lcd_panel_handle_t    s_panel        = NULL;
static esp_lcd_panel_io_handle_t s_io_handle    = NULL; /* saved for brightness cmds */
static esp_lcd_touch_handle_t    s_touch        = NULL;
static lv_disp_drv_t             s_disp_drv;
static lv_disp_draw_buf_t        s_draw_buf;
static SemaphoreHandle_t         s_lvgl_mux     = NULL;
static esp_timer_handle_t        s_tick_timer   = NULL;
static i2c_master_bus_handle_t   s_i2c_bus      = NULL; /* saved for diagnostics */

/* -------------------------------------------------------------------------
 * LVGL tick (called by esp_timer every 2 ms)
 * ------------------------------------------------------------------------- */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_US / 1000);
}

/* -------------------------------------------------------------------------
 * LVGL flush callback — transfers LVGL draw buffer to SH8601
 * ------------------------------------------------------------------------- */
static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv,
                          const lv_area_t *area,
                          lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
}

/* Align dirty regions to even pixel boundaries (SH8601 QSPI requirement) */
static void lvgl_rounder_cb(struct _lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

/* -------------------------------------------------------------------------
 * LVGL touch read callback
 * ------------------------------------------------------------------------- */
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t touch_x[1], touch_y[1];
    uint8_t  touch_cnt = 0;

    /* Only read I2C when INT pin is asserted (active low).
     * This avoids I2C errors caused by reading while FT5x06 is in sleep mode. */
    if (gpio_get_level(PIN_TP_INT) != 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_read_data(tp);
    bool pressed = esp_lcd_touch_get_coordinates(tp,
                                                  touch_x, touch_y,
                                                  NULL, &touch_cnt, 1);
    if (pressed && touch_cnt > 0) {
        data->point.x = touch_x[0];
        data->point.y = touch_y[0];
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* -------------------------------------------------------------------------
 * I2C bus scan (diagnostic — logs all found addresses at boot)
 * ------------------------------------------------------------------------- */
static void i2c_scan(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "I2C bus scan:");
    uint8_t addr_found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        esp_err_t ret = i2c_master_probe(bus, addr, 20);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found device @ 0x%02X", addr);
            addr_found++;
        }
    }
    if (addr_found == 0) {
        ESP_LOGW(TAG, "  No I2C devices found!");
    }
}

/* -------------------------------------------------------------------------
 * TCA9554 power-on sequence
 * Pins 0-2 are outputs; drive low then high to reset the display.
 * ------------------------------------------------------------------------- */
static esp_err_t tca9554_power_on(i2c_master_bus_handle_t i2c_bus)
{
    esp_io_expander_handle_t io_exp = NULL;
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(
        i2c_bus,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
        &io_exp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_exp,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2,
        IO_EXPANDER_OUTPUT));

    /* Assert reset (all low) */
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_0, 0);
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_1, 0);
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_2, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Release reset (all high) */
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_0, 1);
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_1, 1);
    esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_2, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "TCA9554 power sequence done");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * display_driver_init
 * ------------------------------------------------------------------------- */
esp_err_t display_driver_init(void)
{
    ESP_LOGI(TAG, "Display driver init...");

    /* --- Shared I2C bus (TCA9554 + FT5x06 touch) --- */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source              = I2C_CLK_SRC_DEFAULT,
        .i2c_port                = I2C_HOST,
        .scl_io_num              = PIN_TP_SCL,
        .sda_io_num              = PIN_TP_SDA,
        .glitch_ignore_cnt       = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus));

    /* Configure touch INT pin as input (active low — FT5x06 asserts on touch) */
    gpio_config_t tp_int_cfg = {
        .pin_bit_mask = (1ULL << PIN_TP_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&tp_int_cfg);

    /* --- TCA9554: reset display, enable backlight --- */
    ESP_ERROR_CHECK(tca9554_power_on(s_i2c_bus));

    /* Scan AFTER power-on so all ICs are awake */
    i2c_scan(s_i2c_bus);

    /* --- SPI bus for SH8601 QSPI --- */
    spi_bus_config_t spi_cfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK,
        PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        DISPLAY_H_RES * DISPLAY_V_RES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

    /* --- SH8601 panel IO (QSPI) --- */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS,
                                    notify_flush_ready,
                                    &s_disp_drv);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));
    s_io_handle = io_handle; /* save for runtime brightness commands */

    /* --- SH8601 vendor config: custom init with SLPOUT + brightness --- */
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds      = s_sh8601_init_cmds,
        .init_cmds_size = sizeof(s_sh8601_init_cmds) / sizeof(s_sh8601_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = GPIO_NUM_NC,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
        .vendor_config   = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "SH8601 panel ready");

    /* --- FT5x06 touch (I2C) --- */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    esp_err_t tp_ret = esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io);
    if (tp_ret == ESP_OK) {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max           = DISPLAY_H_RES,
            .y_max           = DISPLAY_V_RES,
            .rst_gpio_num    = GPIO_NUM_NC,
            .int_gpio_num    = PIN_TP_INT,
            .levels.reset    = 0,
            .levels.interrupt = 0,
            .flags.swap_xy   = 0,
            .flags.mirror_x  = 0,
            .flags.mirror_y  = 0,
        };
        tp_ret = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch);
        if (tp_ret == ESP_OK) {
            ESP_LOGI(TAG, "FT5x06 touch ready");
        } else {
            ESP_LOGW(TAG, "FT5x06 init failed (%s) — touch disabled", esp_err_to_name(tp_ret));
            s_touch = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Touch IO init failed — touch disabled");
        s_touch = NULL;
    }

    /* --- LVGL init --- */
    lv_init();

    /* Double-buffered draw buffers.
     *
     * Allocate from internal SRAM with DMA capability.
     * MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA is invalid on ESP32-S3 (PSRAM is not
     * tagged DMA-capable by the heap allocator), so we use internal SRAM here.
     * 20 lines × 368 px × 2 bytes = 14,720 bytes per buffer — easily fits.
     */
    const size_t buf_sz = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "LVGL draw buffer alloc failed (need %u bytes x2 internal DMA)",
                 (unsigned)buf_sz);
        free(buf1); free(buf2);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL draw buffers: 2 × %u bytes (internal DMA)", (unsigned)buf_sz);
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2,
                          DISPLAY_H_RES * LVGL_BUF_LINES);

    /* Register display driver */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = DISPLAY_H_RES;
    s_disp_drv.ver_res      = DISPLAY_V_RES;
    s_disp_drv.flush_cb     = lvgl_flush_cb;
    s_disp_drv.rounder_cb   = lvgl_rounder_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.user_data    = s_panel;
    lv_disp_drv_register(&s_disp_drv);

    /* Register touch indev (only if touch init succeeded) */
    if (s_touch) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type      = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb   = lvgl_touch_cb;
        indev_drv.user_data = s_touch;
        lv_indev_drv_register(&indev_drv);
    }

    /* LVGL mutex for multi-task safety */
    s_lvgl_mux = xSemaphoreCreateMutex();
    configASSERT(s_lvgl_mux);

    /* LVGL tick timer — calls lv_tick_inc every 2 ms */
    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_US));

    ESP_LOGI(TAG, "Display driver ready — %dx%d", DISPLAY_H_RES, DISPLAY_V_RES);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Mutex helpers
 * ------------------------------------------------------------------------- */
bool display_driver_lock(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, ticks) == pdTRUE;
}

void display_driver_unlock(void)
{
    xSemaphoreGive(s_lvgl_mux);
}

i2c_master_bus_handle_t display_driver_get_i2c_bus(void)
{
    return s_i2c_bus;
}

void display_driver_i2c_scan(void)
{
    if (s_i2c_bus) i2c_scan(s_i2c_bus);
}

/* -------------------------------------------------------------------------
 * Runtime brightness / display-on control
 * ------------------------------------------------------------------------- */
esp_err_t display_set_brightness(uint8_t level)
{
    if (!s_io_handle) return ESP_ERR_INVALID_STATE;
    /* SH8601 WRDISBV (0x51): single-byte brightness, 0x00–0xFF */
    return esp_lcd_panel_io_tx_param(s_io_handle, 0x51, &level, 1);
}

esp_err_t display_set_on(bool on)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_disp_on_off(s_panel, on);
}
