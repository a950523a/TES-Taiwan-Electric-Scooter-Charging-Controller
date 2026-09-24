#include "drivers/psu_driver.h"
#include "hal/hal_uart.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

#define PSU_UART_TIMEOUT_TICKS    300   // 300 × 10ms = 3s   (UART，有線可寬鬆)
#define PSU_ESPNOW_TIMEOUT_TICKS  200   // 200 × 10ms = 2s  (ESP-NOW；MAC-ACK 連敗仍 30ms 快斷)
#define PSU_ESPNOW_FAIL_LIMIT       3   // 連續 MAC-ACK 失敗 N 次 → 立即標記斷線
#define PSU_SET_MIN_TICKS          50   //  50 × 10ms = 500ms：同值 SET 最短發送間隔（UART 與 ESP-NOW 共用）
#define PSU_HELLO_INTERVAL_TICKS  100   // 100 × 10ms = 1s：已連線但還不知道節點能力時，多久問一次
#define PSU_ESPNOW_RSSI_WARN      -80   // dBm，低於此值印 LOGW
#define PAIRING_TIMEOUT_TICKS    1000   // 1000 × 10ms = 10s pairing window

static const char *TAG = "psu_driver";

// ── Shared state ──────────────────────────────────────────────────────────────

static psu_status_t    s_status           = { .status_age_ms = UINT32_MAX };
static uint32_t        s_poll_ticks       = 0;
static uint32_t        s_last_valid_ticks = 0;   // 最後一個有效訊框（任何類型）
static uint32_t        s_last_st_ticks    = 0;   // 最後一筆 ST
static bool            s_have_st          = false;
static uint32_t        s_last_hello_ticks = 0;
static psu_transport_t s_transport        = PSU_TRANSPORT_UART;

// s_status 由 task_hal_poll 寫入，由 task_tes_sm / HTTP / display 讀取。
// struct copy 不是原子操作，沒有保護時可能讀到「新電壓 + 舊電流」的組合。
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;

// ── UART state ────────────────────────────────────────────────────────────────

static char s_rx_buf[PSU_LINK_MAX_LINE];
static int  s_rx_len     = 0;
static bool s_rx_discard = false;   // 行太長：丟到下一個 '\n' 為止，不要把後半段當成新的一行

// ── ESP-NOW state ─────────────────────────────────────────────────────────────

typedef struct {
    char    data[PSU_LINK_MAX_LINE];
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

// SET 節流與序號（同 task_tes_sm 呼叫，不跨 task）
static float    s_cached_v       = -1.f;  // 最後實際發出的電壓 setpoint
static float    s_cached_a       = -1.f;  // 最後實際發出的電流 setpoint
static uint32_t s_last_v_tx_tick = 0;     // 發出時的 s_poll_ticks
static uint32_t s_last_a_tx_tick = 0;
static uint16_t s_set_seq        = 0;

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Link state ────────────────────────────────────────────────────────────────

// 斷線時連節點身分一起清掉：下一個連上的可能是另一個節點（或同一個重開機、換過韌體）
static void link_down(void)
{
    taskENTER_CRITICAL(&s_status_mux);
    s_status.connected  = false;
    s_status.voltage    = 0.f;
    s_status.current    = 0.f;
    s_status.caps_known = false;
    s_status.mode       = PSU_MODE_UNKNOWN;
    s_status.flags      = 0;
    taskEXIT_CRITICAL(&s_status_mux);
    s_have_st = false;
}

static void link_alive(void)
{
    if (!s_status.connected) ESP_LOGI(TAG, "PSU connected");
    taskENTER_CRITICAL(&s_status_mux);
    s_status.connected = true;
    taskEXIT_CRITICAL(&s_status_mux);
    s_last_valid_ticks = s_poll_ticks;
}

static void send_line(const char *buf, size_t len)
{
    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        if (s_has_peer) esp_now_send(s_peer_mac, (const uint8_t *)buf, len);
    } else {
        hal_uart_psu_write((const uint8_t *)buf, len);
    }
}

