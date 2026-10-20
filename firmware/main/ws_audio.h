#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** 发送 hello（在 WS 握手 handler 内调用）。 */
esp_err_t ws_audio_send_hello(httpd_req_t *req);

void ws_audio_attach(httpd_handle_t server, int fd);
void ws_audio_detach(void);
bool ws_audio_is_active(void);
void ws_audio_close_session(void);

esp_err_t ws_audio_send_json(const char *json);
esp_err_t ws_audio_send_pcm(const uint8_t *pcm, size_t len);

void ws_audio_on_binary(const uint8_t *data, size_t len);
void ws_audio_on_text(const char *data, size_t len);

/** 创建麦克风上行 / 喇叭下行任务。 */
void audio_tasks_start(void);
