#include "pairing.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pairing";

static char s_id[13];
static char s_name[64];
static char s_ap_pass[64];
static uint8_t s_code_hash[32];
static SemaphoreHandle_t s_lock;

static void sha256_buf(const uint8_t *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void hash_code(const char *code, uint8_t out[32])
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%s", code, s_id);
    sha256_buf((const unsigned char *)buf, strlen(buf), out);
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool read_str(nvs_handle_t handle, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static void make_defaults(nvs_handle_t factory)
{
    if (s_id[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_id, sizeof(s_id), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (s_name[0] == '\0') {
        snprintf(s_name, sizeof(s_name), "DSH-Board-%s", s_id + 8);
    }

    if (s_ap_pass[0] == '\0') {
        snprintf(s_ap_pass, sizeof(s_ap_pass), "dsh%05u", (unsigned)(esp_random() % 100000U));
    }

    size_t hash_len = sizeof(s_code_hash);
    if (nvs_get_blob(factory, "code_hash", s_code_hash, &hash_len) != ESP_OK ||
        hash_len != sizeof(s_code_hash)) {
        hash_code(DEV_DEFAULT_PAIR_CODE, s_code_hash);
        ESP_LOGW(TAG, "未找到 code_hash，开发默认配对码：%s（量产必须改写）", DEV_DEFAULT_PAIR_CODE);
    }

    nvs_set_str(factory, "id", s_id);
    nvs_set_str(factory, "name", s_name);
    nvs_set_str(factory, "ap_pass", s_ap_pass);
    nvs_set_blob(factory, "code_hash", s_code_hash, sizeof(s_code_hash));
    nvs_commit(factory);
}

void pairing_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    err = nvs_flash_init_partition(FACTORY_NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(FACTORY_NVS_PARTITION));
        ESP_ERROR_CHECK(nvs_flash_init_partition(FACTORY_NVS_PARTITION));
    } else {
        ESP_ERROR_CHECK(err);
    }

    nvs_handle_t factory;
    ESP_ERROR_CHECK(nvs_open_from_partition(FACTORY_NVS_PARTITION, "dsh", NVS_READWRITE, &factory));

    read_str(factory, "id", s_id, sizeof(s_id));
    read_str(factory, "name", s_name, sizeof(s_name));
    read_str(factory, "ap_pass", s_ap_pass, sizeof(s_ap_pass));
    make_defaults(factory);

    ESP_LOGI(TAG, "id=%s name=%s ap=%s", s_id, s_name, s_ap_pass);
    nvs_close(factory);
}

const char *pairing_id(void)
{
    return s_id;
}

const char *pairing_name(void)
{
    return s_name;
}

const char *pairing_ap_pass(void)
{
    return s_ap_pass;
}

bool pairing_is_paired(void)
{
    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t tmp[32];
    size_t len = sizeof(tmp);
    bool paired = (nvs_get_blob(handle, "token_hash", tmp, &len) == ESP_OK && len == sizeof(tmp));
    nvs_close(handle);
    return paired;
}

bool pairing_verify_code(const char *code)
{
    if (!code || strlen(code) != 6) return false;
    uint8_t candidate[32];
    hash_code(code, candidate);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool match = ct_equal(candidate, s_code_hash, sizeof(candidate));
    xSemaphoreGive(s_lock);
    return match;
}

bool pairing_issue_token(char *out, size_t out_len)
{
    uint8_t raw[32];
    esp_fill_random(raw, sizeof(raw));

    unsigned char b64[64];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, raw, sizeof(raw)) != 0) return false;
    if (b64_len >= out_len) return false;

    size_t j = 0;
    for (size_t i = 0; i < b64_len; i++) {
        char c = (char)b64[i];
        if (c == '=') break;
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        out[j++] = c;
    }
    out[j] = '\0';

    uint8_t hash[32];
    sha256_buf((const unsigned char *)out, strlen(out), hash);

    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "token_hash", hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool pairing_verify_token(const char *token)
{
    if (!token || token[0] == '\0') return false;

    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t stored[32];
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(handle, "token_hash", stored, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(stored)) return false;

    // token 是 base64url 文本；板子只存原始 32 字节的 SHA-256。
    uint8_t hash[32];
    sha256_buf((const unsigned char *)token, strlen(token), hash);
    return ct_equal(hash, stored, sizeof(hash));
}

void pairing_clear_token(void)
{
    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, "token_hash");
    nvs_commit(handle);
    nvs_close(handle);
}

void pairing_set_name(const char *name)
{
    if (!name || !name[0]) return;
    strlcpy(s_name, name, sizeof(s_name));

    nvs_handle_t factory;
    if (nvs_open_from_partition(FACTORY_NVS_PARTITION, "dsh", NVS_READWRITE, &factory) != ESP_OK) return;
    nvs_set_str(factory, "name", s_name);
    nvs_commit(factory);
    nvs_close(factory);
}
