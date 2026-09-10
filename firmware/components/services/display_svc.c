// display_svc.c — OLED status screen + settings menu
// Runs in task_display at 50ms intervals.
// Button events arrive via display_svc_button() called from task_display after
// draining g_display_btn_queue.

#include "services/display_svc.h"
#include "services/config_svc.h"
#include "services/network_svc.h"
#include "drivers/display_driver.h"
#include "drivers/led_driver.h"
#include "drivers/psu_driver.h"
#include "tes_protocol/tes_sm.h"
#include "services/event_bus.h"   // for EVT_BUTTON_* enum values
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display_svc";

// Globals owned by main.c — accessed via extern (same pattern as g_snapshot)
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;
extern volatile bool     g_menu_open;
extern QueueHandle_t     g_btn_event_queue;

// ── Screen state ──────────────────────────────────────────────────────────────

static disp_screen_t s_screen = DISP_SCREEN_STATUS;

// ── Menu state ────────────────────────────────────────────────────────────────

typedef enum {
    MENU_ITEM_AUTO_VOLTAGE = 0,
    MENU_ITEM_MAX_VOLTAGE,
    MENU_ITEM_MAX_CURRENT,
    MENU_ITEM_STOP_MODE,      // 停止條件：SOC / Voltage / Timer
    MENU_ITEM_TARGET_SOC,    // 停止條件值（stop_mode=SOC 時顯示）
    MENU_ITEM_STOP_VOLTAGE,  // 停止條件值（stop_mode=VOLTAGE 時顯示）
    MENU_ITEM_CHARGE_TIMER,  // 充電時長（stop_mode=TIMER 時顯示，分鐘）
    MENU_ITEM_BEACON,
    MENU_ITEM_WIFI_INFO,
    MENU_ITEM_DEVICE_ID,     // 本機 mDNS 主機名 tes-<id>（唯讀，多台辨識用）
    MENU_ITEM_PSU_STATUS,    // PSU 連線狀態（唯讀）
    MENU_ITEM_SCHEDULER,     // 定時充電 ON/OFF（時間設定僅 Web UI）
    MENU_ITEM_AUTO_START,    // Beta: VP 常通 + 自動觸發充電
    MENU_ITEM_RESET_FAULT,   // 手動復歸緊急停止（設定選單確認才有效）
    MENU_ITEM_ABOUT,         // 韌體版本 + 作者（唯讀）
    MENU_ITEM_SAVE,
    MENU_ITEM_CANCEL,
    MENU_ITEM_COUNT
} menu_item_id_t;

typedef enum {
    MENU_MODE_NAV,    // navigating items with cursor
    MENU_MODE_EDIT,   // adjusting a value with START/STOP
} menu_mode_t;

static menu_mode_t  s_mode       = MENU_MODE_NAV;
static int          s_cursor     = 0;
static int          s_scroll_top = 0;

// Working copies of config while menu is open (saved only on "Save & Exit")
static bool        s_edit_auto_voltage;
static uint16_t    s_edit_voltage;
static uint16_t    s_edit_current;
static int8_t      s_edit_soc;
static stop_mode_t s_edit_stop_mode;
static uint16_t    s_edit_stop_voltage;
static uint16_t    s_edit_charge_timer;
static bool        s_edit_beacon;
static bool        s_edit_sched_enabled;
static bool        s_edit_auto_start;

static int s_visible_items[MENU_ITEM_COUNT];
static int s_visible_count = 0;

#define MAX_VISIBLE_ROWS  4
#define HEADER_H         16    // pixels reserved for header + separator
#define ROW_H            12    // pixels per menu row
#define ROW_TEXT_OFS      6    // text baseline offset from row top

// ── Menu helpers ──────────────────────────────────────────────────────────────

static void build_visible_list(void);  // forward declaration

