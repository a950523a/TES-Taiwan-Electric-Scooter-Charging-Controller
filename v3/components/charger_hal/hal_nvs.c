#include "hal/hal_nvs.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "hal_nvs";

esp_err_t hal_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash erase required, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    return nvs_open(ns, mode, h);
}

esp_err_t hal_nvs_get_u32(const char *ns, const char *key, uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_u32(h, key, out);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_set_u32(const char *ns, const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u32(h, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_get_i32(const char *ns, const char *key, int32_t *out)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_i32(h, key, out);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_set_i32(const char *ns, const char *key, int32_t val)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_i32(h, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_get_str(const char *ns, const char *key, char *out, size_t max_len)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_str(h, key, out, &max_len);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_set_str(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(h, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_get_bool(const char *ns, const char *key, bool *out)
{
    uint8_t v = 0;
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_u8(h, key, &v);
    nvs_close(h);
    if (ret == ESP_OK) *out = (v != 0);
    return ret;
}

esp_err_t hal_nvs_set_bool(const char *ns, const char *key, bool val)
{
    nvs_handle_t h;
    esp_err_t ret = open_ns(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(h, key, val ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}
