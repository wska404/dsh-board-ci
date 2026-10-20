#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_mdns.h"
#include "audio_i2s.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mdns.h"
#include "pairing.h"
#include "wifi_prov.h"
#include "ws_audio.h"

static const char *TAG = "main";
static bool s_mdns_started = false;

static void start_mdns(void)
{
    if (s_mdns_started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS 初始化失败");
        return;
    }

    char hostname[32];
    snprintf(hostname, sizeof(hostname), "dsh-board-%s", pairing_id() + 8);
    for (char *p = hostname; *p; p++) *p = (char)tolower((unsigned char)*p);

    mdns_hostname_set(hostname);
    mdns_instance_name_set(pairing_name());

    mdns_txt_item_t txt[] = {
        {"id", pairing_id()},
        {"name", pairing_name()},
        {"model", BOARD_MODEL},
        {"fw", FIRMWARE_VERSION},
        {"proto", "1"},
        {"pair", pairing_is_paired() ? "paired" : "required"},
        {"caps", "pcm16"},
        {"ws", "/ws"},
        {"api", "/api"},
    };

    esp_err_t err = mdns_service_add(NULL, "_dsh-board", "_tcp", HTTP_PORT, txt,
                                     sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS 服务注册失败: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS 已广播 %s.local", hostname);
}

static void on_wifi_connected(void)
{
    char ip[32];
    wifi_prov_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Wi-Fi 已连接，IP=%s", ip);
    start_mdns();
}

void app_mdns_refresh(void)
{
    if (!s_mdns_started) return;
    mdns_txt_item_t txt[] = {
        {"id", pairing_id()},
        {"name", pairing_name()},
        {"model", BOARD_MODEL},
        {"fw", FIRMWARE_VERSION},
        {"proto", "1"},
        {"pair", pairing_is_paired() ? "paired" : "required"},
        {"caps", "pcm16"},
        {"ws", "/ws"},
        {"api", "/api"},
    };
    mdns_service_txt_set("_dsh-board", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
}

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    bool token_cleared = false;

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 5000 && !token_cleared) {
                pairing_clear_token();
                ws_audio_close_session();
                token_cleared = true;
                ESP_LOGW(TAG, "长按 5s：已解除配对；继续按到 10s 会清 Wi-Fi 并重启");
            }
            if (held_ms >= 10000) {
                wifi_prov_forget_credentials();
                ESP_LOGW(TAG, "长按 10s：清 Wi-Fi，重启进配网模式");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            held_ms = 0;
            token_cleared = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DSH-Board v%s 启动", FIRMWARE_VERSION);

    pairing_init();
    wifi_prov_init(on_wifi_connected);
    http_api_start();
    audio_init();
    audio_tasks_start();
    wifi_prov_start();

    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);
}