static void menu_open(void)
{
    const charger_config_t *cfg = config_svc_get();
    s_edit_auto_voltage   = cfg->auto_voltage;
    s_edit_voltage        = cfg->max_voltage_01v;
    s_edit_current        = cfg->max_current_01a;
    s_edit_soc            = cfg->target_soc;
    s_edit_stop_mode      = cfg->stop_mode;
    s_edit_stop_voltage   = cfg->stop_voltage_01v;
    s_edit_charge_timer   = cfg->charge_timer_min;
    s_edit_beacon         = cfg->beacon_unlocked;
    s_edit_sched_enabled  = cfg->sched_enabled;
    s_edit_auto_start     = cfg->auto_start;
    s_cursor       = 0;
    s_scroll_top   = 0;
    s_mode         = MENU_MODE_NAV;
    build_visible_list();
    s_screen       = DISP_SCREEN_MENU;
    g_menu_open    = true;
    ESP_LOGI(TAG, "menu open");
}

static void menu_close(void)
{
    s_screen    = DISP_SCREEN_STATUS;
    g_menu_open = false;
    ESP_LOGI(TAG, "menu closed");
}

static void menu_save(void)
{
    config_svc_set_auto_voltage(s_edit_auto_voltage);
    config_svc_set_charging(s_edit_voltage, s_edit_current, s_edit_soc);
    config_svc_set_stop(s_edit_stop_mode, s_edit_stop_voltage, s_edit_charge_timer);
    config_svc_set_beacon(s_edit_beacon);
    {
        const charger_config_t *cfg = config_svc_get();
        config_svc_set_scheduler(s_edit_sched_enabled, cfg->sched_start_min,
                                  cfg->sched_stop_en, cfg->sched_stop_min);
    }
    config_svc_set_auto_start(s_edit_auto_start);
    ESP_LOGI(TAG, "saved: auto_v=%d %u.%uV %u.%uA SOC=%d stop=%d stpV=%u.%u timer=%um beacon=%d",
             (int)s_edit_auto_voltage,
             s_edit_voltage / 10, s_edit_voltage % 10,
             s_edit_current / 10, s_edit_current % 10,
             s_edit_soc, (int)s_edit_stop_mode,
             s_edit_stop_voltage / 10, s_edit_stop_voltage % 10,
             s_edit_charge_timer, (int)s_edit_beacon);
}

