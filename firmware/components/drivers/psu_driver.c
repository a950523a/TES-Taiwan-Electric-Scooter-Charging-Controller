#include "drivers/psu_driver.h"
#include "hal/hal_uart.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PSU_UART_TIMEOUT_TICKS    300   // 300 × 10ms = 3s   (UART，有線可寬鬆)
#define PSU_ESPNOW_TIMEOUT_TICKS  200   // 200 × 10ms = 2s  (ESP-NOW；MAC-ACK 連敗仍 30ms 快斷)
#define PSU_ESPNOW_FAIL_LIMIT       3   // 連續 MAC-ACK 失敗 N 次 → 立即標記斷線
#define PSU_ESPNOW_TX_MIN_TICKS    50   //  50 × 10ms = 500ms：同值 SET 最短發送間隔
#define PSU_ESPNOW_RSSI_WARN      -80   // dBm，低於此值印 LOGW
#define PAIRING_TIMEOUT_TICKS    1000   // 1000 × 10ms = 10s pairing window

static const char *TAG = "psu_driver";

// ── Shared state ──────────────────────────────────────────────────────────────

static psu_status_t    s_status           = { .voltage = 0.f, .current = 0.f, .connected = false };
static uint32_t        s_poll_ticks       = 0;
static uint32_t        s_last_valid_ticks = 0;
static psu_transport_t s_transport        = PSU_TRANSPORT_UART;

// ── UART state ────────────────────────────────────────────────────────────────

static char s_rx_buf[64];
static int  s_rx_len = 0;

// ── ESP-NOW state ─────────────────────────────────────────────────────────────

typedef struct {
    char    data[64];
    int     len;
    uint8_t src_mac[6];
} espnow_rx_item_t;

static QueueHandle_t      s_espnow_rx_q = NULL;
static bool               s_has_peer    = false;
static uint8_t            s_peer_mac[6] = {0};
static volatile bool      s_pairing     = false;
static uint32_t           s_pairing_end = 0;
static psu_pair_done_cb_t s_pair_cb     = NULL;

// ESP-NOW 品質追蹤
static volatile uint8_t  s_send_fail_streak = 0;   // 連續 MAC-ACK 失敗次數（WiFi task 寫）
static volatile int8_t   s_last_rssi        = 0;   // 最後收到的 RSSI（WiFi task 寫）
static bool              s_rssi_warned      = false;

// SET 節流（同 task_tes_sm 呼叫，不跨 task）
static float    s_cached_v       = -1.f;  // 最後實際發出的電壓 setpoint
static float    s_cached_a       = -1.f;  // 最後實際發出的電流 setpoint
static uint32_t s_last_v_tx_tick = 0;    // 發出時的 s_poll_ticks
static uint32_t s_last_a_tx_tick = 0;

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Helpers ───────────────────────────────────────────────────────────────────

static void parse_frame(const char *frame)
{
    float v = 0.f, a = 0.f;
    if (sscanf(frame, "V=%f,I=%f", &v, &a) == 2) {
        s_status.voltage   = v;
        s_status.current   = a;
        s_status.connected = true;
        s_last_valid_ticks = s_poll_ticks;
    } else if (strncmp(frame, "CMD_ACK:", 8) == 0) {
        if (!s_status.connected)
            ESP_LOGI(TAG, "PSU connected (CMD_ACK)");
        s_status.connected = true;
        s_last_valid_ticks = s_poll_ticks;
    } else if (strncmp(frame, "HB", 2) == 0) {
        // PSU 待機心跳：只更新 liveness，不改 voltage/current
        if (!s_status.connected)
            ESP_LOGI(TAG, "PSU connected (HB)");
        s_status.connected = true;
        s_last_valid_ticks = s_poll_ticks;
    } else {
        ESP_LOGW(TAG, "bad frame: %s", frame);
    }
}

static void add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, mac, 6);
    esp_now_add_peer(&peer);
}

