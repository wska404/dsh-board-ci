#include "http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_mdns.h"
#include "audio_i2s.h"
#include "board_config.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pairing.h"
#include "wifi_prov.h"
#include "ws_audio.h"

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

static int s_volume = 70;
static bool s_muted = false;

static int s_pair_fails = 0;
static int64_t s_pair_lock_until = 0;

static const char *SETUP_HTML =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>DSH-Board 配网</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:2rem auto;max-width:28rem;padding:0 1rem}"
    "input,button{width:100%%;padding:.7rem;margin:.4rem 0;font-size:1rem;box-sizing:border-box}"
    "button{background:#1f4e79;color:#fff;border:0;border-radius:.4rem}"
    "label{font-size:.9rem;color:#444}</style></head><body>"
    "<h2>DSH-Board 配网</h2><p>设备：%s</p>"
    "<label>Wi-Fi 名称（仅 2.4G）</label><input id=\"ssid\">"
    "<label>密码</label><input id=\"pass\" type=\"password\">"
    "<button onclick=\"save()\">连接</button><p id=\"msg\"></p>"
    "<script>async function save(){const ssid=document.getElementById('ssid').value.trim();"
    "const password=document.getElementById('pass').value;"
    "if(!ssid){document.getElementById('msg').textContent='请填写 Wi-Fi 名称';return}"
    "document.getElementById('msg').textContent='保存中…';"
    "const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid,password})});"
    "const j=await r.json();"
    "document.getElementById('msg').textContent=j.ok?'已保存，板子重启中…':'失败：'+((j.error&&j.error.message)||r.status)}"
    "</script></body></html>";

static esp_err_t send_json_status(httpd_req_t *req, const char *status, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body ? body : "{}", body ? strlen(body) : 2);
    if (body) cJSON_free(body);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return send_json_status(req, status, root);
}

static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= len) return -1;

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) return -1;
        received += ret;
    }
    buf[total] = '\0';
    return total;
}

static bool get_token(httpd_req_t *req, char *out, size_t out_len)
{
    char header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        const char *p = header;
        if (strncmp(p, "Bearer ", 7) == 0) p += 7;
        strlcpy(out, p, out_len);
        return out[0] != '\0';
    }

    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[80];
        if (httpd_query_key_value(query, "token", value, sizeof(value)) == ESP_OK) {
            strlcpy(out, value, out_len);
            return true;
        }
    }
    return false;
}

static bool require_auth(httpd_req_t *req)
{
    char token[96];
    if (!get_token(req, token, sizeof(token)) || !pairing_verify_token(token)) {
        send_error(req, "401 Unauthorized", "UNAUTHORIZED", "token 无效");
        return false;
    }
    return true;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", pairing_id());
    cJSON_AddStringToObject(root, "name", pairing_name());
    cJSON_AddStringToObject(root, "model", BOARD_MODEL);
    cJSON_AddStringToObject(root, "fw", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "proto", PROTO_VERSION);
    cJSON_AddStringToObject(root, "pairing",
                            pairing_is_paired() ? "paired"
                                                : (wifi_prov_is_ap_mode() ? "unprovisioned" : "required"));

    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    cJSON_AddItemToArray(caps, cJSON_CreateString("pcm16"));

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "codec", "pcm_s16le");
    cJSON_AddNumberToObject(audio, "rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_ms", AUDIO_FRAME_MS);

    char ip[32];
    wifi_prov_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return send_json_status(req, "200 OK", root);
}

