#include "audio_i2s.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "board_config.h"
#include "codec_init.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#if BOARD_AUDIO_ENABLE
static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
#endif

void audio_init(void)
{
#if BOARD_AUDIO_ENABLE
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (I2S_MCLK_GPIO < 0) ? I2S_GPIO_UNUSED : I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
#endif

    // I2S MCLK 起来之后再配置 codec。
    codec_init();
    if (codec_present_tca9555()) {
        codec_speaker_enable(true);
    }

#if BOARD_AUDIO_ENABLE
    ESP_LOGI(TAG, "I2S + codec ready (ES8311=%d ES7210=%d)",
             codec_present_es8311(), codec_present_es7210());
#else
    ESP_LOGW(TAG, "BOARD_AUDIO_ENABLE=0：只探测 codec，音频走静音/回环");
#endif
}

esp_err_t audio_read(uint8_t *buf, size_t len, size_t *out_len, uint32_t timeout_ms)
{
    if (out_len) *out_len = 0;
#if BOARD_AUDIO_ENABLE
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx, buf, len, &bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK && out_len) *out_len = bytes_read;
    return err;
#else
    memset(buf, 0, len);
    vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
    if (out_len) *out_len = len;
    return ESP_OK;
#endif
}

esp_err_t audio_write(const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
#if BOARD_AUDIO_ENABLE
    size_t written = 0;
    return i2s_channel_write(s_tx, buf, len, &written, pdMS_TO_TICKS(timeout_ms));
#else
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return ESP_OK;
#endif
}

void audio_play_tone(uint32_t freq_hz, uint32_t ms)
{
    int16_t frame[AUDIO_FRAME_SAMPLES];
    const uint32_t total = (AUDIO_SAMPLE_RATE * ms) / 1000;
    uint32_t done = 0;
    uint32_t phase = 0;

    while (done < total) {
        uint32_t count = total - done;
        if (count > AUDIO_FRAME_SAMPLES) count = AUDIO_FRAME_SAMPLES;
        for (uint32_t i = 0; i < count; i++) {
            frame[i] = (int16_t)(sinf(2.0f * (float)M_PI * (float)freq_hz * (float)phase / AUDIO_SAMPLE_RATE) * 8000);
            phase++;
        }
        audio_write((const uint8_t *)frame, count * sizeof(int16_t), 100);
        done += count;
    }
}

void audio_test(audio_test_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

#if BOARD_AUDIO_ENABLE
    audio_play_tone(1000, 300);
    out->speaker_ok = true;

    uint8_t frame[AUDIO_FRAME_BYTES];
    double sum = 0.0;
    size_t total = 0;
    for (int i = 0; i < 15; i++) {
        size_t got = 0;
        if (audio_read(frame, sizeof(frame), &got, 100) == ESP_OK && got >= 2) {
            const int16_t *samples = (const int16_t *)frame;
            size_t count = got / 2;
            for (size_t j = 0; j < count; j++) {
                double value = (double)samples[j] / 32768.0;
                sum += value * value;
                total++;
            }
        }
    }
    if (total > 0) {
        out->mic_rms = (float)sqrt(sum / (double)total);
        out->mic_ok = out->mic_rms > 0.01f;
    }
#else
    out->speaker_ok = false;
    out->mic_ok = false;
#endif
}