static void item_label(int item, char *buf, size_t bufsz)
{
    switch ((menu_item_id_t)item) {
    case MENU_ITEM_AUTO_VOLTAGE:
        snprintf(buf, bufsz, "Auto Volt: %s",
                 s_edit_auto_voltage ? "ON" : "OFF");
        break;
    case MENU_ITEM_MAX_VOLTAGE:
        if (s_edit_auto_voltage)
            snprintf(buf, bufsz, "V Cap: %u.%uV",
                     s_edit_voltage / 10, s_edit_voltage % 10);
        else
            snprintf(buf, bufsz, "Max Volt: %u.%uV",
                     s_edit_voltage / 10, s_edit_voltage % 10);
        break;
    case MENU_ITEM_MAX_CURRENT:
        snprintf(buf, bufsz, "Max Curr: %u.%uA",
                 s_edit_current / 10, s_edit_current % 10);
        break;
    case MENU_ITEM_TARGET_SOC:
        snprintf(buf, bufsz, "Target SOC: %d%%", s_edit_soc);
        break;
    case MENU_ITEM_STOP_MODE:
        snprintf(buf, bufsz, "Stop: %s",
                 s_edit_stop_mode == STOP_MODE_VOLTAGE ? "Volt" :
                 s_edit_stop_mode == STOP_MODE_TIMER   ? "Timer" : "SOC");
        break;
    case MENU_ITEM_STOP_VOLTAGE:
        snprintf(buf, bufsz, "Stop V: %u.%uV",
                 s_edit_stop_voltage / 10, s_edit_stop_voltage % 10);
        break;
    case MENU_ITEM_CHARGE_TIMER:
        snprintf(buf, bufsz, "Timer: %um", s_edit_charge_timer);
        break;
    case MENU_ITEM_BEACON:
        snprintf(buf, bufsz, "LuxBeacon: %s",
                 s_edit_beacon ? "ON" : "OFF");
        break;
    case MENU_ITEM_WIFI_INFO: {
        char ip[20];
        network_svc_get_ip_str(ip, sizeof(ip));
        if (network_svc_is_ap_mode())
            snprintf(buf, bufsz, "AP: %s", ip);
        else if (ip[0] != '\0')
            snprintf(buf, bufsz, "IP: %s", ip);
        else
            snprintf(buf, bufsz, "WiFi: ---");
        break;
    }
    case MENU_ITEM_DEVICE_ID: {
        // 同網段有多台時，靠這個對照網頁上的 device id
        char host[24];
        network_svc_get_hostname(host, sizeof(host));
        snprintf(buf, bufsz, "%s", host[0] ? host : "tes-?");
        break;
    }
    case MENU_ITEM_PSU_STATUS: {
        const charger_config_t *cfg = config_svc_get();
        psu_status_t pst = psu_driver_get_status();
        if (cfg->psu_transport == PSU_TRANSPORT_ESPNOW) {
            if (!cfg->psu_paired)
                snprintf(buf, bufsz, "PSU NOW: NoPair");
            else if (pst.connected)
                snprintf(buf, bufsz, "PSU NOW:%ddBm", (int)pst.rssi);
            else
                snprintf(buf, bufsz, "PSU NOW: %s",
                         pst.fail_streak > 0 ? "TxFail" : "Lost");
        } else {
            snprintf(buf, bufsz, "PSU UART: %s", pst.connected ? "OK" : "---");
        }
        break;
    }
    case MENU_ITEM_SCHEDULER:
        snprintf(buf, bufsz, "Scheduler: %s",
                 s_edit_sched_enabled ? "ON" : "OFF");
        break;
    case MENU_ITEM_AUTO_START:
        snprintf(buf, bufsz, "[Beta]Auto: %s",
                 s_edit_auto_start ? "ON" : "OFF");
        break;
    case MENU_ITEM_RESET_FAULT:
        snprintf(buf, bufsz, "Reset Fault");
        break;
    case MENU_ITEM_ABOUT: {
        const esp_app_desc_t *app = esp_app_get_description();
        strncpy(buf, app->version, bufsz - 1);
        buf[bufsz - 1] = '\0';
        char *dirty = strstr(buf, "-dirty");
        if (dirty) *dirty = '\0';   // "-dirty" 超出 OLED 行寬，截掉
        break;
    }
    case MENU_ITEM_SAVE:
        snprintf(buf, bufsz, "Save & Exit");
        break;
    case MENU_ITEM_CANCEL:
        snprintf(buf, bufsz, "Cancel");
        break;
    default:
        snprintf(buf, bufsz, "---");
        break;
    }
}

static bool item_is_editable(int item)
{
    return (item == MENU_ITEM_AUTO_VOLTAGE  ||
            item == MENU_ITEM_MAX_VOLTAGE   ||
            item == MENU_ITEM_MAX_CURRENT   ||
            item == MENU_ITEM_TARGET_SOC    ||
            item == MENU_ITEM_STOP_MODE     ||
            item == MENU_ITEM_STOP_VOLTAGE  ||
            item == MENU_ITEM_CHARGE_TIMER  ||
            item == MENU_ITEM_BEACON        ||
            item == MENU_ITEM_SCHEDULER     ||
            item == MENU_ITEM_AUTO_START);
    // MENU_ITEM_WIFI_INFO, MENU_ITEM_ABOUT are display-only
}

static bool item_is_visible(int item)
{
    if (item == MENU_ITEM_TARGET_SOC)
        return s_edit_stop_mode == STOP_MODE_SOC;
    if (item == MENU_ITEM_STOP_VOLTAGE)
        return s_edit_stop_mode == STOP_MODE_VOLTAGE;
    if (item == MENU_ITEM_CHARGE_TIMER)
        return s_edit_stop_mode == STOP_MODE_TIMER;
    return true;
}

static void build_visible_list(void)
{
    s_visible_count = 0;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (item_is_visible(i))
            s_visible_items[s_visible_count++] = i;
    }
}

