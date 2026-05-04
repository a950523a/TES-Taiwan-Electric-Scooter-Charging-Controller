#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// NVS 薄包裝，集中管理 namespace，替換 V2 的散落 Preferences 呼叫
esp_err_t hal_nvs_init    (void);
esp_err_t hal_nvs_get_u32 (const char *ns, const char *key, uint32_t *out);
esp_err_t hal_nvs_set_u32 (const char *ns, const char *key, uint32_t val);
esp_err_t hal_nvs_get_i32 (const char *ns, const char *key, int32_t  *out);
esp_err_t hal_nvs_set_i32 (const char *ns, const char *key, int32_t  val);
esp_err_t hal_nvs_get_str (const char *ns, const char *key, char *out, size_t max_len);
esp_err_t hal_nvs_set_str (const char *ns, const char *key, const char *val);
esp_err_t hal_nvs_get_bool(const char *ns, const char *key, bool *out);
esp_err_t hal_nvs_set_bool(const char *ns, const char *key, bool val);
esp_err_t hal_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len);
esp_err_t hal_nvs_set_blob(const char *ns, const char *key, const void *val, size_t len);