static void send_msg(const psu_msg_t *m)
{
    char buf[PSU_LINK_MAX_LINE];
    size_t n = psu_link_encode(m, buf, sizeof buf);
    if (n > 0) send_line(buf, n);
}

// ── Incoming messages ─────────────────────────────────────────────────────────

static void on_status(const psu_msg_status_t *st)
{
    // 電壓／電流無效時填 0：狀態機以「psu_voltage > 0」判斷 PSU 是否在回報，
    // 否則改用 ADC 量測。舊協定只在輸出中送 V=、待機送 HB，行為要跟它一致。
    bool v_ok = (st->flags & PSU_ST_V_VALID) != 0;
    bool i_ok = (st->flags & PSU_ST_I_VALID) != 0;
    taskENTER_CRITICAL(&s_status_mux);
    s_status.voltage    = v_ok ? (float)st->v_cv / 100.0f : 0.f;
    s_status.current    = i_ok ? (float)st->i_ca / 100.0f : 0.f;
    s_status.mode       = st->mode;
    s_status.flags      = st->flags;
    s_status.status_seq = st->seq;
    taskEXIT_CRITICAL(&s_status_mux);
    s_last_st_ticks = s_poll_ticks;
    s_have_st       = true;
}

static void on_cap(const psu_msg_cap_t *cap)
{
    bool first = !s_status.caps_known;
    taskENTER_CRITICAL(&s_status_mux);
    s_status.caps_known = true;
    s_status.proto_ver  = cap->proto_ver;
    s_status.node_type  = cap->node_type;
    s_status.caps       = cap->caps;
    s_status.v_max_cv   = cap->v_max_cv;
    s_status.i_max_ca   = cap->i_max_ca;
    s_status.fw_ver     = cap->fw_ver;
    taskEXIT_CRITICAL(&s_status_mux);
    if (first) {
        ESP_LOGI(TAG, "PSU node: type=%u caps=0x%04X max=%u.%02uV/%u.%02uA fw=%u proto=%u",
                 cap->node_type, cap->caps,
                 cap->v_max_cv / 100u, cap->v_max_cv % 100u,
                 cap->i_max_ca / 100u, cap->i_max_ca % 100u,
                 cap->fw_ver, cap->proto_ver);
    }
    if (cap->proto_ver != PSU_LINK_PROTO_VER) {
        ESP_LOGW(TAG, "PSU node speaks protocol v%u, we speak v%u",
                 cap->proto_ver, PSU_LINK_PROTO_VER);
    }
}

static void on_ack(const psu_msg_ack_t *ack)
{
    taskENTER_CRITICAL(&s_status_mux);
    s_status.last_ack_seq    = ack->seq;
    s_status.last_ack_result = ack->result;
    taskEXIT_CRITICAL(&s_status_mux);
    if (ack->result != PSU_ACK_OK) {
        ESP_LOGW(TAG, "PSU rejected SET #%u (result %u)", ack->seq, ack->result);
    }
}

static void handle_line(const char *line, size_t len)
{
    psu_msg_t m;
    psu_link_result_t r = psu_link_decode(line, len, &m);

    switch (r) {
    case PSU_LINK_OK:
        break;
    case PSU_LINK_NOT_FRAME:
        return;   // 節點的開機訊息、文字指令回應…不屬於本協定
    case PSU_LINK_BAD_CRC:
        taskENTER_CRITICAL(&s_status_mux);
        s_status.rx_crc_errors++;
        taskEXIT_CRITICAL(&s_status_mux);
        return;
    default:
        taskENTER_CRITICAL(&s_status_mux);
        s_status.rx_bad_frames++;
        taskEXIT_CRITICAL(&s_status_mux);
        ESP_LOGD(TAG, "unusable frame (%d): %.*s", (int)r, (int)len, line);
        return;
    }

    taskENTER_CRITICAL(&s_status_mux);
    s_status.rx_frames++;
    taskEXIT_CRITICAL(&s_status_mux);

    switch (m.type) {
    case PSU_MSG_STATUS: link_alive(); on_status(&m.u.status); break;
    case PSU_MSG_CAP:    link_alive(); on_cap(&m.u.cap);       break;
    case PSU_MSG_ACK:    link_alive(); on_ack(&m.u.ack);       break;
    default:             break;   // HELO / SET 是控制板送出的方向，收到就忽略
    }
}

