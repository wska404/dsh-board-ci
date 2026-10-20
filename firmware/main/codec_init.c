#include "codec_init.h"

#include <stdint.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "codec";

static bool s_es8311 = false;
static bool s_es7210 = false;
static bool s_tca9555 = false;

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

static esp_err_t i2c_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, value, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

// ---------------------------------------------------------------------------
// ES8311 (DAC / speaker)
// 16kHz, 16-bit, MCLK = 256 * fs = 4.096MHz
// ---------------------------------------------------------------------------

static esp_err_t es8311_write(uint8_t reg, uint8_t value)
{
    return i2c_write(ES8311_I2C_ADDR, reg, value);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *value)
{
    return i2c_read(ES8311_I2C_ADDR, reg, value);
}

static esp_err_t es8311_init(void)
{
    esp_err_t err = ESP_OK;

    // Reset
    err |= es8311_write(0x00, 0x1F);
    err |= es8311_write(0x00, 0x00);

    // Clock manager: MCLK from pin, all clocks on
    err |= es8311_write(0x01, 0x3F);

    // pre_div = 1, pre_mult = 1 (keep low 3 bits)
    uint8_t reg02 = 0;
    err |= es8311_read(0x02, &reg02);
    reg02 &= 0x07;
    err |= es8311_write(0x02, reg02);

    // adc_osr / dac_osr / adc_div=1 / dac_div=1
    err |= es8311_write(0x03, 0x10);
    err |= es8311_write(0x04, 0x20);
    err |= es8311_write(0x05, 0x00);

    // bclk_div = 4 -> value 3
    uint8_t reg06 = 0;
    err |= es8311_read(0x06, &reg06);
    reg06 &= (uint8_t)~0x20;
    reg06 &= 0xE0;
    reg06 |= 0x03;
    err |= es8311_write(0x06, reg06);

    // lrck_h = 0, lrck_l = 0xFF
    uint8_t reg07 = 0;
    err |= es8311_read(0x07, &reg07);
    reg07 &= 0xC0;
    err |= es8311_write(0x07, reg07);
    err |= es8311_write(0x08, 0xFF);

    // Clear bit6, then 16-bit in/out
    uint8_t reg00 = 0;
    err |= es8311_read(0x00, &reg00);
    reg00 &= 0xBF;
    err |= es8311_write(0x00, reg00);
    err |= es8311_write(0x09, 0x0C);
    err |= es8311_write(0x0A, 0x0C);

    // Analog mic / PGA (ES8311's own ADC is unused, but keep it sane)
    err |= es8311_write(0x14, 0x1A);
    err |= es8311_write(0x16, 0x00);
    err |= es8311_write(0x17, 0xC8);

    // DAC volume 0xBF = 0dB
    err |= es8311_write(0x32, 0xBF);

    // Power up
    err |= es8311_write(0x0D, 0x01);
    err |= es8311_write(0x0E, 0x02);
    err |= es8311_write(0x12, 0x00);
    err |= es8311_write(0x13, 0x10);
    err |= es8311_write(0x1C, 0x6A);
    err |= es8311_write(0x37, 0x08);
    err |= es8311_write(0x00, 0x80);

    return err;
}

// ---------------------------------------------------------------------------
// ES7210 (ADC / dual microphones)
// 16kHz, 16-bit, MCLK = 4.096MHz, non-TDM (mic1/2 on SDOUT1)
// ---------------------------------------------------------------------------

static esp_err_t es7210_write(uint8_t reg, uint8_t value)
{
    return i2c_write(ES7210_I2C_ADDR, reg, value);
}

static esp_err_t es7210_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t err = i2c_read(ES7210_I2C_ADDR, reg, &current);
    if (err != ESP_OK) return err;
    current = (uint8_t)((current & ~mask) | (mask & value));
    return es7210_write(reg, current);
}

static uint8_t es7210_gain_reg(float gain_db)
{
    // Datasheet: 0x00..0x0B = 0..33dB step 3dB; 0x0C=34.5, 0x0D=36, 0x0E=37.5
    gain_db += 0.5f;
    if (gain_db <= 33.0f) return (uint8_t)(gain_db / 3.0f);
    if (gain_db < 36.0f) return 0x0C;
    if (gain_db < 37.0f) return 0x0D;
    return 0x0E;
}