// ── ESP-NOW callbacks (run in WiFi task context) ──────────────────────────────

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    // 只追蹤送往已配對 PSU 的封包
    if (!s_has_peer || memcmp(tx_info->des_addr, s_peer_mac, 6) != 0) return;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_fail_streak = 0;
    } else {
        if (s_send_fail_streak < 255) s_send_fail_streak++;
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (data_len <= 0 || data_len > 63 || !s_espnow_rx_q) return;
    // 記錄 RSSI
    if (recv_info->rx_ctrl) s_last_rssi = (int8_t)recv_info->rx_ctrl->rssi;
    espnow_rx_item_t item;
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    item.len = data_len;
    memcpy(item.src_mac, recv_info->src_addr, 6);
    xQueueSend(s_espnow_rx_q, &item, 0);
}

// ── Transport-specific poll ───────────────────────────────────────────────────

static void poll_uart(void)
{
    uint8_t byte;
    while (hal_uart_psu_read(&byte, 1, 0) == 1) {
        if (byte == '\n') {
            s_rx_buf[s_rx_len] = '\0';
            parse_frame(s_rx_buf);
            s_rx_len = 0;
        } else if (s_rx_len < (int)(sizeof(s_rx_buf) - 1)) {
            s_rx_buf[s_rx_len++] = (char)byte;
        } else {
            s_rx_len = 0;
        }
    }
}

