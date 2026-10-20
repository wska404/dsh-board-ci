#pragma once

#include <stdbool.h>

/** 初始化 I2C，配置 ES8311 / ES7210 / TCA9555，并使能功放。 */
void codec_init(void);

bool codec_present_es8311(void);
bool codec_present_es7210(void);
bool codec_present_tca9555(void);

/** TCA9555 EXIO8 控制 NS4150 功放使能。 */
void codec_speaker_enable(bool on);

/** ES8311 DAC 音量，0–100。 */
void codec_set_volume(int percent);
