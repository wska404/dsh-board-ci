#pragma once

#include <stdbool.h>
#include <stddef.h>

/** 初始化 NVS（默认分区 + factory 分区），加载/生成设备身份。 */
void pairing_init(void);

const char *pairing_id(void);
const char *pairing_name(void);
const char *pairing_ap_pass(void);

bool pairing_is_paired(void);

/** 校验标签上的 6 位配对码。 */
bool pairing_verify_code(const char *code);

/** 生成并保存 token；调用方负责把 token 返回给客户端。 */
bool pairing_issue_token(char *out, size_t out_len);

/** 校验 Authorization / query 里的 token。 */
bool pairing_verify_token(const char *token);

/** 解除配对（只清 token，不清 Wi-Fi/配对码）。 */
void pairing_clear_token(void);

/** 修改显示名（写 factory 分区）。 */
void pairing_set_name(const char *name);