// 一個 ESP-NOW 封包可能帶不只一行
static void handle_datagram(const char *data, int len)
{
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            if (i > start) handle_line(data + start, (size_t)(i - start));
            start = i + 1;
        }
    }
}

// ── ESP-NOW helpers & callbacks ───────────────────────────────────────────────

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

// (run in WiFi task context)
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

// (run in WiFi task context)
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (data_len <= 0 || data_len > (int)PSU_LINK_MAX_LINE - 1 || !s_espnow_rx_q) return;
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
            if (!s_rx_discard && s_rx_len > 0) handle_line(s_rx_buf, (size_t)s_rx_len);
            s_rx_len     = 0;
            s_rx_discard = false;
        } else if (s_rx_discard) {
            // 丟棄中
        } else if (s_rx_len < (int)(sizeof(s_rx_buf) - 1)) {
            s_rx_buf[s_rx_len++] = (char)byte;
        } else {
            s_rx_len     = 0;
            s_rx_discard = true;
            taskENTER_CRITICAL(&s_status_mux);
            s_status.rx_bad_frames++;
            taskEXIT_CRITICAL(&s_status_mux);
        }
    }
}

static void poll_espnow(void)
{
    if (!s_espnow_rx_q) return;

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
        handle_datagram(item.data, item.len);
    }

    // 配對逾時
    if (s_pairing && s_poll_ticks >= s_pairing_end) {
        s_pairing = false;
        ESP_LOGW(TAG, "ESP-NOW pairing timed out");
    }

    // 連續 MAC-ACK 失敗 → 立即斷線（不等 timeout）
    if (s_has_peer && s_send_fail_streak >= PSU_ESPNOW_FAIL_LIMIT && s_status.connected) {
        link_down();
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
    if (t != PSU_TRANSPORT_ESPNOW) {
        s_transport = PSU_TRANSPORT_UART;
        return ESP_OK;
    }
    // pair_cb 若已由外部設定則保留，否則等 start_pairing() 時再設定

    s_espnow_rx_q = xQueueCreate(8, sizeof(espnow_rx_item_t));
    if (!s_espnow_rx_q) return ESP_ERR_NO_MEM;

    esp_err_t r = esp_now_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(r));
        vQueueDelete(s_espnow_rx_q);      // 舊版在此洩漏 queue
        s_espnow_rx_q = NULL;
        s_transport   = PSU_TRANSPORT_UART;
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

    // 最後才切換 transport：task_hal_poll 優先權高於 app_main，先切換的話
    // 它可能在 s_espnow_rx_q 建立完成前就跑進 poll_espnow()。
    s_transport = PSU_TRANSPORT_ESPNOW;
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

    // PSU 廣播固定在 channel 1。只有在未連上 AP 時才切換 channel ——
    // STA 已連線時 esp_wifi_set_channel() 會被 AP 的 channel 覆寫，
    // 而且強行切換會打斷現有連線（Web UI / MQTT 全斷）。
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGW(TAG, "pairing while STA connected on ch%u — "
                      "PSU must broadcast on the same channel", (unsigned)ap.primary);
    } else {
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        ESP_LOGI(TAG, "switched to ch1 for pairing");
    }

    s_pairing     = true;
    s_pairing_end = s_poll_ticks + PAIRING_TIMEOUT_TICKS;
    ESP_LOGI(TAG, "ESP-NOW pairing started (10s window)");
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

    // 逾時：無有效訊框 → 標記斷線（ESP-NOW 2s，UART 3s）
    uint32_t timeout = (s_transport == PSU_TRANSPORT_ESPNOW)
                       ? PSU_ESPNOW_TIMEOUT_TICKS : PSU_UART_TIMEOUT_TICKS;
    if (s_status.connected &&
        (s_poll_ticks - s_last_valid_ticks) >= timeout) {
        link_down();
        ESP_LOGW(TAG, "PSU timeout — disconnected (%s, %lums)",
                 s_transport == PSU_TRANSPORT_ESPNOW ? "ESP-NOW" : "UART",
                 (unsigned long)(timeout * 10));
    }

    // 節點在說話但還不知道它是什麼 → 問。節點開機時會主動送 CAP，
    // 這是給「控制板比節點晚開機、錯過那一則」的情況用的。
    // 沒連線時不問：場上多數機器的 UART 什麼都沒接。
    if (s_status.connected && !s_status.caps_known &&
        (s_poll_ticks - s_last_hello_ticks) >= PSU_HELLO_INTERVAL_TICKS) {
        s_last_hello_ticks = s_poll_ticks;
        psu_msg_t m = { .type = PSU_MSG_HELLO };
        m.u.hello.proto_ver = PSU_LINK_PROTO_VER;
        send_msg(&m);
    }
}