static void rebuild_and_fix_cursor(void)
{
    int actual = (s_cursor < s_visible_count) ? s_visible_items[s_cursor] : MENU_ITEM_SAVE;
    build_visible_list();
    for (int i = 0; i < s_visible_count; i++) {
        if (s_visible_items[i] == actual) {
            s_cursor = i;
            break;
        }
    }
    if (s_cursor < s_scroll_top)
        s_scroll_top = s_cursor;
    if (s_cursor >= s_scroll_top + MAX_VISIBLE_ROWS)
        s_scroll_top = s_cursor - MAX_VISIBLE_ROWS + 1;
}

// delta > 0 = up, delta < 0 = down
// fine  step: |delta|=1  → 0.1 V / 0.1 A / 1 %
// coarse step: |delta|=10 → 1 V   / 1 A   / 5 %
static void value_step(int item, int delta)
{
    switch ((menu_item_id_t)item) {
    case MENU_ITEM_MAX_VOLTAGE: {
        int v = (int)s_edit_voltage + delta;
        if (v < 400)  v = 400;
        if (v > 1200) v = 1200;
        s_edit_voltage = (uint16_t)v;
        break;
    }
    case MENU_ITEM_MAX_CURRENT: {
        int a = (int)s_edit_current + delta;
        if (a < 10)   a = 10;
        if (a > 1000) a = 1000;
        s_edit_current = (uint16_t)a;
        break;
    }
    case MENU_ITEM_TARGET_SOC: {
        // coarse |delta|=10 → 5 %; fine |delta|=1 → 1 %
        int soc_d = (delta >= 10) ? 5 : (delta <= -10) ? -5 : delta;
        int s = (int)s_edit_soc + soc_d;
        if (s < 20)  s = 20;
        if (s > 100) s = 100;
        s_edit_soc = (int8_t)s;
        break;
    }
    case MENU_ITEM_AUTO_VOLTAGE:
        s_edit_auto_voltage = !s_edit_auto_voltage;
        break;
    case MENU_ITEM_STOP_MODE:
        if (s_edit_stop_mode == STOP_MODE_SOC)
            s_edit_stop_mode = STOP_MODE_VOLTAGE;
        else if (s_edit_stop_mode == STOP_MODE_VOLTAGE)
            s_edit_stop_mode = STOP_MODE_TIMER;
        else
            s_edit_stop_mode = STOP_MODE_SOC;
        break;
    case MENU_ITEM_STOP_VOLTAGE: {
        int v = (int)s_edit_stop_voltage + delta;
        if (v < 400)  v = 400;
        if (v > 1200) v = 1200;
        s_edit_stop_voltage = (uint16_t)v;
        break;
    }
    case MENU_ITEM_CHARGE_TIMER: {
        // coarse |delta|=10 → 30 min; fine |delta|=1 → 10 min
        int step = (delta >= 10 || delta <= -10) ? (delta > 0 ? 30 : -30) : (delta > 0 ? 10 : -10);
        int t = (int)s_edit_charge_timer + step;
        if (t < 1)   t = 1;
        if (t > 600) t = 600;
        s_edit_charge_timer = (uint16_t)t;
        break;
    }
    case MENU_ITEM_BEACON:
        s_edit_beacon = !s_edit_beacon;
        break;
    case MENU_ITEM_SCHEDULER:
        s_edit_sched_enabled = !s_edit_sched_enabled;
        break;
    case MENU_ITEM_AUTO_START:
        s_edit_auto_start = !s_edit_auto_start;
        break;
    default:
        break;
    }
}

// Quick SOC presets cycled by short SETTING press on status screen
static void cycle_quick_soc(void)
{
    static const int8_t presets[] = {80, 95, 100};
    const int n = (int)(sizeof(presets) / sizeof(presets[0]));
    const charger_config_t *cfg = config_svc_get();
    int8_t cur  = cfg->target_soc;
    int8_t next = presets[0];
    for (int i = 0; i < n; i++) {
        if (cur == presets[i]) {
            next = presets[(i + 1) % n];
            break;
        }
    }
    config_svc_set_charging(cfg->max_voltage_01v, cfg->max_current_01a, next);
    ESP_LOGI(TAG, "quick SOC -> %d%%", next);
}

