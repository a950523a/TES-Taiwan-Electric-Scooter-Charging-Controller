#include "drivers/psu_driver.h"
#include "hal/hal_uart.h"
#include "psu_link/psu_pair.h"
#include "psu_link/psu_sess.h"
#include "psu_link/psu_crypto_mbedtls.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

#define PSU_UART_TIMEOUT_TICKS    300   // 300 × 10ms = 3s   (UART，有線可寬鬆)
#define PSU_ESPNOW_TIMEOUT_TICKS  200   // 200 × 10ms = 2s  (ESP-NOW；MAC-ACK 連敗仍 30ms 快斷)
#define PSU_ESPNOW_FAIL_LIMIT       3   // 連續 MAC-ACK 失敗 N 次 → 立即標記斷線
#define PSU_SET_MIN_TICKS          50   //  50 × 10ms = 500ms：同值 SET 最短發送間隔（UART 與 ESP-NOW 共用）
#define PSU_HELLO_INTERVAL_TICKS  100   // 100 × 10ms = 1s：已連線但還不知道節點能力時，多久問一次
#define PSU_ESPNOW_RSSI_WARN      -80   // dBm，低於此值印 LOGW
#define PAIR_LINGER_TICKS         300   // 配對結束後再處理 3 s 的訊息：對方可能還在等最後一則確認

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
    char    data[PSU_LINK_MAX_AUTH_LINE];
    int     len;
    uint8_t src_mac[6];
} espnow_rx_item_t;

static QueueHandle_t      s_espnow_rx_q = NULL;
static bool               s_has_peer    = false;
static uint8_t            s_peer_mac[6] = {0};
static psu_pair_done_cb_t s_pair_cb     = NULL;

// ── ESP-NOW 配對與連線驗證 ──────────────────────────────────────────────────────
//
// 無線上任何人都能送封包，所以配對的節點送來的每一行都要驗證（psu_sess），
// 沒帶驗證碼的只接受配對與握手訊息。為什麼不用 ESP-NOW 自己的加密：它擋不住
// 偽造 —— 裝置照樣收未加密的單播，回呼也分不出來。見 psu_link/psu_sess.h。
//
// s_sess 會被兩個任務碰：task_hal_poll（收、握手）與 task_tes_sm（送 SET 時加尾碼）。
// 用 mutex 不用 portMUX：HMAC 要幾十 µs，不適合關中斷。送出也在鎖內，
// 否則計數器 5、6 可能以 6、5 的順序送出，5 就被當成重送丟掉。
static SemaphoreHandle_t s_link_lock;
static psu_sess_t        s_sess;
static bool              s_sess_on;        // 有配對金鑰，驗證生效中
static bool              s_needs_repair;   // NVS 裡只有舊版配對的 MAC

// 配對狀態機只在 task_hal_poll 裡跑；其他任務只送要求
static psu_pair_t        s_pair;
static bool              s_pair_on;        // 配對中，或剛結束、還在收尾
static uint32_t          s_pair_state_ticks;
static uint8_t           s_pair_last_state;
static bool              s_legacy_seen;
static volatile bool     s_pair_start_req;
static volatile uint8_t  s_pair_user_req;  // 0 = 無、1 = 確認、2 = 取消
static psu_pair_info_t   s_pair_info;      // 給 OLED／網頁，在 s_status_mux 下讀寫

static uint32_t now_ms(void) { return s_poll_ticks * 10u; }

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

static void add_peer(const uint8_t *mac);

