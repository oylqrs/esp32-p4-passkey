#include "lcd_driver.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/color_types.h"
#include "hal/mipi_dsi_host_ll.h"
#include "mipi_dsi_priv.h"
#undef TAG

static const char *TAG = "st7701sn_lcd";

#define LCD_MIPI_DSI_LANE_NUM           2
#define LCD_MIPI_DSI_LANE_BITRATE_MBPS  1000
#define LCD_MIPI_DPI_CLK_MHZ            10

#define LCD_MIPI_DSI_PHY_LDO_CHAN       3
#define LCD_MIPI_DSI_PHY_LDO_MV         2500
#define LCD_USE_DSI_TEST_PATTERN        0

#define LCD_BACKLIGHT_GPIO              19
#define LCD_BACKLIGHT_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER        LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL      LEDC_CHANNEL_0
#define LCD_BACKLIGHT_LEDC_DUTY_RES     LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_LEDC_DUTY_MAX     ((1 << 10) - 1)
#define LCD_BACKLIGHT_LEDC_FREQ_HZ      5000
#define LCD_RESET_GPIO                  GPIO_NUM_1

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t data_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static void lcd_check_mipi_step(const char *step, esp_err_t ret)
{
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MIPI check OK: %s", step);
    } else {
        ESP_LOGE(TAG, "MIPI check failed: %s, err=%s", step, esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK(ret);
}

static void lcd_backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LCD_BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = LCD_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const ledc_channel_config_t channel_config = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
}

void lcd_driver_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    const uint32_t duty = LCD_BACKLIGHT_LEDC_DUTY_MAX * percent / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL));
}

static void lcd_reset_init_and_release(void)
{
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << LCD_RESET_GPIO,
       // .mode = GPIO_MODE_OUTPUT,
       .mode = GPIO_MODE_INPUT_OUTPUT ,
       .pull_up_en = GPIO_PULLUP_ENABLE,   // 打开内部上拉
       // .pull_up_en = GPIO_PULLUP_DISABLE,
       // .pull_down_en = GPIO_PULLDOWN_DISABLE,
      // .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,

 

    };
    gpio_config(&reset_config);
    //gpio_set_input_enabled(GPIO_NUM_1, true); // 关键：开启输入采样通路

    ESP_LOGI(TAG, "LCD reset GPIO%d configured, readback=%d", LCD_RESET_GPIO, gpio_get_level(LCD_RESET_GPIO));
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_LOGI(TAG, "Precharge LCD reset on GPIO%d high", LCD_RESET_GPIO);
    gpio_set_level(LCD_RESET_GPIO, 1);
    ESP_LOGI(TAG, "LCD reset GPIO%d precharge high, readback=%d", LCD_RESET_GPIO, gpio_get_level(LCD_RESET_GPIO));
    vTaskDelay(pdMS_TO_TICKS(12));
    //vTaskDelay(pdMS_TO_TICKS(1));

    ESP_LOGI(TAG, "Assert LCD reset on GPIO%d", LCD_RESET_GPIO);
    gpio_set_level(LCD_RESET_GPIO, 0);
    ESP_LOGI(TAG, "LCD reset GPIO%d after assert, readback=%d", LCD_RESET_GPIO, gpio_get_level(LCD_RESET_GPIO));
    vTaskDelay(pdMS_TO_TICKS(30));
    //vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "Release LCD reset on GPIO%d", LCD_RESET_GPIO);
    gpio_set_level(LCD_RESET_GPIO, 1);
    ESP_LOGI(TAG, "LCD reset GPIO%d after release, readback=%d", LCD_RESET_GPIO, gpio_get_level(LCD_RESET_GPIO));
    if (gpio_get_level(LCD_RESET_GPIO) == 0) {
        ESP_LOGW(TAG, "LCD reset GPIO%d is still low after release; check pin mapping or external pull-down", LCD_RESET_GPIO);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
}

static void lcd_enable_dsi_phy_power(void)
{
    static esp_ldo_channel_handle_t ldo_mipi_phy;
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = LCD_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = LCD_MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY LDO enabled");
}

static void lcd_tx_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *data, size_t len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd, data, len));
}

static void lcd_dsi_disable_command_ack(esp_lcd_dsi_bus_handle_t bus)
{
    mipi_dsi_host_ll_enable_cmd_ack(bus->hal.host, false);
    ESP_LOGI(TAG, "MIPI DSI command ACK disabled");
}