// ── Status screen render ──────────────────────────────────────────────────────

static const char *state_name(tes_state_t state)
{
    switch (state) {
    case TES_STATE_IDLE:           return "STANDBY";
    case TES_STATE_PARAM_EXCHANGE: return "CONNECTING";
    case TES_STATE_PRE_CHARGE:     return "PRE-CHARGE";
    case TES_STATE_CHARGING:       return "CHARGING";
    case TES_STATE_ENDING:         return "ENDING";
    case TES_STATE_FAULT:          return "FAULT";
    case TES_STATE_EMERGENCY:      return "EMERGENCY";
    case TES_STATE_FINALIZE:       return "FINALIZING";
    default:                       return "---";
    }
}

// 故障說明。OLED 只有 ASCII 字型（u8g2 *_tr），所以維持英文；
// 中文的完整說明與處置建議在網頁 UI。
//   l1 = 發生什麼事（6x10 字型，約 21 字）
//   l2 = 該怎麼辦（5x8 字型，約 25 字）
//   detail 由 fault_detail() 依 fault_ctx_a/b 組出實際數值
static void fault_src_text(uint8_t src, uint16_t ctx_a, const char **l1, const char **l2)
{
    switch ((fault_source_t)src) {
    case FAULT_SRC_VEHICLE_TIMEOUT:
        *l1 = "No CAN permit";      *l2 = "Check CAN wiring / plug"; break;
    case FAULT_SRC_VOLTAGE_INCOMPAT:
        *l1 = "Volt limit too low"; *l2 = "Raise Max Voltage";       break;
    case FAULT_SRC_PRECHARGE_TIMEOUT:
        *l1 = "Pre-charge timeout"; *l2 = "Re-plug the connector";   break;
    case FAULT_SRC_CONTACTOR_TIMEOUT:
        *l1 = "Car contactor open"; *l2 = "Vehicle-side issue";      break;
    case FAULT_SRC_PSU_LOST:
        *l1 = "PSU disconnected";   *l2 = "Check PSU power / link";  break;
    case FAULT_SRC_BMS_FAULT:
        // bit0 = 供電系統異常：車輛指控的是充電樁，不是它自己的電池，
        // 兩者的處置完全不同，OLED 上就要分開講。
        if (ctx_a & V500_FAULT_SUPPLY_SYSTEM) {
            *l1 = "Car blames charger"; *l2 = "Supply fault reported";
        } else {
            *l1 = "Vehicle battery";    *l2 = "fault - see dashboard";
        }
        break;
    case FAULT_SRC_CP_LOST:
        *l1 = "Connector loose";    *l2 = "Re-seat and lock it";     break;
    case FAULT_SRC_EMERGENCY_BTN:
        *l1 = "E-STOP pressed";     *l2 = "Menu > Reset Fault";      break;
    case FAULT_SRC_EMERGENCY_VEHICLE:
        *l1 = "Vehicle E-stop";     *l2 = "Normal after charge end"; break;
    default:
        *l1 = "Unknown fault";      *l2 = "";                        break;
    }
}

