#include "ws_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "board_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pairing.h"

static const char *TAG = "ws";

static httpd_handle_t s_server = NULL;
static int s_fd = -1;
static volatile bool s_active = false;
static volatile bool s_session = false;
static SemaphoreHandle_t s_lock = NULL;

typedef struct {
    uint8_t *data;
    size_t len;
} spk_item_t;

static QueueHandle_t s_spk_queue = NULL;

void ws_audio_attach(httpd_handle_t server, int fd)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_server = server;
    s_fd = fd;
    s_active = true;
    s_session = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "客户端接入 fd=%d", fd);
}

void ws_audio_detach(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active = false;
    s_session = false;
    s_fd = -1;
    xSemaphoreGive(s_lock);
}

bool ws_audio_is_active(void)
{
    return s_active;
}

void ws_audio_close_session(void)
{
    if (s_active && s_server != NULL && s_fd >= 0) {
        httpd_ws_frame_t close = {.type = HTTPD_WS_TYPE_CLOSE};
        httpd_ws_send_frame_async(s_server, s_fd, &close);
    }
    ws_audio_detach();
}

esp_err_t ws_audio_send_json(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_active) return ESP_ERR_INVALID_STATE;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    return httpd_ws_send_frame_async(s_server, s_fd, &frame);
}

esp_err_t ws_audio_send_pcm(const uint8_t *pcm, size_t len)
{
    if (!pcm || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_active) return ESP_ERR_INVALID_STATE;

    uint8_t *payload = malloc(len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    payload[0] = 0x02;  // 下行喇叭
    memcpy(payload + 1, pcm, len);

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = payload,
        .len = len + 1,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, s_fd, &frame);
    // 生产建议改成 TX 队列，确保异步发送完成后再释放 payload。
    free(payload);
    return err;
}

esp_err_t ws_audio_send_hello(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", pairing_id());
    cJSON_AddNumberToObject(root, "proto", PROTO_VERSION);
    cJSON_AddStringToObject(root, "fw", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "session_id", "s-1");

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "codec", "pcm_s16le");
    cJSON_AddNumberToObject(audio, "rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_ms", AUDIO_FRAME_MS);

    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    cJSON_AddItemToArray(caps, cJSON_CreateString("pcm16"));

    char *text = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_FAIL;
    if (text) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)text,
            .len = strlen(text),
        };
        err = httpd_ws_send_frame(req, &frame);
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return err;
}

void ws_audio_on_text(const char *data, size_t len)
{
    if (!data || len == 0) return;

    cJSON *root = cJSON_Parse(data);
    if (!root) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "session.start") == 0) {
            s_session = true;
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
            ESP_LOGI(TAG, "会话开始 mode=%s", cJSON_IsString(mode) ? mode->valuestring : "push_to_talk");
        } else if (strcmp(type->valuestring, "session.stop") == 0) {
            s_session = false;
            ESP_LOGI(TAG, "会话结束");
        } else if (strcmp(type->valuestring, "abort") == 0) {
            if (s_spk_queue) xQueueReset(s_spk_queue);
            ws_audio_send_json("{\"type\":\"aborted\"}");
        } else if (strcmp(type->valuestring, "speak.start") == 0) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            ESP_LOGI(TAG, "开始播报 %s", cJSON_IsString(id) ? id->valuestring : "");
        } else if (strcmp(type->valuestring, "ping") == 0) {
            char pong[64];
            snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"ts\":%lld}",
                     (long long)(esp_timer_get_time() / 1000));
            ws_audio_send_json(pong);
        }
    }
    cJSON_Delete(root);
}

void ws_audio_on_binary(const uint8_t *data, size_t len)
{
    if (!data || len < 1) return;
    uint8_t type = data[0];

    if (type == 0x02 && len > 1) {
        // 下行 PCM：拷进队列，由喇叭任务消费。
        spk_item_t item = {.data = malloc(len - 1), .len = len - 1};
        if (!item.data) return;
        memcpy(item.data, data + 1, len - 1);
        if (!s_spk_queue || xQueueSend(s_spk_queue, &item, 0) != pdTRUE) {
            free(item.data);
        }
    } else if (type == 0x04) {
        // 测试音：插件自检用。
        audio_play_tone(1000, 300);
    }
}

static void mic_task(void *arg)
{
    (void)arg;
    uint8_t frame[AUDIO_FRAME_BYTES];
    while (1) {
        if (s_active && s_session) {
            size_t got = 0;
            if (audio_read(frame, sizeof(frame), &got, 100) == ESP_OK && got > 0) {
                ws_audio_send_pcm(frame, got);
            } else {
                vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
        }
    }
}

static void speaker_task(void *arg)
{
    (void)arg;
    spk_item_t item;
    while (1) {
        if (xQueueReceive(s_spk_queue, &item, pdMS_TO_TICKS(20)) == pdTRUE) {
            audio_write(item.data, item.len, 100);
            free(item.data);
        }
    }
}

void audio_tasks_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_spk_queue) s_spk_queue = xQueueCreate(16, sizeof(spk_item_t));

    xTaskCreatePinnedToCore(mic_task, "mic_tx", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(speaker_task, "spk_rx", 4096, NULL, 5, NULL, 1);
}