static esp_err_t es7210_init(void)
{
    esp_err_t err = ESP_OK;

    // Reset + clock off
    err |= es7210_write(0x00, 0xFF);
    err |= es7210_write(0x00, 0x32);
    err |= es7210_write(0x01, 0x3F);

    // Power-up timing
    err |= es7210_write(0x09, 0x30);
    err |= es7210_write(0x0A, 0x30);

    // HPF for ADC1..4
    err |= es7210_write(0x23, 0x2A);
    err |= es7210_write(0x22, 0x0A);
    err |= es7210_write(0x20, 0x0A);
    err |= es7210_write(0x21, 0x2A);

    // Primary I2S mode
    err |= es7210_update(0x08, 0x01, 0x00);

    // Analog power + mic bias
    err |= es7210_write(0x40, 0xC3);
    err |= es7210_write(0x41, 0x70);
    err |= es7210_write(0x42, 0x70);

    // 16-bit, 非 TDM（mic1/2 输出到 SDOUT1）。
    // 如果上板后麦克风 RMS 一直为 0，改成 TDM：reg12=0x02，
    // I2S 配 4 slots，mic 在 slot 0/2（参考微雪/ESPHome 的 TDM 配置）。
    err |= es7210_write(0x11, 0x60);
    err |= es7210_write(0x12, 0x00);

    // 4.096MHz / 16kHz coefficient:
    // adc_div=1, doubler=1, dll=1 -> 0xC1; osr=0x20; lrck=0x0100
    err |= es7210_write(0x02, 0xC1);
    err |= es7210_write(0x07, 0x20);
    err |= es7210_write(0x04, 0x01);
    err |= es7210_write(0x05, 0x00);

    // Microphone gain: 24dB
    const uint8_t gain = es7210_gain_reg(24.0f);
    for (int i = 0; i < 4; i++) {
        err |= es7210_update(0x43 + i, 0x10, 0x00);
    }
    err |= es7210_write(0x4B, 0xFF);
    err |= es7210_write(0x4C, 0xFF);

    for (int i = 0; i < 4; i++) {
        const uint8_t clock_mask = (i < 2) ? 0x0B : 0x15;
        const uint8_t power_reg = (i < 2) ? 0x4B : 0x4C;
        err |= es7210_update(0x01, clock_mask, 0x00);
        err |= es7210_write(power_reg, 0x00);
        err |= es7210_update(0x43 + i, 0x10, 0x10);
        err |= es7210_update(0x43 + i, 0x0F, gain);
    }

    // Power on mics / ADC / PGA
    err |= es7210_write(0x47, 0x08);
    err |= es7210_write(0x48, 0x08);
    err |= es7210_write(0x49, 0x08);
    err |= es7210_write(0x4A, 0x08);
    err |= es7210_write(0x06, 0x04);
    err |= es7210_write(0x4B, 0x0F);
    err |= es7210_write(0x4C, 0x0F);

    // Enable device
    err |= es7210_write(0x00, 0x71);
    err |= es7210_write(0x00, 0x41);

    return err;
}

// ---------------------------------------------------------------------------
// TCA9555 (speaker enable)
// ---------------------------------------------------------------------------

static esp_err_t tca9555_init(void)
{
    esp_err_t err = ESP_OK;
    // port0 all input; port1 bit0=EXIO8 output, rest input
    err |= i2c_write(TCA9555_I2C_ADDR, 0x06, 0xFF);
    err |= i2c_write(TCA9555_I2C_ADDR, 0x07, 0xFE);
    err |= i2c_write(TCA9555_I2C_ADDR, 0x02, 0x00);
    err |= i2c_write(TCA9555_I2C_ADDR, 0x03, 0x01);
    return err;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void codec_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    s_es8311 = (i2c_probe(ES8311_I2C_ADDR) == ESP_OK);
    s_es7210 = (i2c_probe(ES7210_I2C_ADDR) == ESP_OK);
    s_tca9555 = (i2c_probe(TCA9555_I2C_ADDR) == ESP_OK);

    ESP_LOGI(TAG, "probe ES8311=%s ES7210=%s TCA9555=%s",
             s_es8311 ? "yes" : "no", s_es7210 ? "yes" : "no", s_tca9555 ? "yes" : "no");

    if (s_es8311) {
        esp_err_t err = es8311_init();
        ESP_LOGI(TAG, "ES8311 init: %s", esp_err_to_name(err));
    }
    if (s_es7210) {
        esp_err_t err = es7210_init();
        ESP_LOGI(TAG, "ES7210 init: %s", esp_err_to_name(err));
    }
    if (s_tca9555) {
        esp_err_t err = tca9555_init();
        ESP_LOGI(TAG, "TCA9555 init: %s", esp_err_to_name(err));
    }

    codec_set_volume(70);
}

bool codec_present_es8311(void)
{
    return s_es8311;
}

bool codec_present_es7210(void)
{
    return s_es7210;
}

bool codec_present_tca9555(void)
{
    return s_tca9555;
}

void codec_speaker_enable(bool on)
{
    if (!s_tca9555) return;
    i2c_write(TCA9555_I2C_ADDR, 0x03, on ? 0x01 : 0x00);
}

void codec_set_volume(int percent)
{
    if (!s_es8311) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t value = (uint8_t)((percent * 255) / 100);
    i2c_write(ES8311_I2C_ADDR, 0x32, value);
}
