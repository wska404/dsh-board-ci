#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef void (*wifi_prov_connected_cb_t)(void);

void wifi_prov_init(wifi_prov_connected_cb_t on_connected);
void wifi_prov_start(void);

bool wifi_prov_is_connected(void);
bool wifi_prov_is_ap_mode(void);
void wifi_prov_get_ip(char *out, size_t len);

bool wifi_prov_save_credentials(const char *ssid, const char *password);
void wifi_prov_forget_credentials(void);

/** 扫描附近 AP，返回 JSON 数组字符串；失败返回 -1。 */
int wifi_prov_scan_json(char *out, size_t len);
