#pragma once

// ============================================================================
// DSH-Board 固件骨架 —— 板级配置
//
// 重要：下面的 I2S/I2C/按键引脚是占位值，必须按你手上板子的原理图修改。
// 微雪 ESP32-S3-AUDIO-Board 使用 ES7210（麦克风 ADC）+ ES8311（codec），
// 引脚以微雪 wiki / 原理图为准。改完把 BOARD_AUDIO_ENABLE 设为 1。
// ============================================================================

#define FIRMWARE_VERSION   "0.1.0"
#define BOARD_MODEL        "esp32s3-audio"
#define PROTO_VERSION      1
#define HTTP_PORT          8080

#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_FRAME_MS     20
#define AUDIO_FRAME_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 320
#define AUDIO_FRAME_BYTES   (AUDIO_FRAME_SAMPLES * 2)                     // 640

// 1 = 初始化 I2S + ES8311/ES7210（默认，上板验证音频）；0 = 只跑网络/协议，音频静音。
#define BOARD_AUDIO_ENABLE 1

// 微雪 ESP32-S3-AUDIO-Board 引脚（来自板级资料/社区验证）：
// I2S: MCLK=12 BCLK=13 LRCLK=14 DIN=15 DOUT=16
// I2C: SDA=11 SCL=10；ES8311@0x18，ES7210@0x40，TCA9555@0x20
// LED: GPIO38；BOOT: GPIO0；功放使能：TCA9555 EXIO8
#define I2S_MCLK_GPIO       12
#define I2S_BCLK_GPIO       13
#define I2S_LRCK_GPIO       14
#define I2S_DOUT_GPIO       16
#define I2S_DIN_GPIO        15
#define I2C_SCL_GPIO        10
#define I2C_SDA_GPIO        11
#define BOOT_BUTTON_GPIO    0
#define LED_GPIO            38

#define ES8311_I2C_ADDR     0x18
#define ES7210_I2C_ADDR     0x40
#define TCA9555_I2C_ADDR    0x20

// 配网 AP 名前缀，后面拼设备 ID 后 4 位。
#define BOARD_AP_PREFIX     "DSH-Board-"

// 只读 factory 分区：存 id / code_hash / ap_pass，用户重置不清除。
#define FACTORY_NVS_PARTITION "factory_nvs"

// 开发默认值。量产必须由产线写入每台唯一的配对码与 AP 密码。
#define DEV_DEFAULT_PAIR_CODE "123456"

#define PAIR_MAX_FAILS      5
#define PAIR_LOCK_MS        60000
