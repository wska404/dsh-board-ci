#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** 初始化 I2S（BOARD_AUDIO_ENABLE=0 时只做静音/丢弃）。 */
void audio_init(void);

/** 读一帧 PCM（16k/16bit/单声道）。 */
esp_err_t audio_read(uint8_t *buf, size_t len, size_t *out_len, uint32_t timeout_ms);

/** 写一帧 PCM。 */
esp_err_t audio_write(const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** 播放测试音（阻塞，最长 ms 毫秒）。 */
void audio_play_tone(uint32_t freq_hz, uint32_t ms);

typedef struct {
    bool speaker_ok;
    bool mic_ok;
    float mic_rms;
} audio_test_result_t;

/** 自检：播放测试音 + 录一段麦克风，返回结果。 */
void audio_test(audio_test_result_t *out);