bool psu_driver_can_set_voltage(void)
{
    return !s_status.caps_known || (s_status.caps & PSU_CAP_SET_V);
}

bool psu_driver_can_set_current(void)
{
    return !s_status.caps_known || (s_status.caps & PSU_CAP_SET_I);
}

// 節流：值沒變（誤差 < 0.05）且距上次發送未滿 500ms → 跳過。
// 兩種 transport 共用 —— task_tes_sm 在 CHARGING 中每 10ms 就會呼叫一次 setpoint，
// 沒有節流等於用 100Hz 灌 PSU。
static bool set_due(float value, float *cached, uint32_t *last_tick)
{
    bool changed = (value < *cached - 0.05f || value > *cached + 0.05f);
    bool due     = (s_poll_ticks - *last_tick) >= PSU_SET_MIN_TICKS;
    if (!changed && !due) return false;
    *cached    = value;
    *last_tick = s_poll_ticks;
    return true;
}

static uint16_t to_centi(float x)
{
    if (!(x > 0.0f)) return 0;               // 負值與 NaN 一律當 0
    if (x >= 655.35f) return 65535u;
    return (uint16_t)(x * 100.0f + 0.5f);
}

static void send_set(bool has_v, float v, bool has_i, float a)
{
    psu_msg_t m = { .type = PSU_MSG_SET };
    m.u.set.seq   = ++s_set_seq;
    m.u.set.has_v = has_v;
    m.u.set.v_cv  = has_v ? to_centi(v) : 0;
    m.u.set.has_i = has_i;
    m.u.set.i_ca  = has_i ? to_centi(a) : 0;
    send_msg(&m);
    taskENTER_CRITICAL(&s_status_mux);
    s_status.last_set_seq = s_set_seq;
    taskEXIT_CRITICAL(&s_status_mux);
}

void psu_driver_set_voltage(float v)
{
    if (!psu_driver_can_set_voltage()) return;
    if (!set_due(v, &s_cached_v, &s_last_v_tx_tick)) return;
    send_set(true, v, false, 0.f);
}

void psu_driver_set_current(float a)
{
    if (!psu_driver_can_set_current()) return;
    if (!set_due(a, &s_cached_a, &s_last_a_tx_tick)) return;
    send_set(false, 0.f, true, a);
}

psu_status_t psu_driver_get_status(void)
{
    psu_status_t st;
    taskENTER_CRITICAL(&s_status_mux);
    st = s_status;
    taskEXIT_CRITICAL(&s_status_mux);
    st.status_age_ms = s_have_st ? (s_poll_ticks - s_last_st_ticks) * 10u : UINT32_MAX;
    if (s_transport == PSU_TRANSPORT_ESPNOW) {
        st.rssi        = s_last_rssi;
        st.fail_streak = s_send_fail_streak;
    }
    return st;
}