// 一般訊息（HELO、SET）。ESP-NOW 上一律加驗證尾碼；還沒握手完成就不送 ——
// 節點反正會丟掉沒驗證的訊息，送了只是佔頻寬。
static void send_msg(const psu_msg_t *m)
{
    char buf[PSU_LINK_MAX_LINE];
    size_t n = psu_link_encode(m, buf, sizeof buf);
    if (n == 0) return;
    if (s_transport != PSU_TRANSPORT_ESPNOW) {
        hal_uart_psu_write((const uint8_t *)buf, n);
        return;
    }
    if (!s_sess_on || !s_link_lock) return;
    if (xSemaphoreTake(s_link_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;   // SET 會在 500 ms 內重送
    char out[PSU_LINK_MAX_AUTH_LINE];
    size_t w = psu_sess_wrap(&s_sess, buf, n, out, sizeof out);
    if (w > 0) esp_now_send(s_peer_mac, (const uint8_t *)out, w);
    xSemaphoreGive(s_link_lock);
}

// 配對與握手訊息：不帶驗證尾碼（它們本身就是建立驗證的過程）
static void send_plain(const uint8_t *dest, const psu_msg_t *m)
{
    char buf[PSU_LINK_MAX_LINE];
    size_t n = psu_link_encode(m, buf, sizeof buf);
    if (n == 0) return;
    add_peer(dest);
    esp_now_send(dest, (const uint8_t *)buf, n);
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

static void count_reject(void)
{
    taskENTER_CRITICAL(&s_status_mux);
    s_status.auth_rejects++;
    taskEXIT_CRITICAL(&s_status_mux);
}

// ESP-NOW 收到的一行。配對的節點送來、驗證通過的才交給 handle_line()；
// 沒帶驗證碼的只接受配對與握手訊息 —— 其餘的正是冒用 MAC 的偽造。
static void handle_espnow_line(const char *line, size_t len, const uint8_t src[6])
{
    bool from_peer = s_has_peer && memcmp(src, s_peer_mac, 6) == 0;
    size_t inner = 0;
    psu_sess_result_t r = PSU_SESS_PLAIN;

    xSemaphoreTake(s_link_lock, portMAX_DELAY);
    if (s_sess_on && from_peer) r = psu_sess_unwrap(&s_sess, line, len, &inner, now_ms());
    xSemaphoreGive(s_link_lock);

    if (r == PSU_SESS_OK) { handle_line(line, inner); return; }
    if (r != PSU_SESS_PLAIN) { count_reject(); return; }   // 驗不過、重送、還沒握手

    psu_msg_t m;
    if (psu_link_decode(line, len, &m) != PSU_LINK_OK) {
        // 舊版 LianMing 韌體的純文字配對廣播：提示使用者更新 PSU
        if (s_pair_on && len >= 9 && memcmp(line, "PSU_HELLO", 9) == 0) s_legacy_seen = true;
        return;
    }
    switch (m.type) {
    case PSU_MSG_SESS_INIT: case PSU_MSG_SESS_REPLY:
    case PSU_MSG_SESS_FINISH: case PSU_MSG_SESS_REQUEST:
        if (s_sess_on && from_peer) {
            xSemaphoreTake(s_link_lock, portMAX_DELAY);
            psu_sess_rx(&s_sess, &m, now_ms());
            xSemaphoreGive(s_link_lock);
        }
        return;
    case PSU_MSG_PAIR_KEY: case PSU_MSG_PAIR_COMMIT: case PSU_MSG_PAIR_NONCE:
    case PSU_MSG_PAIR_CONFIRM: case PSU_MSG_PAIR_REJECT:
        if (s_pair_on) psu_pair_rx(&s_pair, &m, src, now_ms());
        return;
    default:
        if (from_peer) count_reject();   // 沒有驗證碼的 ST／ACK／CAP：不收
        return;
    }
}

// 一個 ESP-NOW 封包可能帶不只一行
static void handle_datagram(const char *data, int len, const uint8_t src[6])
{
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            if (i > start) handle_espnow_line(data + start, (size_t)(i - start), src);
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
    if (data_len <= 0 || data_len > (int)PSU_LINK_MAX_AUTH_LINE - 1 || !s_espnow_rx_q) return;
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

static void pair_start(void)
{
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

    // ESP-NOW 從 STA 介面送出，節點看到的來源位址就是這個 MAC；
    // 兩邊推導金鑰時都要用對方「看到的」MAC，對不上金鑰確認就會失敗。
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    psu_pair_start(&s_pair, PSU_ROLE_CONTROLLER, mac, psu_crypto_mbedtls(), now_ms());
    s_pair_on     = true;
    s_legacy_seen = false;
    ESP_LOGI(TAG, "ESP-NOW pairing started — waiting for a PSU in pairing mode");
}

static void pair_apply(void)
{
    if (s_has_peer && memcmp(s_peer_mac, s_pair.peer_mac, 6) != 0) esp_now_del_peer(s_peer_mac);
    memcpy(s_peer_mac, s_pair.peer_mac, 6);
    s_has_peer     = true;
    s_needs_repair = false;
    add_peer(s_peer_mac);

    xSemaphoreTake(s_link_lock, portMAX_DELAY);
    psu_sess_init(&s_sess, PSU_ROLE_CONTROLLER, s_pair.ltk, psu_crypto_mbedtls(), now_ms());
    s_sess_on = true;
    xSemaphoreGive(s_link_lock);
    link_down();   // 新的節點從頭來：先握手，再等它的 CAP / ST

    ESP_LOGI(TAG, "PSU paired (code %06lu): %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned long)s_pair.code, s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
             s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
    if (s_pair_cb) s_pair_cb(s_peer_mac, s_pair.ltk);
}

static const char *pair_fail_name(uint8_t f)
{
    switch (f) {
    case PSU_PAIR_FAIL_TIMEOUT:     return "timeout";
    case PSU_PAIR_FAIL_USER_REJECT: return "cancelled here";
    case PSU_PAIR_FAIL_PEER_REJECT: return "cancelled on the PSU";
    case PSU_PAIR_FAIL_COMMIT:      return "commitment mismatch (possible man-in-the-middle)";
    case PSU_PAIR_FAIL_CONFIRM:     return "key confirmation mismatch (possible man-in-the-middle)";
    case PSU_PAIR_FAIL_CRYPTO:      return "crypto error";
    default:                        return "?";
    }
}

static void pair_step(void)
{
    if (s_pair_start_req) {
        s_pair_start_req = false;
        pair_start();
    }
    uint8_t u = s_pair_user_req;
    if (u) {
        s_pair_user_req = 0;
        if (s_pair_on) psu_pair_user(&s_pair, u == 1, now_ms());
    }
    if (!s_pair_on) return;

    psu_msg_t m;
    bool bcast;
    while (psu_pair_poll(&s_pair, now_ms(), &m, &bcast))
        send_plain(bcast ? BCAST_MAC : s_pair.peer_mac, &m);

    if (s_pair.state != s_pair_last_state) {
        s_pair_last_state  = s_pair.state;
        s_pair_state_ticks = s_poll_ticks;
        if (s_pair.state == PSU_PAIR_CONFIRM)
            ESP_LOGI(TAG, "pairing code %06lu — confirm on both devices", (unsigned long)s_pair.code);
        else if (s_pair.state == PSU_PAIR_DONE)
            pair_apply();
        else if (s_pair.state == PSU_PAIR_FAILED)
            ESP_LOGW(TAG, "pairing failed: %s", pair_fail_name(s_pair.fail));
    }
    if (!psu_pair_busy(&s_pair) && s_poll_ticks - s_pair_state_ticks >= PAIR_LINGER_TICKS) {
        s_pair_on = false;
        memset(s_pair.ltk, 0, sizeof s_pair.ltk);   // 已存進 s_sess 與 NVS
    }
}

static void pair_publish(void)
{
    psu_pair_info_t i = {
        .state       = s_pair.state,
        .fail        = s_pair.fail,
        .code        = s_pair.code,
        .local_ok    = s_pair.local_ok,
        .peer_ok     = s_pair.peer_ok,
        .legacy_seen = s_legacy_seen,
        .age_ms      = (s_poll_ticks - s_pair_state_ticks) * 10u,
    };
    memcpy(i.peer_mac, s_pair.peer_mac, 6);
    taskENTER_CRITICAL(&s_status_mux);
    s_pair_info = i;
    taskEXIT_CRITICAL(&s_status_mux);
}

static void poll_espnow(void)
{
    if (!s_espnow_rx_q) return;

    espnow_rx_item_t item;
    while (xQueueReceive(s_espnow_rx_q, &item, 0) == pdTRUE)
        handle_datagram(item.data, item.len, item.src_mac);

    pair_step();
    pair_publish();

    // 握手訊息（SH1 重送、SH3）。在鎖內送：跟 send_msg() 的計數器順序一致
    if (s_sess_on) {
        psu_msg_t m;
        xSemaphoreTake(s_link_lock, portMAX_DELAY);
        while (psu_sess_poll(&s_sess, now_ms(), &m)) send_plain(s_peer_mac, &m);
        xSemaphoreGive(s_link_lock);
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

esp_err_t psu_driver_set_transport(psu_transport_t t, const uint8_t *peer_mac_6,
                                   const uint8_t *ltk_32)
{
    if (t != PSU_TRANSPORT_ESPNOW) {
        s_transport = PSU_TRANSPORT_UART;
        return ESP_OK;
    }
    // pair_cb 若已由外部設定則保留，否則等 start_pairing() 時再設定

    s_link_lock = xSemaphoreCreateMutex();
    s_espnow_rx_q = xQueueCreate(8, sizeof(espnow_rx_item_t));
    if (!s_espnow_rx_q || !s_link_lock) return ESP_ERR_NO_MEM;

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

    if (peer_mac_6 && ltk_32) {
        memcpy(s_peer_mac, peer_mac_6, 6);
        s_has_peer = true;
        add_peer(s_peer_mac);
        psu_sess_init(&s_sess, PSU_ROLE_CONTROLLER, ltk_32, psu_crypto_mbedtls(), now_ms());
        s_sess_on = true;
        ESP_LOGI(TAG, "ESP-NOW transport ready, peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
                 s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
    } else if (peer_mac_6) {
        // 舊版配對只存了 MAC：沒有金鑰就無法驗證任何一則訊息，只能當成沒配對
        s_needs_repair = true;
        ESP_LOGW(TAG, "ESP-NOW: stored pairing predates link authentication — pair the PSU again");
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
    s_pair_start_req = true;  // task_hal_poll 的 psu_driver_poll() 裡才真正開始
}

psu_pair_info_t psu_driver_pair_info(void)
{
    psu_pair_info_t i;
    taskENTER_CRITICAL(&s_status_mux);
    i = s_pair_info;
    taskEXIT_CRITICAL(&s_status_mux);
    return i;
}

static bool pair_busy_state(uint8_t st)
{
    return st == PSU_PAIR_SEARCHING || st == PSU_PAIR_EXCHANGING || st == PSU_PAIR_CONFIRM;
}

bool psu_driver_is_pairing(void)
{
    return s_pair_start_req || pair_busy_state(psu_driver_pair_info().state);
}

bool psu_driver_pair_wants_buttons(void)
{
    return psu_driver_is_pairing();
}

void psu_driver_pair_user(bool accept)
{
    s_pair_user_req = accept ? 1 : 2;
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
        st.rssi         = s_last_rssi;
        st.fail_streak  = s_send_fail_streak;
        st.link_auth    = s_sess_on && psu_sess_ready(&s_sess);
        st.needs_repair = s_needs_repair;
    }
    return st;
}