// 把 fault_ctx_a/b 轉成該故障看得懂的一行數值
static void fault_detail(const tes_snapshot_t *s, char *buf, size_t bufsz)
{
    uint16_t a = s->fault_ctx_a, b = s->fault_ctx_b;
    switch ((fault_source_t)s->fault_source) {
    case FAULT_SRC_VOLTAGE_INCOMPAT:
        snprintf(buf, bufsz, "car %.1fV > set %.1fV", a / 10.0f, b / 10.0f); break;
    case FAULT_SRC_VEHICLE_TIMEOUT:
    case FAULT_SRC_CONTACTOR_TIMEOUT:
        snprintf(buf, bufsz, "waited %us  st:0x%02X", (unsigned)a, (unsigned)b); break;
    case FAULT_SRC_PRECHARGE_TIMEOUT:
        snprintf(buf, bufsz, "CP %.1fV  st:0x%02X", a / 10.0f, (unsigned)b); break;
    case FAULT_SRC_PSU_LOST:
        snprintf(buf, bufsz, "after %us of charging", (unsigned)a); break;
    case FAULT_SRC_BMS_FAULT: {
        // 0x500 byte0 的位元名稱（TES-0D-02-01 表 16）。多個同時成立時只顯示
        // 最低位的那一個 —— OLED 一行放不下，完整清單在網頁。
        static const char *n[] = {
            "supply system", "batt overvolt", "batt undervolt",
            "current diff",  "batt overtemp", "volt diff"
        };
        const char *w = NULL;
        for (int i = 0; i < 6; i++) if (a & (1u << i)) { w = n[i]; break; }
        if (w) snprintf(buf, bufsz, "%s (0x%02X)", w, (unsigned)a);
        else   snprintf(buf, bufsz, "flags:0x%02X st:0x%02X", (unsigned)a, (unsigned)b);
        break;
    }
    case FAULT_SRC_CP_LOST:
        snprintf(buf, bufsz, "CP %.1fV", a / 10.0f); break;
    case FAULT_SRC_EMERGENCY_VEHICLE:
        snprintf(buf, bufsz, "auto-clears in 5s"); break;
    default:
        buf[0] = '\0'; break;
    }
}

static void render_status(const tes_snapshot_t *snap)
{
    char buf[24];
    display_driver_clear();
    display_driver_set_color(1);

    // 故障詳情畫面：FAULT/EMERGENCY 期間，以及故障後仍停在 IDLE 的情況。
    // （舊版只判斷 IDLE && fault_latched —— 但狀態機離開 FAULT 時一定會清掉
    //   fault_latched，所以那個條件永遠不成立，整個畫面等於死碼。）
    if (snap->state == TES_STATE_FAULT || snap->state == TES_STATE_EMERGENCY ||
        (snap->state == TES_STATE_IDLE && snap->fault_latched)) {
        const char *l1, *l2;
        char detail[26];
        fault_src_text(snap->fault_source, snap->fault_ctx_a, &l1, &l2);
        fault_detail(snap, detail, sizeof(detail));

        display_driver_font_bold();
        display_driver_draw_str(0, 12,
            snap->state == TES_STATE_EMERGENCY ? "EMERGENCY" : "FAULT STOP");
        display_driver_draw_hline(0, 15, 128);

        // 先講「發生什麼事」和「怎麼辦」，故障碼降級成最後一行的參考值
        display_driver_font_medium();
        display_driver_draw_str(0, 27, l1);
        display_driver_font_small();
        display_driver_draw_str(0, 38, l2);
        if (detail[0]) display_driver_draw_str(0, 48, detail);

        snprintf(buf, sizeof(buf), "0x%02X", snap->last_fault_flags);
        display_driver_draw_str(0, 60, buf);
        display_driver_draw_str(30, 60, "START:retry L:menu");
        display_driver_flush();
        return;
    }

    display_driver_font_bold();
    display_driver_draw_str(0, 12, state_name(snap->state));
    display_driver_draw_hline(0, 15, 128);

    const charger_config_t *cfg = config_svc_get();

    display_driver_font_medium();
    if (cfg->stop_mode == STOP_MODE_VOLTAGE) {
        // Volt 模式：行 2 = 即時電壓/目標停止電壓，行 3 = SOC（純參考）+ 剩餘時間
        float stop_v = cfg->stop_voltage_01v / 10.0f;
        snprintf(buf, sizeof(buf), "%.1fV/%.1fV",
                 snap->output_voltage, stop_v);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d%%", snap->soc);
        display_driver_draw_str(0, 42, buf);
        uint32_t rem_min = snap->timer_running
            ? (snap->remaining_seconds + 30) / 60 : 0;
        snprintf(buf, sizeof(buf), "%luh%02lum", rem_min / 60, rem_min % 60);
        display_driver_draw_str(80, 42, buf);
    } else if (cfg->stop_mode == STOP_MODE_TIMER) {
        // Timer 模式：行 2 = 電壓/電流，行 3 = SOC + 已充/目標時間
        snprintf(buf, sizeof(buf), "%.1fV  %.1fA",
                 snap->output_voltage, snap->output_current);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d%%", snap->soc);
        display_driver_draw_str(0, 42, buf);
        uint32_t el_min = snap->elapsed_seconds / 60;
        snprintf(buf, sizeof(buf), "%um/%um",
                 (unsigned)el_min, (unsigned)cfg->charge_timer_min);
        display_driver_draw_str(60, 42, buf);
    } else {
        // SOC 模式：行 2 = 電壓/電流，行 3 = SOC 現在/目標 + 剩餘時間
        snprintf(buf, sizeof(buf), "%.1fV  %.1fA",
                 snap->output_voltage, snap->output_current);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d/%d%%", snap->soc, (int)snap->target_soc);
        display_driver_draw_str(0, 42, buf);
        uint32_t rem_min = snap->timer_running
            ? (snap->remaining_seconds + 30) / 60 : 0;
        snprintf(buf, sizeof(buf), "%luh%02lum", rem_min / 60, rem_min % 60);
        display_driver_draw_str(80, 42, buf);
    }

    // FAULT / EMERGENCY 已在函式開頭走專屬畫面，這裡只會是正常運作狀態
    display_driver_font_small();
    if (snap->vehicle_req_voltage > 0.5f || snap->vehicle_req_current > 0.1f) {
        snprintf(buf, sizeof(buf), "REQ:%.0fV %.0fA",
                 snap->vehicle_req_voltage, snap->vehicle_req_current);
    } else {
        snprintf(buf, sizeof(buf), "REQ: --V --A");
    }
    display_driver_draw_str(0, 56, buf);
    display_driver_draw_str(68, 56, "S:SOC L:CFG");

    display_driver_flush();
}