static void lcd_init_st7701sn(esp_lcd_panel_io_handle_t io)
{
    static const uint8_t cmd2_bk0[] = {0x77, 0x01, 0x00, 0x00, 0x10};
    static const uint8_t cmd2_bk1[] = {0x77, 0x01, 0x00, 0x00, 0x11};
    static const uint8_t cmd2_bk3[] = {0x77, 0x01, 0x00, 0x00, 0x13};
    static const uint8_t cmd2_bk_default[] = {0x77, 0x01, 0x00, 0x00, 0x00};

    const lcd_init_cmd_t init_cmds[] = {
        {LCD_CMD_SWRESET, NULL, 0, 120},
        {0xFF, cmd2_bk3, sizeof(cmd2_bk3), 0},
        {0xEF, (const uint8_t[]){0x08}, 1, 0},
        {0xFF, cmd2_bk0, sizeof(cmd2_bk0), 0},
        {0xC0, (const uint8_t[]){0x4F, 0x00}, 2, 0},
        {0xC1, (const uint8_t[]){0x11, 0x0C}, 2, 0},
        {0xC2, (const uint8_t[]){0x07, 0x0A}, 2, 0},
        {0xC3, (const uint8_t[]){0x83, 0x33, 0x1B}, 3, 0},
        {0xCC, (const uint8_t[]){0x10}, 1, 0},
        {0xB0, (const uint8_t[]){0x00, 0x0F, 0x18, 0x0D, 0x12, 0x07, 0x05, 0x08, 0x07, 0x21, 0x03, 0x10, 0x0F, 0x26, 0x2F, 0x1F}, 16, 0},
        {0xB1, (const uint8_t[]){0x00, 0x1B, 0x20, 0x0C, 0x0E, 0x03, 0x08, 0x08, 0x08, 0x22, 0x05, 0x11, 0x0F, 0x2A, 0x32, 0x1F}, 16, 0},
        {0xFF, cmd2_bk1, sizeof(cmd2_bk1), 0},
        {0xB0, (const uint8_t[]){0x35}, 1, 0},
        {0xB1, (const uint8_t[]){0x6A}, 1, 0},
        {0xB2, (const uint8_t[]){0x81}, 1, 0},
        {0xB3, (const uint8_t[]){0x80}, 1, 0},
        {0xB5, (const uint8_t[]){0x4E}, 1, 0},
        {0xB7, (const uint8_t[]){0x85}, 1, 0},
        {0xB8, (const uint8_t[]){0x21}, 1, 0},
        {0xC0, (const uint8_t[]){0x09}, 1, 0},
        {0xC1, (const uint8_t[]){0x78}, 1, 0},
        {0xC2, (const uint8_t[]){0x78}, 1, 0},
        {0xD0, (const uint8_t[]){0x88}, 1, 0},
        {0xE0, (const uint8_t[]){0x00, 0xA0, 0x02}, 3, 0},
        {0xE1, (const uint8_t[]){0x06, 0xA0, 0x08, 0xA0, 0x05, 0xA0, 0x07, 0xA0, 0x00, 0x44, 0x44}, 11, 0},
        {0xE2, (const uint8_t[]){0x20, 0x20, 0x40, 0x40, 0x96, 0xA0, 0x00, 0x00, 0x96, 0xA0, 0x00, 0x00, 0x00}, 13, 0},
        {0xE3, (const uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
        {0xE4, (const uint8_t[]){0x44, 0x44}, 2, 0},
        {0xE5, (const uint8_t[]){0x0E, 0x97, 0x10, 0xA0, 0x10, 0x99, 0x10, 0xA0, 0x0A, 0x93, 0x10, 0xA0, 0x0C, 0x95, 0x10, 0xA0}, 16, 0},
        {0xE6, (const uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
        {0xE7, (const uint8_t[]){0x44, 0x44}, 2, 0},
        {0xE8, (const uint8_t[]){0x0D, 0x96, 0x10, 0xA0, 0x0F, 0x98, 0x10, 0xA0, 0x09, 0x92, 0x10, 0xA0, 0x0B, 0x94, 0x10, 0xA0}, 16, 0},
        {0xEB, (const uint8_t[]){0x00, 0x01, 0x4E, 0x4E, 0x44, 0x88, 0x40}, 7, 0},
        {0xEC, (const uint8_t[]){0x78, 0x00}, 2, 0},
        {0xED, (const uint8_t[]){0xFF, 0xFA, 0x2F, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xF2, 0xAF, 0xFF}, 16, 0},
        {0xEF, (const uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
        {0xFF, cmd2_bk3, sizeof(cmd2_bk3), 0},
        {0xE8, (const uint8_t[]){0x00, 0x0E}, 2, 0},
        {0xE8, (const uint8_t[]){0x00, 0x0C}, 2, 20},
        {0xE8, (const uint8_t[]){0x00, 0x00}, 2, 0},
        {0xFF, cmd2_bk_default, sizeof(cmd2_bk_default), 0},
        {LCD_CMD_SLPOUT, NULL, 0, 120},
        {LCD_CMD_COLMOD, (const uint8_t[]){0x50}, 1, 0},
        {LCD_CMD_DISPON, NULL, 0, 20},
    };

    ESP_LOGI(TAG, "Send ST7701SN init commands");
    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        ESP_LOGI(TAG, "LCD init cmd[%u]: 0x%02X, len=%u", (unsigned)i, init_cmds[i].cmd, (unsigned)init_cmds[i].data_len);
        lcd_tx_cmd(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_len);
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].data_len > 4 ? 20 : 10));
        if (init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
        }
    }
}

esp_lcd_panel_handle_t lcd_driver_init(void)
{
    //lcd_reset_init_and_release();
     vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Initialize ST7701SN 480x640 MIPI display with ESP-IDF DSI flow");
    ESP_LOGI(TAG,
             "MIPI DSI config: lanes=%d lane_bitrate=%dMbps aggregate=%dMbps dpi_clk=%dMHz color=RGB565",
             LCD_MIPI_DSI_LANE_NUM,
             LCD_MIPI_DSI_LANE_BITRATE_MBPS,
             LCD_MIPI_DSI_LANE_NUM * LCD_MIPI_DSI_LANE_BITRATE_MBPS,
             LCD_MIPI_DPI_CLK_MHZ);

    lcd_backlight_init();
    lcd_driver_set_backlight(0);
    lcd_enable_dsi_phy_power();

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_LOGI(TAG, "Create MIPI DSI bus");
    lcd_check_mipi_step("create DSI bus", esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));
    ESP_LOGI(TAG, "MIPI DSI bus handle=%p", mipi_dsi_bus);

    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_LOGI(TAG, "Create MIPI DBI command IO");
    lcd_check_mipi_step("create DBI command channel", esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));
    ESP_LOGI(TAG, "MIPI DBI IO handle=%p vc=%d cmd_bits=%d param_bits=%d",
             mipi_dbi_io,
             dbi_config.virtual_channel,
             dbi_config.lcd_cmd_bits,
             dbi_config.lcd_param_bits);
    lcd_dsi_disable_command_ack(mipi_dsi_bus);

    esp_lcd_panel_handle_t dpi_panel = NULL;
    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_MIPI_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = 10,
            .hsync_pulse_width = 2,
            .hsync_front_porch = 10,
            .vsync_back_porch = 30,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 30,
        },
        .num_fbs = 1,
        .flags = {
            .disable_lp = 1,
        },
    };
    ESP_LOGI(TAG,
             "MIPI DPI timing: %ux%u hbp=%u hpw=%u hfp=%u vbp=%u vpw=%u vfp=%u fb=%u disable_lp=%u",
             (unsigned)dpi_config.video_timing.h_size,
             (unsigned)dpi_config.video_timing.v_size,
             (unsigned)dpi_config.video_timing.hsync_back_porch,
             (unsigned)dpi_config.video_timing.hsync_pulse_width,
             (unsigned)dpi_config.video_timing.hsync_front_porch,
             (unsigned)dpi_config.video_timing.vsync_back_porch,
             (unsigned)dpi_config.video_timing.vsync_pulse_width,
             (unsigned)dpi_config.video_timing.vsync_front_porch,
             (unsigned)dpi_config.num_fbs,
             (unsigned)dpi_config.flags.disable_lp);
    ESP_LOGI(TAG, "Create MIPI DPI panel");
    lcd_check_mipi_step("create DPI video panel", esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &dpi_panel));
    ESP_LOGI(TAG, "MIPI DPI panel handle=%p", dpi_panel);

    lcd_reset_init_and_release();
    lcd_init_st7701sn(mipi_dbi_io);

    ESP_LOGI(TAG, "Start MIPI DPI panel init");
    lcd_check_mipi_step("init DPI panel and start video stream", esp_lcd_panel_init(dpi_panel));
    ESP_LOGI(TAG, "MIPI DPI panel init done, video stream should be enabled");

#if LCD_USE_DSI_TEST_PATTERN
    ESP_LOGI(TAG, "Show MIPI DSI vertical color bar test pattern");
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(dpi_panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    lcd_driver_set_backlight(80);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    lcd_driver_set_backlight(80);
    ESP_LOGI(TAG, "LCD backlight enabled");

    return dpi_panel;
}