static esp_err_t pair_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_pair_lock_until) {
        cJSON *root = cJSON_CreateObject();
        cJSON *error = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(error, "code", "PAIR_LOCKED");
        cJSON_AddStringToObject(error, "message", "配对已锁定");
        cJSON_AddNumberToObject(error, "retry_after_ms", (double)(s_pair_lock_until - now));
        return send_json_status(req, "423 Locked", root);
    }

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "请求体为空");
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *code = root ? cJSON_GetObjectItemCaseSensitive(root, "code") : NULL;
    if (!cJSON_IsString(code) || strlen(code->valuestring) != 6) {
        if (root) cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "code 必须是 6 位数字");
    }

    bool valid = pairing_verify_code(code->valuestring);
    cJSON_Delete(root);

    if (!valid) {
        s_pair_fails++;
        if (s_pair_fails >= PAIR_MAX_FAILS) {
            s_pair_fails = 0;
            s_pair_lock_until = now + PAIR_LOCK_MS;
            cJSON *locked = cJSON_CreateObject();
            cJSON *error = cJSON_AddObjectToObject(locked, "error");
            cJSON_AddStringToObject(error, "code", "PAIR_LOCKED");
            cJSON_AddStringToObject(error, "message", "错误次数过多，稍后再试");
            cJSON_AddNumberToObject(error, "retry_after_ms", PAIR_LOCK_MS);
            return send_json_status(req, "423 Locked", locked);
        }
        return send_error(req, "401 Unauthorized", "PAIR_CODE_INVALID", "配对码错误");
    }

    s_pair_fails = 0;
    bool rotated = pairing_is_paired();
    if (rotated) ws_audio_close_session();

    char token[96];
    if (!pairing_issue_token(token, sizeof(token))) {
        return send_error(req, "500 Internal Server Error", "INTERNAL", "签发 token 失败");
    }
    app_mdns_refresh();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "token", token);
    cJSON_AddStringToObject(response, "device_id", pairing_id());
    cJSON_AddNumberToObject(response, "proto", PROTO_VERSION);
    cJSON_AddStringToObject(response, "name", pairing_name());
    cJSON_AddBoolToObject(response, "rotated", rotated);
    return send_json_status(req, "200 OK", response);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_FAIL;

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "paired", pairing_is_paired());
    cJSON_AddStringToObject(root, "session", ws_audio_is_active() ? "active" : "idle");
    cJSON_AddNumberToObject(root, "volume", s_volume);
    cJSON_AddBoolToObject(root, "muted", s_muted);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddNumberToObject(wifi, "rssi", rssi);
    cJSON_AddStringToObject(root, "fw", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return send_json_status(req, "200 OK", root);
}

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_FAIL;

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "请求体为空");
    }

    cJSON *root = cJSON_Parse(body);
    if (root) {
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
        if (cJSON_IsNumber(volume)) {
            int value = volume->valueint;
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            s_volume = value;
        }
        const cJSON *muted = cJSON_GetObjectItemCaseSensitive(root, "muted");
        if (cJSON_IsBool(muted)) s_muted = cJSON_IsTrue(muted);

        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(name) && name->valuestring[0]) {
            pairing_set_name(name->valuestring);
            app_mdns_refresh();
        }
        cJSON_Delete(root);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "volume", s_volume);
    cJSON_AddBoolToObject(response, "muted", s_muted);
    cJSON_AddStringToObject(response, "name", pairing_name());
    return send_json_status(req, "200 OK", response);
}

static esp_err_t unpair_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_FAIL;
    pairing_clear_token();
    ws_audio_close_session();
    app_mdns_refresh();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_status(req, "200 OK", root);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    send_json_status(req, "200 OK", root);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    if (!wifi_prov_is_ap_mode()) {
        return send_error(req, "409 Conflict", "BUSY", "板子已配网，请先重置");
    }

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "请求体为空");
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *ssid = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;
    const cJSON *password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    bool ok = cJSON_IsString(ssid) && ssid->valuestring[0] &&
              wifi_prov_save_credentials(ssid->valuestring,
                                         cJSON_IsString(password) ? password->valuestring : "");
    if (root) cJSON_Delete(root);

    if (!ok) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "ssid 不能为空");
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    send_json_status(req, "200 OK", response);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!wifi_prov_is_ap_mode()) {
        return send_error(req, "409 Conflict", "BUSY", "仅在配网模式可用");
    }

    char json[2048];
    if (wifi_prov_scan_json(json, sizeof(json)) != 0) {
        return send_error(req, "500 Internal Server Error", "INTERNAL", "扫描失败");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t root_handler(httpd_req_t *req)
{
    char html[4096];
    snprintf(html, sizeof(html), SETUP_HTML, pairing_name());
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t test_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_FAIL;

    audio_test_result_t result;
    audio_test(&result);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "speaker_ok", result.speaker_ok);
    cJSON_AddBoolToObject(root, "mic_ok", result.mic_ok);
    cJSON_AddNumberToObject(root, "mic_rms", result.mic_rms);
    return send_json_status(req, "200 OK", root);
}

static esp_err_t ota_manifest_handler(httpd_req_t *req)
{
    // 骨架：无更新时返回 204；有固件时返回 version/url/sha256/size。
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!require_auth(req)) return ESP_FAIL;
        ws_audio_attach(s_server, httpd_req_to_sockfd(req));
        return ws_audio_send_hello(req);
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_audio_detach();
        return ESP_OK;
    }
    if (frame.len == 0) return ESP_OK;

    uint8_t *buffer = calloc(1, frame.len + 1);
    if (!buffer) return ESP_ERR_NO_MEM;

    frame.payload = buffer;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        if (frame.type == HTTPD_WS_TYPE_BINARY) {
            ws_audio_on_binary(buffer, frame.len);
        } else if (frame.type == HTTPD_WS_TYPE_TEXT) {
            ws_audio_on_text((const char *)buffer, frame.len);
        } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
            ws_audio_detach();
        }
    }
    free(buffer);
    return ret;
}

void http_api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败");
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_handler},
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair_handler},
        {.uri = "/api/unpair", .method = HTTP_POST, .handler = unpair_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/test", .method = HTTP_POST, .handler = test_handler},
        {.uri = "/ota/manifest", .method = HTTP_GET, .handler = ota_manifest_handler},
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    ESP_LOGI(TAG, "HTTP 服务已启动，端口 %d", HTTP_PORT);
}