// ── Menu screen render ────────────────────────────────────────────────────────

static void render_menu(void)
{
    char buf[24];
    display_driver_clear();
    display_driver_set_color(1);

    // Header
    display_driver_font_bold();
    if (s_mode == MENU_MODE_EDIT)
        display_driver_draw_str(0, 12, "< EDIT VALUE >");
    else
        display_driver_draw_str(0, 12, "SETTINGS");
    display_driver_draw_hline(0, 15, 128);

    // Items: 4 visible rows, each 12px tall
    display_driver_font_small();
    for (int row = 0; row < MAX_VISIBLE_ROWS; row++) {
        int vis = s_scroll_top + row;
        if (vis >= s_visible_count) break;
        int item = s_visible_items[vis];

        int box_y  = HEADER_H + row * ROW_H;
        int text_y = box_y + ROW_TEXT_OFS;
        bool selected = (vis == s_cursor);

        item_label(item, buf, sizeof(buf));

        if (selected) {
            // Highlighted row: white box, black text
            display_driver_set_color(1);
            display_driver_draw_box(0, box_y, 128, ROW_H);
            display_driver_set_color(0);
            display_driver_draw_str(2, text_y, buf);
            display_driver_set_color(1);
        } else {
            display_driver_draw_str(2, text_y, buf);
        }
    }

    // Scroll arrows (right side) when there are hidden items
    if (s_scroll_top > 0)
        display_driver_draw_str(120, HEADER_H + ROW_TEXT_OFS, "^");
    if (s_scroll_top + MAX_VISIBLE_ROWS < s_visible_count)
        display_driver_draw_str(120, HEADER_H + (MAX_VISIBLE_ROWS - 1) * ROW_H + ROW_TEXT_OFS, "v");

    display_driver_flush();
}

// ── Public API ────────────────────────────────────────────────────────────────

void display_svc_init(void)
{
    s_screen    = DISP_SCREEN_STATUS;
    g_menu_open = false;
    ESP_LOGI(TAG, "init");
}

