#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "pairing.h"

static const char *TAG = "wifi";

static bool s_connected = false;
static bool s_ap_mode = false;
static bool s_sta_enabled = false;
static wifi_prov_connected_cb_t s_on_connected = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_sta_enabled) {
            ESP_LOGW(TAG, "STA 断开，重试");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_ap_mode = false;
        ESP_LOGI(TAG, "STA 已连接");
        if (s_on_connected) s_on_connected();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        s_ap_mode = true;
        ESP_LOGI(TAG, "SoftAP 已启动");
    }
}

void wifi_prov_init(wifi_prov_connected_cb_t on_connected)
{
    s_on_connected = on_connected;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READONLY, &handle) != ESP_OK) return false;

    esp_err_t err = nvs_get_str(handle, "wifi_ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        nvs_get_str(handle, "wifi_pass", pass, &pass_len);
    }
    nvs_close(handle);
    return err == ESP_OK && ssid[0] != '\0';
}

void wifi_prov_start(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        s_sta_enabled = true;
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "连接 Wi-Fi: %s", ssid);
        return;
    }

    // 没有 Wi-Fi 凭证：开 SoftAP 配网。用 APSTA 以便网页里能扫描附近 AP。
    wifi_config_t ap = {0};
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%s", BOARD_AP_PREFIX, pairing_id() + 8);
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, pairing_ap_pass(), sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_mode = true;
    ESP_LOGI(TAG, "配网热点: %s  密码: %s", ap_ssid, pairing_ap_pass());
}

bool wifi_prov_is_connected(void)
{
    return s_connected;
}

bool wifi_prov_is_ap_mode(void)
{
    return s_ap_mode;
}

void wifi_prov_get_ip(char *out, size_t len)
{
    esp_netif_ip_info_t info = {0};
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, out, len);
    } else {
        strlcpy(out, "0.0.0.0", len);
    }
}

bool wifi_prov_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return false;

    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READWRITE, &handle) != ESP_OK) return false;

    esp_err_t err = nvs_set_str(handle, "wifi_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", password ? password : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void wifi_prov_forget_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open("dsh", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, "wifi_ssid");
    nvs_erase_key(handle, "wifi_pass");
    nvs_commit(handle);
    nvs_close(handle);
}

int wifi_prov_scan_json(char *out, size_t len)
{
    wifi_scan_config_t scan = {0};
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return -1;

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (!records) return -1;
    if (count > 0) esp_wifi_scan_get_ap_records(&count, records);

    cJSON *array = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddItemToArray(array, item);
    }

    char *text = cJSON_PrintUnformatted(array);
    if (!text) {
        cJSON_Delete(array);
        free(records);
        return -1;
    }
    strlcpy(out, text, len);
    cJSON_free(text);
    cJSON_Delete(array);
    free(records);
    return 0;
}
