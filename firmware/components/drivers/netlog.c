#include "drivers/netlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static netlog_entry_t s_ring[NETLOG_LEN];
static uint32_t       s_count;            // 總共寫過幾件（環形位置 = s_count % NETLOG_LEN）
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;

void netlog_add(netlog_code_t code, int32_t a, int32_t b)
{
    netlog_entry_t e = {
        .t_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .code = (uint8_t)code, .a = a, .b = b,
    };
    taskENTER_CRITICAL(&s_mux);
    s_ring[s_count % NETLOG_LEN] = e;
    s_count++;
    taskEXIT_CRITICAL(&s_mux);
}

size_t netlog_read(netlog_entry_t *out, size_t cap)
{
    taskENTER_CRITICAL(&s_mux);
    uint32_t n = s_count < NETLOG_LEN ? s_count : NETLOG_LEN;
    if (n > cap) n = (uint32_t)cap;
    uint32_t first = s_count - n;
    for (uint32_t i = 0; i < n; i++) out[i] = s_ring[(first + i) % NETLOG_LEN];
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

const char *netlog_name(uint8_t code)
{
    switch (code) {
    case NETLOG_WIFI_TRY:     return "wifi_try";
    case NETLOG_WIFI_UP:      return "wifi_up";
    case NETLOG_WIFI_DOWN:    return "wifi_down";
    case NETLOG_WIFI_TIMEOUT: return "wifi_timeout";
    case NETLOG_SCAN_START:   return "scan";
    case NETLOG_SCAN_DONE:    return "scan_done";
    case NETLOG_AP_ON:        return "ap_on";
    case NETLOG_AP_OFF:       return "ap_off";
    case NETLOG_RADIO_BUSY:   return "radio_busy";
    case NETLOG_PSU_UP:       return "psu_up";
    case NETLOG_PSU_DOWN:     return "psu_down";
    case NETLOG_SESS_READY:   return "sess_ready";
    case NETLOG_CHN:          return "chn";
    default:                  return "?";
    }
}