void display_svc_button(uint8_t evt)
{
    if (s_screen == DISP_SCREEN_STATUS) {
        switch (evt) {
        case EVT_BUTTON_SETTING:      // short press → cycle quick SOC preset
            cycle_quick_soc();
            break;
        case EVT_BUTTON_SETTING_LONG: // long press → open settings menu
            menu_open();
            break;
        default:
            break;
        }
        return;
    }

    // ── In MENU screen ────────────────────────────────────────────────────────
    if (s_mode == MENU_MODE_NAV) {
        switch (evt) {
        case EVT_BUTTON_START:   // scroll UP
            if (s_cursor > 0) {
                s_cursor--;
                if (s_cursor < s_scroll_top)
                    s_scroll_top = s_cursor;
            }
            break;

        case EVT_BUTTON_STOP:    // scroll DOWN
            if (s_cursor < s_visible_count - 1) {
                s_cursor++;
                if (s_cursor >= s_scroll_top + MAX_VISIBLE_ROWS)
                    s_scroll_top = s_cursor - MAX_VISIBLE_ROWS + 1;
            }
            break;

        case EVT_BUTTON_SETTING: // CONFIRM / execute
            {
                int actual = s_visible_items[s_cursor];
                if (actual == MENU_ITEM_SAVE) {
                    menu_save();
                    menu_close();
                } else if (actual == MENU_ITEM_CANCEL) {
                    menu_close();
                } else if (actual == MENU_ITEM_RESET_FAULT) {
                    uint8_t evt = (uint8_t)EVT_FAULT_CLEAR;
                    xQueueSend(g_btn_event_queue, &evt, 0);
                    ESP_LOGI(TAG, "fault clear sent");
                    menu_close();
                } else if (item_is_editable(actual)) {
                    s_mode = MENU_MODE_EDIT;
                }
            }
            break;

        default:
            break;
        }
    } else { // MENU_MODE_EDIT
        switch (evt) {
        case EVT_BUTTON_START:      value_step(s_visible_items[s_cursor], +1);  rebuild_and_fix_cursor(); break;
        case EVT_BUTTON_STOP:       value_step(s_visible_items[s_cursor], -1);  rebuild_and_fix_cursor(); break;
        case EVT_BUTTON_START_LONG: value_step(s_visible_items[s_cursor], +10); rebuild_and_fix_cursor(); break;
        case EVT_BUTTON_STOP_LONG:  value_step(s_visible_items[s_cursor], -10); rebuild_and_fix_cursor(); break;
        case EVT_BUTTON_SETTING:    s_mode = MENU_MODE_NAV;    break; // confirm, back to nav
        default:
            break;
        }
    }
}

void display_svc_tick(void)
{
    tes_snapshot_t snap;
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snap = g_snapshot;
        xSemaphoreGive(g_snapshot_mutex);
    } else {
        return;
    }

    // LED 狀態直接採用狀態機算好的值。
    // 舊版在這裡重算一次，但用的是 snap.charge_complete —— 那個旗標在使用者
    // 手動停止時也會被設起（它同時是 auto_start 的重入防護），導致中途停止
    // 也亮「充電完成」綠燈。tes_sm_get_snapshot() 已依 stop_reason 判斷。
    led_state_t led = (snap.state == TES_STATE_FAULT || snap.state == TES_STATE_EMERGENCY)
                      ? LED_STATE_FAULT
                      : snap.led_state;
    led_driver_set_state(led);

    const charger_config_t *cfg = config_svc_get();
    led_driver_set_beacon_enable(cfg->beacon_unlocked);
    led_driver_set_beacon_soc(snap.soc);
    // ESP-NOW 配對但失連 → STANDBY 下每 5s 短閃紅燈
    led_driver_set_psu_warn(cfg->psu_transport == PSU_TRANSPORT_ESPNOW
                            && psu_driver_has_peer()
                            && !psu_driver_get_status().connected);
    led_driver_tick();

    if (!display_driver_is_ok()) return;

    switch (s_screen) {
    case DISP_SCREEN_STATUS:
        render_status(&snap);
        break;
    case DISP_SCREEN_MENU:
        render_menu();
        break;
    case DISP_SCREEN_FAULT:
        render_status(&snap);   // fault detail reuses status screen for now
        break;
    }
}