static void poll_espnow(void)
{
    espnow_rx_item_t item;
    while (xQueueReceive(s_espnow_rx_q, &item, 0) == pdTRUE) {
        if (s_pairing) {
            if (strncmp(item.data, "PSU_HELLO", 9) == 0) {
                memcpy(s_peer_mac, item.src_mac, 6);
                s_has_peer = true;
                s_pairing  = false;
                add_peer(s_peer_mac);
                // PSU learns TES MAC from the Src MAC of this reply (any payload works)
                const char reply[] = "TES_HELLO\n";
                esp_now_send(s_peer_mac, (const uint8_t *)reply, sizeof(reply) - 1);
                ESP_LOGI(TAG, "PSU paired: %02X:%02X:%02X:%02X:%02X:%02X",
                         s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
                         s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
                if (s_pair_cb) s_pair_cb(s_peer_mac);
            }
            continue;
        }
        if (!s_has_peer || memcmp(item.src_mac, s_peer_mac, 6) != 0) continue;
        parse_frame(item.data);
    }

    // 配對逾時
    if (s_pairing && s_poll_ticks >= s_pairing_end) {
        s_pairing = false;
        ESP_LOGW(TAG, "ESP-NOW pairing timed out");
    }

    // 連續 MAC-ACK 失敗 → 立即斷線（不等 timeout）
    if (s_has_peer && s_send_fail_streak >= PSU_ESPNOW_FAIL_LIMIT && s_status.connected) {
        s_status.connected = false;
        s_status.voltage   = 0.f;
        s_status.current   = 0.f;
        ESP_LOGE(TAG, "ESP-NOW: %u consecutive MAC-ACK failures — PSU disconnected",
                 (unsigned)s_send_fail_streak);
    }

    // RSSI 臨界警告（跨越邊界才印，避免 log 洪泛）
    if (s_status.connected && s_last_rssi != 0) {
        if (s_last_rssi < PSU_ESPNOW_RSSI_WARN && !s_rssi_warned) {
            ESP_LOGW(TAG, "ESP-NOW RSSI low: %d dBm", (int)s_last_rssi);
            s_rssi_warned = true;
        } else if (s_last_rssi >= PSU_ESPNOW_RSSI_WARN && s_rssi_warned) {
            ESP_LOGI(TAG, "ESP-NOW RSSI recovered: %d dBm", (int)s_last_rssi);
            s_rssi_warned = false;
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t psu_driver_init(void)
{
    return ESP_OK;   // UART already initialised by hal_uart_psu_init()
}

esp_err_t psu_driver_set_transport(psu_transport_t t, const uint8_t *peer_mac_6)
{
    s_transport = t;
    if (t != PSU_TRANSPORT_ESPNOW) return ESP_OK;
    // pair_cb 若已由外部設定則保留，否則等 start_pairing() 時再設定

    s_espnow_rx_q = xQueueCreate(8, sizeof(espnow_rx_item_t));
    if (!s_espnow_rx_q) return ESP_ERR_NO_MEM;

    esp_err_t r = esp_now_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(r));
        s_transport = PSU_TRANSPORT_UART;
        return r;
    }
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    // 廣播 peer — 配對時接收 PSU_HELLO 廣播封包
    add_peer(BCAST_MAC);

    if (peer_mac_6) {
        memcpy(s_peer_mac, peer_mac_6, 6);
        s_has_peer = true;
        add_peer(s_peer_mac);
        ESP_LOGI(TAG, "ESP-NOW transport ready, peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
                 s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
    } else {
        ESP_LOGI(TAG, "ESP-NOW transport ready, no peer yet — awaiting pairing");
    }
    return ESP_OK;
}

void psu_driver_set_pair_callback(psu_pair_done_cb_t cb)
{
    s_pair_cb = cb;
}

void psu_driver_start_pairing(psu_pair_done_cb_t cb)
{
    if (s_transport != PSU_TRANSPORT_ESPNOW) {
        ESP_LOGW(TAG, "start_pairing: transport is not ESP-NOW");
        return;
    }
    if (cb) s_pair_cb = cb;   // 覆蓋；NULL 表示沿用已設定的持久回呼
    // PSU AP is fixed on channel 1; switch to ch1 so broadcast is receivable
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    s_pairing     = true;
    s_pairing_end = s_poll_ticks + PAIRING_TIMEOUT_TICKS;
    ESP_LOGI(TAG, "ESP-NOW pairing started (10s window, switched to ch1)");
}

bool psu_driver_is_pairing(void)
{
    return s_pairing;
}

bool psu_driver_has_peer(void)
{
    return s_transport == PSU_TRANSPORT_ESPNOW && s_has_peer;
}

void psu_driver_poll(void)
{
    s_poll_ticks++;

    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        poll_espnow();
    } else {
        poll_uart();
    }

    // 逾時：無有效 frame → 標記斷線（ESP-NOW 500ms，UART 3s）
    uint32_t timeout = (s_transport == PSU_TRANSPORT_ESPNOW)
                       ? PSU_ESPNOW_TIMEOUT_TICKS : PSU_UART_TIMEOUT_TICKS;
    if (s_status.connected &&
        (s_poll_ticks - s_last_valid_ticks) >= timeout) {
        s_status.connected = false;
        s_status.voltage   = 0.f;
        s_status.current   = 0.f;
        ESP_LOGW(TAG, "PSU timeout — disconnected (%s, %lums)",
                 s_transport == PSU_TRANSPORT_ESPNOW ? "ESP-NOW" : "UART",
                 (unsigned long)(timeout * 10));
    }
}

void psu_driver_set_voltage(float v)
{
    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        // 節流：值沒變（誤差 < 0.05V）且距上次發送未滿 500ms → 跳過
        bool changed = (v < s_cached_v - 0.05f || v > s_cached_v + 0.05f);
        bool due     = (s_poll_ticks - s_last_v_tx_tick) >= PSU_ESPNOW_TX_MIN_TICKS;
        if (!changed && !due) return;
        s_cached_v       = v;
        s_last_v_tx_tick = s_poll_ticks;
        char buf[24];
        int len = snprintf(buf, sizeof(buf), "SET:V=%.1f\n", v);
        if (s_has_peer) esp_now_send(s_peer_mac, (const uint8_t *)buf, (size_t)len);
    } else {
        char buf[24];
        int len = snprintf(buf, sizeof(buf), "SET:V=%.1f\n", v);
        hal_uart_psu_write((const uint8_t *)buf, (size_t)len);
    }
}

void psu_driver_set_current(float a)
{
    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        bool changed = (a < s_cached_a - 0.05f || a > s_cached_a + 0.05f);
        bool due     = (s_poll_ticks - s_last_a_tx_tick) >= PSU_ESPNOW_TX_MIN_TICKS;
        if (!changed && !due) return;
        s_cached_a       = a;
        s_last_a_tx_tick = s_poll_ticks;
        char buf[24];
        int len = snprintf(buf, sizeof(buf), "SET:I=%.1f\n", a);
        if (s_has_peer) esp_now_send(s_peer_mac, (const uint8_t *)buf, (size_t)len);
    } else {
        char buf[24];
        int len = snprintf(buf, sizeof(buf), "SET:I=%.1f\n", a);
        hal_uart_psu_write((const uint8_t *)buf, (size_t)len);
    }
}

psu_status_t psu_driver_get_status(void)
{
    psu_status_t st = s_status;
    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        st.rssi        = s_last_rssi;
        st.fail_streak = s_send_fail_streak;
    }
    return st;
}
