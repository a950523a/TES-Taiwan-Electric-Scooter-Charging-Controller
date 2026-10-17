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
#include "drivers/adc_driver.h"
#include "tes_protocol/tes_sm.h"
#include "services/event_bus.h"   // for EVT_BUTTON_* enum values
#include "services/hwtest_svc.h"
#include "services/restart_svc.h"
#include "psu_link/psu_pair.h"
#include "hal/hal_gpio.h"
#include "platform/platform.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "display_svc";

// Globals owned by main.c — accessed via extern (same pattern as g_snapshot)
extern tes_snapshot_t    g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;
extern volatile bool     g_menu_open;
extern QueueHandle_t     g_btn_event_queue;

extern volatile bool     g_screen_saver;
extern volatile bool     g_screen_wake;
extern volatile uint8_t  g_btn_stable;

// ── 語言（2026-10-17）─────────────────────────────────────────────────────────
// OLED 可以選中文或英文（設定 oled_lang）。中文字串一律寫成 ZH("...")，字型只含這些字
// （tools/gen_oled_font.py）。數字（V／A／kWh／SOC）兩種語言都用原本的英文字型
static bool lang_zh(void) { return config_svc_get()->oled_lang == OLED_LANG_ZH; }
#define L(en, zh) (lang_zh() ? (zh) : (en))

// 一行文字：中文用 12 px 中文字型，英文用指定的英文字型
typedef enum { EN_SMALL, EN_MEDIUM, EN_BOLD } en_font_t;
static void text(int x, int y, en_font_t ef, const char *en, const char *zh)
{
    if (lang_zh()) {
        display_driver_font_zh();
        display_driver_draw_utf8(x, y, zh);
    } else {
        if (ef == EN_SMALL)       display_driver_font_small();
        else if (ef == EN_MEDIUM) display_driver_font_medium();
        else                      display_driver_font_bold();
        display_driver_draw_str(x, y, en);
    }
}

// 標題列：狀態名稱＋分隔線（兩種語言共用的位置）
static void header(const char *en, const char *zh)
{
    text(0, 12, EN_BOLD, en, zh);
    display_driver_draw_hline(0, 15, 128);
}

// ── Screen state ──────────────────────────────────────────────────────────────

static disp_screen_t s_screen = DISP_SCREEN_STATUS;

// ── Menu state ────────────────────────────────────────────────────────────────

typedef enum {
    MENU_ITEM_AUTO_VOLTAGE = 0,
    MENU_ITEM_MAX_VOLTAGE,
    MENU_ITEM_MAX_CURRENT,
    MENU_ITEM_STOP_MODE,      // 停止條件：SOC / Voltage / Timer / kWh
    MENU_ITEM_TARGET_SOC,    // 停止條件值（stop_mode=SOC 時顯示）
    MENU_ITEM_STOP_VOLTAGE,  // 停止條件值（stop_mode=VOLTAGE 時顯示）
    MENU_ITEM_CHARGE_TIMER,  // 充電時長（stop_mode=TIMER 時顯示，分鐘）
    MENU_ITEM_STOP_ENERGY,   // 充入電量目標（stop_mode=ENERGY 時顯示，0.1–20.0 kWh）
    MENU_ITEM_BEACON,
    MENU_ITEM_WIFI_INFO,
    MENU_ITEM_AP_PASS,       // 自動熱點的密碼（唯讀；連不上 WiFi 時手機用它連進來）
    MENU_ITEM_DEVICE_ID,     // 本機 mDNS 主機名 tes-<id>（唯讀，多台辨識用）
    MENU_ITEM_BOARD,         // 硬體版本（唯讀，開機時由 AIN3 辨識）
    MENU_ITEM_PSU_STATUS,    // PSU 連線狀態（唯讀）
    MENU_ITEM_SCHEDULER,     // 定時充電 ON/OFF（時間設定僅 Web UI）
    MENU_ITEM_AUTO_START,    // Beta: VP 常通 + 自動觸發充電
    MENU_ITEM_LANG,          // OLED 語言：中文 / English
    MENU_ITEM_CHG_ANIM,      // 充電中動畫：全部 / 電池 / 箭頭 / 關
    MENU_ITEM_SAVER,         // 螢幕保護等待：5 / 15 / 30 分 / 關
    MENU_ITEM_SAVER_STYLE,   // 螢幕保護樣式：烏龜 / 全黑（螢幕保護關閉時不顯示）
    MENU_ITEM_RESET_FAULT,   // 手動復歸緊急停止（設定選單確認才有效）
    MENU_ITEM_RESTART,       // 重新啟動：按兩次確認；充電流程中不可用（restart_svc）
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
static uint16_t    s_edit_stop_energy;    // Wh
static bool        s_edit_beacon;
static bool        s_edit_sched_enabled;
static bool        s_edit_auto_start;
static uint8_t     s_edit_lang, s_edit_anim, s_edit_ss_min, s_edit_ss_style;

// 重新啟動要按兩次：第一次只是「武裝」，3 秒內再按 SETTING 才執行；移動游標就取消。
// 選單裡捲動時一不小心按到 SETTING 不該讓機器重開。
#define RESTART_CONFIRM_MS 3000u
static bool     s_restart_armed;
static uint32_t s_restart_armed_ms;

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
    s_restart_armed       = false;
    s_edit_auto_voltage   = cfg->auto_voltage;
    s_edit_voltage        = cfg->max_voltage_01v;
    s_edit_current        = cfg->max_current_01a;
    s_edit_soc            = cfg->target_soc;
    s_edit_stop_mode      = cfg->stop_mode;
    s_edit_stop_voltage   = cfg->stop_voltage_01v;
    s_edit_charge_timer   = cfg->charge_timer_min;
    s_edit_stop_energy    = cfg->stop_energy_wh;
    s_edit_beacon         = cfg->beacon_unlocked;
    s_edit_sched_enabled  = cfg->sched_enabled;
    s_edit_auto_start     = cfg->auto_start;
    s_edit_lang           = cfg->oled_lang;
    s_edit_anim           = cfg->oled_anim;
    s_edit_ss_min         = cfg->ss_min;
    s_edit_ss_style       = cfg->ss_style;
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
    config_svc_set_stop_energy(s_edit_stop_energy);   // 目標先存，再切模式
    config_svc_set_stop(s_edit_stop_mode, s_edit_stop_voltage, s_edit_charge_timer);
    config_svc_set_beacon(s_edit_beacon);
    {
        const charger_config_t *cfg = config_svc_get();
        config_svc_set_scheduler(s_edit_sched_enabled, cfg->sched_start_min,
                                  cfg->sched_stop_en, cfg->sched_stop_min);
    }
    config_svc_set_auto_start(s_edit_auto_start);
    config_svc_set_oled(s_edit_lang, s_edit_anim, s_edit_ss_min, s_edit_ss_style);
    ESP_LOGI(TAG, "saved: auto_v=%d %u.%uV %u.%uA SOC=%d stop=%d stpV=%u.%u timer=%um beacon=%d",
             (int)s_edit_auto_voltage,
             s_edit_voltage / 10, s_edit_voltage % 10,
             s_edit_current / 10, s_edit_current % 10,
             s_edit_soc, (int)s_edit_stop_mode,
             s_edit_stop_voltage / 10, s_edit_stop_voltage % 10,
             s_edit_charge_timer, (int)s_edit_beacon);
}

// 選單一行：英文 5x8 約 25 字；中文 12 px 一行 10 個中文字（半形英數 6 px）
static void item_label(int item, char *buf, size_t bufsz)
{
    switch ((menu_item_id_t)item) {
    case MENU_ITEM_AUTO_VOLTAGE:
        snprintf(buf, bufsz, "%s%s", L("Auto Volt: ", ZH("自動電壓: ")),
                 s_edit_auto_voltage ? L("ON", ZH("開")) : L("OFF", ZH("關")));
        break;
    case MENU_ITEM_MAX_VOLTAGE:
        snprintf(buf, bufsz, "%s%u.%uV",
                 s_edit_auto_voltage ? L("V Cap: ", ZH("電壓上限: ")) : L("Max Volt: ", ZH("最大電壓: ")),
                 s_edit_voltage / 10, s_edit_voltage % 10);
        break;
    case MENU_ITEM_MAX_CURRENT:
        snprintf(buf, bufsz, "%s%u.%uA", L("Max Curr: ", ZH("最大電流: ")),
                 s_edit_current / 10, s_edit_current % 10);
        break;
    case MENU_ITEM_TARGET_SOC:
        snprintf(buf, bufsz, "%s%d%%", L("Target SOC: ", ZH("目標SOC: ")), s_edit_soc);
        break;
    case MENU_ITEM_STOP_MODE:
        snprintf(buf, bufsz, "%s%s", L("Stop: ", ZH("停止條件: ")),
                 s_edit_stop_mode == STOP_MODE_VOLTAGE ? L("Volt",  ZH("電壓")) :
                 s_edit_stop_mode == STOP_MODE_TIMER   ? L("Timer", ZH("時間")) :
                 s_edit_stop_mode == STOP_MODE_ENERGY  ? L("kWh",   ZH("電量")) : "SOC");
        break;
    case MENU_ITEM_STOP_VOLTAGE:
        snprintf(buf, bufsz, "%s%u.%uV", L("Stop V: ", ZH("停止電壓: ")),
                 s_edit_stop_voltage / 10, s_edit_stop_voltage % 10);
        break;
    case MENU_ITEM_CHARGE_TIMER:
        snprintf(buf, bufsz, "%s%u%s", L("Timer: ", ZH("充電時長: ")), s_edit_charge_timer,
                 L("m", ZH("分")));
        break;
    case MENU_ITEM_STOP_ENERGY:
        snprintf(buf, bufsz, "%s%u.%ukWh", L("Stop E: ", ZH("目標電量: ")),
                 s_edit_stop_energy / 1000, (s_edit_stop_energy % 1000) / 100);
        break;
    case MENU_ITEM_BEACON:
        snprintf(buf, bufsz, "LuxBeacon: %s", s_edit_beacon ? L("ON", ZH("開")) : L("OFF", ZH("關")));
        break;
    case MENU_ITEM_WIFI_INFO: {
        char ip[20];
        network_svc_get_ip_str(ip, sizeof(ip));
        // 熱點可能跟 WiFi 同時開著：連著 WiFi 就顯示那個位址
        if (network_svc_is_connected())
            snprintf(buf, bufsz, "IP: %s", ip);
        else if (network_svc_is_ap_mode())
            snprintf(buf, bufsz, "AP: %s", ip);
        else
            snprintf(buf, bufsz, "WiFi: ---");
        break;
    }
    case MENU_ITEM_AP_PASS: {
        char pass[WIFI_PASS_MAX_LEN + 1];
        config_svc_get_ap_pass(pass);
        snprintf(buf, bufsz, "%s%s", L("AP pass: ", ZH("熱點密碼:")), pass);
        break;
    }
    case MENU_ITEM_DEVICE_ID: {
        // 同網段有多台時，靠這個對照網頁上的 device id
        char host[24];
        network_svc_get_hostname(host, sizeof(host));
        snprintf(buf, bufsz, "%s", host[0] ? host : "tes-?");
        break;
    }
    case MENU_ITEM_BOARD: {
        const adc_board_t *hw = adc_driver_board();
        const char *b = L("Board: ", ZH("硬體: "));
        if (hw->id_mismatch)   // 出廠紀錄和 AIN3 不同：版本照出廠紀錄，ID 電阻要檢查
            snprintf(buf, bufsz, "%s%s ID!", b, hw->name);
        else if (hw->known)
            snprintf(buf, bufsz, "%s%s", b, hw->name);
        else
            snprintf(buf, bufsz, "%s? %.2fV", b, (double)hw->id_volts);
        break;
    }
    case MENU_ITEM_PSU_STATUS: {
        const charger_config_t *cfg = config_svc_get();
        psu_status_t pst = psu_driver_get_status();
        if (cfg->psu_transport == PSU_TRANSPORT_ESPNOW) {
            const char *p = L("PSU NOW: ", ZH("電源無線: "));
            if (!cfg->psu_paired)
                snprintf(buf, bufsz, "%s%s", p, L("NoPair", ZH("未配對")));
            else if (pst.connected)
                snprintf(buf, bufsz, "%s%ddBm", p, (int)pst.rssi);
            else
                snprintf(buf, bufsz, "%s%s", p,
                         pst.fail_streak > 0 ? L("TxFail", ZH("傳送失敗")) : L("Lost", ZH("失聯")));
        } else {
            snprintf(buf, bufsz, "%s%s", L("PSU UART: ", ZH("電源有線: ")),
                     pst.connected ? "OK" : "---");
        }
        break;
    }
    case MENU_ITEM_SCHEDULER:
        snprintf(buf, bufsz, "%s%s", L("Scheduler: ", ZH("定時充電: ")),
                 s_edit_sched_enabled ? L("ON", ZH("開")) : L("OFF", ZH("關")));
        break;
    case MENU_ITEM_AUTO_START:
        snprintf(buf, bufsz, "%s%s", L("[Beta]Auto: ", ZH("[Beta]自動充電: ")),
                 s_edit_auto_start ? L("ON", ZH("開")) : L("OFF", ZH("關")));
        break;
    case MENU_ITEM_LANG:
        // 兩種語言都用「本身的語言」寫，選錯了也認得出要切回哪一個
        snprintf(buf, bufsz, "%s", s_edit_lang == OLED_LANG_ZH ? ZH("語言: 中文") : "Language: English");
        break;
    case MENU_ITEM_CHG_ANIM: {
        static const char *const en[] = { "All", "Battery", "Arrows", "OFF" };
        static const char *const zh[] = { ZH("全部"), ZH("電池"), ZH("箭頭"), ZH("關") };
        snprintf(buf, bufsz, "%s%s", L("Charge anim: ", ZH("充電動畫: ")),
                 L(en[s_edit_anim & 3], zh[s_edit_anim & 3]));
        break;
    }
    case MENU_ITEM_SAVER:
        if (s_edit_ss_min)
            snprintf(buf, bufsz, "%s%u%s", L("Screensaver: ", ZH("螢幕保護: ")), s_edit_ss_min,
                     L("min", ZH("分鐘")));
        else
            snprintf(buf, bufsz, "%s%s", L("Screensaver: ", ZH("螢幕保護: ")), L("OFF", ZH("關")));
        break;
    case MENU_ITEM_SAVER_STYLE:
        snprintf(buf, bufsz, "%s%s", L("Saver style: ", ZH("保護樣式: ")),
                 s_edit_ss_style == OLED_SS_DARK ? L("Dark", ZH("全黑")) : L("Turtle", ZH("烏龜")));
        break;
    case MENU_ITEM_RESET_FAULT:
        snprintf(buf, bufsz, "%s", L("Reset Fault", ZH("復歸故障")));
        break;
    case MENU_ITEM_RESTART:
        if (!restart_svc_allowed())
            snprintf(buf, bufsz, "%s", L("Restart (stop first)", ZH("重新啟動(請先停止)")));
        else if (s_restart_armed)
            snprintf(buf, bufsz, "%s", L("Restart? press again", ZH("再按一次重新啟動")));
        else
            snprintf(buf, bufsz, "%s", L("Restart", ZH("重新啟動")));
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
        snprintf(buf, bufsz, "%s", L("Save & Exit", ZH("儲存並離開")));
        break;
    case MENU_ITEM_CANCEL:
        snprintf(buf, bufsz, "%s", L("Cancel", ZH("取消")));
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
            item == MENU_ITEM_STOP_ENERGY   ||
            item == MENU_ITEM_BEACON        ||
            item == MENU_ITEM_SCHEDULER     ||
            item == MENU_ITEM_AUTO_START    ||
            item == MENU_ITEM_LANG          ||
            item == MENU_ITEM_CHG_ANIM      ||
            item == MENU_ITEM_SAVER         ||
            item == MENU_ITEM_SAVER_STYLE);
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
    if (item == MENU_ITEM_STOP_ENERGY)
        return s_edit_stop_mode == STOP_MODE_ENERGY;
    if (item == MENU_ITEM_SAVER_STYLE)
        return s_edit_ss_min != 0;
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
        int vmax = adc_driver_board()->max_volt_01v;   // 舊板 110 V、V1.3 120 V
        if (v < 400)  v = 400;
        if (v > vmax) v = vmax;
        s_edit_voltage = (uint16_t)v;
        break;
    }
    case MENU_ITEM_MAX_CURRENT: {
        int a = (int)s_edit_current + delta;
        // 接上會回報額定電流的電源（LianMing 66 A）時，最多調到那裡
        int amax = psu_driver_rated_current_01a();
        if (amax < 10 || amax > 1000) amax = 1000;
        if (a < 10)   a = 10;
        if (a > amax) a = amax;
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
        else if (s_edit_stop_mode == STOP_MODE_TIMER)
            s_edit_stop_mode = STOP_MODE_ENERGY;
        else
            s_edit_stop_mode = STOP_MODE_SOC;
        break;
    case MENU_ITEM_STOP_VOLTAGE: {
        int v = (int)s_edit_stop_voltage + delta;
        int vmax = adc_driver_board()->max_volt_01v;
        if (v < 400)  v = 400;
        if (v > vmax) v = vmax;
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
    case MENU_ITEM_STOP_ENERGY: {
        // coarse |delta|=10 → 1 kWh; fine |delta|=1 → 0.1 kWh
        int e = (int)s_edit_stop_energy + delta * 100;
        if (e < STOP_ENERGY_MIN_WH) e = STOP_ENERGY_MIN_WH;
        if (e > STOP_ENERGY_MAX_WH) e = STOP_ENERGY_MAX_WH;
        s_edit_stop_energy = (uint16_t)e;
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
    case MENU_ITEM_LANG:
        s_edit_lang = s_edit_lang == OLED_LANG_ZH ? OLED_LANG_EN : OLED_LANG_ZH;
        break;
    case MENU_ITEM_CHG_ANIM:   // 上 = 下一個，下 = 上一個
        s_edit_anim = (uint8_t)((s_edit_anim + (delta > 0 ? 1 : 3)) % 4);
        break;
    case MENU_ITEM_SAVER: {
        static const uint8_t opts[] = { 5, 15, 30, 0 };
        int i = 0;
        while (i < 4 && opts[i] != s_edit_ss_min) i++;
        s_edit_ss_min = opts[((i % 4) + (delta > 0 ? 1 : 3)) % 4];
        break;
    }
    case MENU_ITEM_SAVER_STYLE:
        s_edit_ss_style = s_edit_ss_style == OLED_SS_DARK ? OLED_SS_TURTLE : OLED_SS_DARK;
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
    case TES_STATE_IDLE:           return L("STANDBY",    ZH("待機中"));
    case TES_STATE_PARAM_EXCHANGE: return L("CONNECTING", ZH("連線中"));
    case TES_STATE_PRE_CHARGE:     return L("PRE-CHARGE", ZH("準備充電"));
    case TES_STATE_CHARGING:       return L("CHARGING",   ZH("充電中"));
    case TES_STATE_ENDING:         return L("ENDING",     ZH("結束中"));
    case TES_STATE_FAULT:          return L("FAULT",      ZH("故障"));
    case TES_STATE_EMERGENCY:      return L("EMERGENCY",  ZH("緊急停止"));
    case TES_STATE_FINALIZE:       return L("FINALIZING", ZH("收尾中"));
    default:                       return "---";
    }
}

// 故障說明。中文一行 10 個字、英文 l1 約 21 字（6x10）、l2 約 25 字（5x8）。
// 完整說明與處置建議在網頁與 App。
//   l1 = 發生什麼事，l2 = 該怎麼辦；detail 由 fault_detail() 依 fault_ctx_a/b 組出實際數值（英數）
static void fault_src_text(uint8_t src, uint16_t ctx_a, const char **l1, const char **l2)
{
    switch ((fault_source_t)src) {
    case FAULT_SRC_VEHICLE_TIMEOUT:
        *l1 = L("No CAN permit",      ZH("車輛沒有回應"));
        *l2 = L("Check CAN wiring / plug", ZH("檢查CAN線與充電槍")); break;
    case FAULT_SRC_VOLTAGE_INCOMPAT:
        // 不叫人調高 Max Voltage：它是送給車輛的過壓門檻（VLIM2），電壓不夠要換電源
        *l1 = L("PSU volt too low",   ZH("電源電壓不夠高"));
        *l2 = L("Use a higher-V PSU", ZH("請換更高電壓的電源")); break;
    case FAULT_SRC_PRECHARGE_TIMEOUT:
        *l1 = L("Pre-charge timeout", ZH("預充電逾時"));
        *l2 = L("Re-plug the connector", ZH("請重新插好充電槍")); break;
    case FAULT_SRC_CONTACTOR_TIMEOUT:
        *l1 = L("Car contactor open", ZH("車輛接觸器沒閉合"));
        *l2 = L("Vehicle-side issue", ZH("車輛端的問題")); break;
    case FAULT_SRC_PSU_LOST:
        *l1 = L("PSU disconnected",   ZH("電源斷線"));
        *l2 = L("Check PSU power / link", ZH("檢查電源與連線")); break;
    case FAULT_SRC_BMS_FAULT:
        // bit0 = 供電系統異常：車輛指控的是充電樁，不是它自己的電池，
        // 兩者的處置完全不同，OLED 上就要分開講。
        if (ctx_a & V500_FAULT_SUPPLY_SYSTEM) {
            *l1 = L("Car blames charger", ZH("車輛回報供電異常"));
            *l2 = L("Supply fault reported", ZH("檢查充電樁與電源"));
        } else {
            *l1 = L("Vehicle battery",    ZH("車輛電池異常"));
            *l2 = L("fault - see dashboard", ZH("請看車輛儀表"));
        }
        break;
    case FAULT_SRC_CP_LOST:
        *l1 = L("Connector loose",    ZH("充電槍鬆脫"));
        *l2 = L("Re-seat and lock it", ZH("請重新插好並鎖緊")); break;
    case FAULT_SRC_EMERGENCY_BTN:
        *l1 = L("E-STOP pressed",     ZH("緊急停止已按下"));
        *l2 = L("Menu > Reset Fault", ZH("選單>復歸故障")); break;
    case FAULT_SRC_EMERGENCY_VEHICLE:
        *l1 = L("Vehicle E-stop",     ZH("車輛緊急停止"));
        *l2 = L("Normal after charge end", ZH("充電結束後屬正常")); break;
    case FAULT_SRC_OVER_CURRENT:
        // 只有接電流量測節點、配旋鈕電源時會出現：電源開得比最大電流（量測節點上限 15 A）大
        *l1 = L("Over current",       ZH("電流過大"));
        *l2 = L("Turn PSU current down", ZH("請把電源電流調低")); break;
    default:
        *l1 = L("Unknown fault",      ZH("未知故障"));
        *l2 = ""; break;
    }
}

// 把 fault_ctx_a/b 轉成該故障看得懂的一行數值
static void fault_detail(const tes_snapshot_t *s, char *buf, size_t bufsz)
{
    uint16_t a = s->fault_ctx_a, b = s->fault_ctx_b;
    switch ((fault_source_t)s->fault_source) {
    case FAULT_SRC_VOLTAGE_INCOMPAT:
        snprintf(buf, bufsz, "car %.1fV > max %.1fV", a / 10.0f, b / 10.0f); break;
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
    case FAULT_SRC_OVER_CURRENT:
        snprintf(buf, bufsz, "out %.1fA  car %.1fA", a / 10.0f, b / 10.0f); break;
    default:
        buf[0] = '\0'; break;
    }
}

// 故障詳情畫面：FAULT/EMERGENCY 期間，以及故障後仍停在 IDLE 的情況。
//
// 不能只看 fault_latched —— 狀態機從 FAULT 自動復歸時一定會清掉它
// （tes_sm.c 的 fault_timeout_ms，一般故障 10s、手動模式 CP 斷開只有 1s），
// 畫面因此會被 Standby 蓋掉，使用者根本來不及看到停止原因。
//
// 改看 fault_source：它在自動復歸時刻意保留，只有「使用者按 START」或
// 「auto_start 觸發新的一輪」才會清成 FAULT_SRC_NONE。
// 效果是故障說明會一直留在畫面上，直到真的開始下一次充電為止。
static bool fault_showing(const tes_snapshot_t *snap)
{
    return snap->state == TES_STATE_FAULT || snap->state == TES_STATE_EMERGENCY ||
           (snap->state == TES_STATE_IDLE &&
            (snap->fault_latched || snap->fault_source != FAULT_SRC_NONE));
}

static void render_fault(const tes_snapshot_t *snap)
{
    char buf[24], detail[26];
    const char *l1, *l2;
    fault_src_text(snap->fault_source, snap->fault_ctx_a, &l1, &l2);
    fault_detail(snap, detail, sizeof(detail));
    bool emerg = snap->state == TES_STATE_EMERGENCY;
    snprintf(buf, sizeof(buf), "0x%02X", snap->last_fault_flags);

    if (lang_zh()) {
        // 中文：標題、兩行說明各 12 px，數值與故障碼用英數小字
        header("", emerg ? ZH("緊急停止") : ZH("故障停止"));
        display_driver_font_small();
        display_driver_draw_str(104, 10, buf);
        display_driver_font_zh();
        display_driver_draw_utf8(0, 29, l1);
        display_driver_draw_utf8(0, 42, l2);
        display_driver_font_small();
        if (detail[0]) display_driver_draw_str(0, 51, detail);
        display_driver_font_zh();
        display_driver_draw_utf8(0, 63, ZH("START:重試 長按:選單"));
        return;
    }
    display_driver_font_bold();
    display_driver_draw_str(0, 12, emerg ? "EMERGENCY" : "FAULT STOP");
    display_driver_draw_hline(0, 15, 128);
    // 先講「發生什麼事」和「怎麼辦」，故障碼降級成最後一行的參考值
    display_driver_font_medium();
    display_driver_draw_str(0, 27, l1);
    display_driver_font_small();
    display_driver_draw_str(0, 38, l2);
    if (detail[0]) display_driver_draw_str(0, 48, detail);
    display_driver_draw_str(0, 60, buf);
    display_driver_draw_str(30, 60, "START:retry L:menu");
}

// ── 動畫（2026-10-17）────────────────────────────────────────────────────────
// 畫面任務 50 ms 跑一次、每次都重畫；動畫以 100 ms 為一格。全部只在高階款
//   * 充電中：標題列右邊的電池圖示（從目前 SOC 往上一格一格填）、電壓下面一排往前流的箭頭
//   * 充電完成：打勾一筆一筆畫出來，停在完成畫面直到按 SETTING 或開始下一次
//   * 待機：整個畫面每分鐘偏 1 px（防烙印，一律開）；超過設定時間沒人碰就進螢幕保護
//   * 故障畫面不動，要讓人專心讀
// 充電完成畫面的狀態（說明見下方「充電完成畫面」）
static uint8_t  s_done_soc;
static uint32_t s_done_el;
static float    s_done_wh;
static bool     s_done_dismissed;   // 按過鍵或顯示滿 DONE_SHOW_MS：回到一般待機畫面
static bool     s_done_showing;     // 目前畫面是完成畫面（SETTING 短按改成「返回」）
static bool     s_eat_set;          // 用 SETTING 離開完成畫面：放開時的短按事件不要再切 SOC
static uint32_t s_done_since;       // 進入完成畫面的時間（打勾動畫從這裡起算）

static uint32_t anim_frame(uint32_t now) { return now / 100u; }

static bool anim_on(uint8_t which)   // which = OLED_ANIM_BATTERY / OLED_ANIM_ARROWS
{
    uint8_t a = config_svc_get()->oled_anim;
    return a == OLED_ANIM_BOTH || a == which;
}

// 電池圖示：外框 24×10 在標題列右邊，5 格。從 SOC 對應的格數開始，每 0.4 秒多填一格，填滿再回來
static void draw_battery(int soc, uint32_t f)
{
    display_driver_draw_frame(101, 2, 24, 11);
    display_driver_draw_box(125, 5, 2, 5);
    int base = soc / 20; if (base > 4) base = 4;
    int n = base + (int)((f / 4) % (uint32_t)(6 - base));
    if (n > 5) n = 5;
    for (int i = 0; i < n; i++) display_driver_draw_box(103 + i * 4, 4, 3, 7);
}

// 一排 > 往右流：間距 8 px，每格前進 2 px
static void draw_arrows(int y, uint32_t f)
{
    for (int x = -8 + (int)((f * 2) % 8); x < 128; x += 8) {
        display_driver_draw_line(x, y, x + 3, y + 3);
        display_driver_draw_line(x + 3, y + 3, x, y + 6);
    }
}

static void render_status(const tes_snapshot_t *snap, uint32_t now)
{
    char buf[48];   // UTF-8：中文一個字 3 bytes
    const charger_config_t *cfg = config_svc_get();
    uint32_t f = anim_frame(now);
    bool charging = snap->state == TES_STATE_CHARGING;
    bool arrows   = charging && anim_on(OLED_ANIM_ARROWS);

    header(state_name(snap->state), state_name(snap->state));
    if (charging && anim_on(OLED_ANIM_BATTERY)) draw_battery(snap->soc, f);

    // 行 2：電壓／電流；開了箭頭動畫就在下面多一排箭頭，行 3 與底行往下移
    int y3   = arrows ? 50 : 42;
    int ybot = (arrows || lang_zh()) ? 62 : 56;

    display_driver_font_medium();
    if (cfg->stop_mode == STOP_MODE_VOLTAGE) {
        // Volt 模式：行 2 = 即時電壓/目標停止電壓，行 3 = SOC（純參考）+ 剩餘時間
        snprintf(buf, sizeof(buf), "%.1fV/%.1fV", snap->output_voltage, cfg->stop_voltage_01v / 10.0f);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d%%", snap->soc);
        display_driver_draw_str(0, y3, buf);
    } else if (cfg->stop_mode == STOP_MODE_ENERGY) {
        // kWh 模式：行 3 = 已充入/目標 kWh。SOC 放不下，網頁與 App 看得到。
        // 沒接 PSU 時電量是用最大電流設定值估的，前面加 ~（可能提早停）
        snprintf(buf, sizeof(buf), "%.1fV  %.1fA", snap->output_voltage, snap->output_current);
        display_driver_draw_str(0, 28, buf);
        float kwh = snap->energy_wh / 1000.0f;
        snprintf(buf, sizeof(buf), kwh < 10.0f ? "%s%.2f/%u.%ukWh" : "%s%.1f/%u.%ukWh",
                 snap->psu_connected ? "" : "~", kwh,
                 cfg->stop_energy_wh / 1000, (cfg->stop_energy_wh % 1000) / 100);
        display_driver_draw_str(0, y3, buf);
    } else if (cfg->stop_mode == STOP_MODE_TIMER) {
        // Timer 模式：行 3 = SOC + 已充/目標時間
        snprintf(buf, sizeof(buf), "%.1fV  %.1fA", snap->output_voltage, snap->output_current);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d%%", snap->soc);
        display_driver_draw_str(0, y3, buf);
        snprintf(buf, sizeof(buf), "%um/%um",
                 (unsigned)(snap->elapsed_seconds / 60), (unsigned)cfg->charge_timer_min);
        display_driver_draw_str(60, y3, buf);
    } else {
        // SOC 模式：行 3 = SOC 現在/目標 + 剩餘時間
        snprintf(buf, sizeof(buf), "%.1fV  %.1fA", snap->output_voltage, snap->output_current);
        display_driver_draw_str(0, 28, buf);
        snprintf(buf, sizeof(buf), "SOC:%d/%d%%", snap->soc, (int)snap->target_soc);
        display_driver_draw_str(0, y3, buf);
    }
    if (cfg->stop_mode != STOP_MODE_TIMER) {
        uint32_t rem_min = snap->timer_running ? (snap->remaining_seconds + 30) / 60 : 0;
        snprintf(buf, sizeof(buf), "%luh%02lum", rem_min / 60, rem_min % 60);
        display_driver_draw_str(cfg->stop_mode == STOP_MODE_ENERGY ? 86 : 80, y3, buf);
    }
    if (arrows) draw_arrows(31, f);

    // FAULT / EMERGENCY 已在前面走專屬畫面，這裡只會是正常運作狀態
    uint16_t over = config_svc_volt_over();
    if (over && snap->state == TES_STATE_IDLE) {
        // Auto Volt 讀到電源比這塊板的上限高：待機時用最下面一行提醒把電源調低
        uint16_t vmax = adc_driver_board()->max_volt_01v;
        if (lang_zh()) {
            snprintf(buf, sizeof(buf), "%s%u.%uV>%uV", ZH("!電源過高 "), over / 10, over % 10,
                     (unsigned)(vmax / 10));
            display_driver_font_zh();
            display_driver_draw_utf8(0, 63, buf);
        } else {
            snprintf(buf, sizeof(buf), "!PSU %u.%uV>%uV", over / 10, over % 10, (unsigned)(vmax / 10));
            display_driver_font_small();
            display_driver_draw_str(0, ybot, buf);
        }
        return;
    }
    display_driver_font_small();
    if (snap->vehicle_req_voltage > 0.5f || snap->vehicle_req_current > 0.1f)
        snprintf(buf, sizeof(buf), "REQ:%.0fV %.0fA", snap->vehicle_req_voltage, snap->vehicle_req_current);
    else
        snprintf(buf, sizeof(buf), "REQ: --V --A");
    display_driver_draw_str(0, ybot, buf);
    if (lang_zh()) {
        display_driver_font_zh();
        display_driver_draw_utf8(128 - display_driver_utf8_width(ZH("長按:設定")), 63, ZH("長按:設定"));
    } else {
        display_driver_draw_str(68, ybot, "S:SOC L:CFG");
    }
}

// ── 充電完成畫面 ───────────────────────────────────────────────────────────────
// 正常結束（LED 綠燈）後回到 IDLE 時顯示 DONE_SHOW_MS，之後自動回到待機畫面；這段時間內按任何一顆鍵
// 也會馬上回去（使用者 2026-10-17：「還要按設定鍵返回用起來不順」）。回到的是一般待機畫面，
// 照樣看得到充電設定（SOC:0/95% 這一行的目標值）——試過把那行換成上一次的摘要，使用者要看設定值，改回來了
// 這一輪的 SOC、時長、電量在 CHARGING 期間記下來 —— 回到 IDLE 時快照裡的已經歸零
#define DONE_SHOW_MS 60000u

static bool done_active(const tes_snapshot_t *snap)
{
    return snap->state == TES_STATE_IDLE && snap->led_state == LED_STATE_COMPLETE &&
           !fault_showing(snap) && !s_done_dismissed;
}

static void render_done(uint32_t now)
{
    char buf[24];
    // 打勾：圓圈內兩筆，1.2 秒畫完。粗 3 px（畫三次錯開 1 px）
    const int cx = 21, cy = 32;
    display_driver_draw_circle(cx, cy, 19);
    float t = (now - s_done_since) / 1200.0f;
    if (t > 1.0f) t = 1.0f;
    const float ax = 12, ay = 32, bx = 19, by = 40, ex = 31, ey = 23;   // 兩筆的端點
    const float l1 = 10.6f, l2 = 21.0f, d = t * (l1 + l2);
    for (int k = -1; k <= 1; k++) {
        if (d <= l1) {
            float r = d / l1;
            display_driver_draw_line(ax, ay + k, ax + (bx - ax) * r, ay + (by - ay) * r + k);
        } else {
            float r = (d - l1) / l2;
            display_driver_draw_line(ax, ay + k, bx, by + k);
            display_driver_draw_line(bx, by + k, bx + (ex - bx) * r, by + (ey - by) * r + k);
        }
    }
    if (now - s_done_since < 600) return;   // 先打勾，文字晚一點出來

    text(46, 14, EN_BOLD, "COMPLETE", ZH("充電完成"));
    display_driver_font_medium();
    snprintf(buf, sizeof(buf), "SOC %u%%", (unsigned)s_done_soc);
    display_driver_draw_str(46, 30, buf);
    display_driver_font_small();
    uint32_t m = (s_done_el + 30) / 60;
    snprintf(buf, sizeof(buf), "%luh%02lum %.2fkWh", (unsigned long)(m / 60), (unsigned long)(m % 60),
             s_done_wh / 1000.0f);
    display_driver_draw_str(46, 42, buf);
    if (lang_zh()) {
        display_driver_font_zh();
        display_driver_draw_utf8(46, 63, ZH("按任意鍵返回"));
    } else {
        display_driver_draw_str(46, 60, "any key: back");
    }
}

// ── 螢幕保護 ───────────────────────────────────────────────────────────────────
// 待機（含完成畫面、待機中的故障說明）超過 ss_min 分鐘沒有任何動靜就進入。資訊框每分鐘換一個
// 角落：狀態、時間（有對時才有）、電源電壓、「按鍵喚醒」。樣式「烏龜」另外有一隻小烏龜游過去
static void draw_small_turtle(int x, int y, bool legs)
{
    // 頭朝右的側面烏龜：殼、頭、四隻鰭、尾巴
    display_driver_draw_circle(x, y, 5);
    display_driver_draw_circle(x, y, 3);
    display_driver_draw_disc(x + 8, y, 2);
    int k = legs ? 1 : -1;
    display_driver_draw_triangle(x + 2, y - 4, x + 5 + k, y - 8, x + 6, y - 4);
    display_driver_draw_triangle(x - 2, y - 4, x - 5 - k, y - 8, x - 6, y - 4);
    display_driver_draw_triangle(x + 2, y + 4, x + 5 - k, y + 8, x + 6, y + 4);
    display_driver_draw_triangle(x - 2, y + 4, x - 5 + k, y + 8, x - 6, y + 4);
    display_driver_draw_triangle(x - 5, y - 1, x - 9, y, x - 5, y + 1);
}

static void render_saver(const tes_snapshot_t *snap, uint32_t now, bool done)
{
    const charger_config_t *cfg = config_svc_get();
    uint32_t f = anim_frame(now);

    if (cfg->ss_style == OLED_SS_TURTLE) {
        const uint32_t period = 180;   // 18 秒游過一次
        int x = -12 + (int)((f % period) * 152 / period);
        int y = 34 + (int)(6.0f * sinf(f / 6.0f));
        draw_small_turtle(x, y, (f / 3) % 2);
        for (int i = 0; i < 3; i++) {   // 泡泡
            uint32_t bt = (f + (uint32_t)i * 13u) % 40u;
            if (bt < 24) display_driver_draw_circle(x + 10 + i * 3, y - 6 - (int)(bt * 4 / 5), 1 + (i % 2));
        }
    }

    // 資訊框兩行：「狀態 時間」、「電源 84.0V 按鍵喚醒」
    char l1[32], l2[40], clock[8] = "";
    time_t t = time(NULL);
    if (t > 1600000000) {
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    const char *st = fault_showing(snap) ? L("FAULT", ZH("故障停止"))
                   : done                ? L("DONE", ZH("充電完成"))
                                         : L("STANDBY", ZH("待機中"));
    snprintf(l1, sizeof l1, "%s %s", st, clock);
    snprintf(l2, sizeof l2, "%s%u.%uV %s", L("PSU ", ZH("電源")),
             cfg->max_voltage_01v / 10, cfg->max_voltage_01v % 10, L("any key", ZH("按鍵喚醒")));

    // 用中文字型量寬度（英文模式也用它畫，兩行一致；字型含半形英數）
    display_driver_font_zh();
    int w1 = display_driver_utf8_width(l1), w2 = display_driver_utf8_width(l2);
    int w = w1 > w2 ? w1 : w2;
    uint32_t corner = (now / 60000u) % 4u;   // 每分鐘換一個角落
    int x  = (corner & 1u) ? 128 - w : 0;
    int y1 = (corner & 2u) ? 51 : 11;
    display_driver_draw_utf8(x, y1, l1);
    display_driver_draw_utf8(x, y1 + 12, l2);
}

// ── ESP-NOW 配對畫面 ───────────────────────────────────────────────────────────
//
// 跟藍牙一樣：兩邊螢幕顯示同一組 6 位數，使用者比對後在兩邊都按確認。
// 配對中這個畫面蓋過一切（包括設定選單）—— 因為 task_hal_poll 此時把
// START/STOP 當成確認/取消，畫面必須說清楚按鈕現在的意思。
// 結束後留幾秒顯示結果。狀態值見 psu_link/psu_pair.h。
#define PAIR_DONE_SHOW_MS  3000u
#define PAIR_FAIL_SHOW_MS  5000u

static bool render_pair(void)
{
    psu_pair_info_t p = psu_driver_pair_info();
    bool busy = p.state == PSU_PAIR_SEARCHING || p.state == PSU_PAIR_EXCHANGING ||
                p.state == PSU_PAIR_CONFIRM;
    bool show_done = p.state == PSU_PAIR_DONE   && p.age_ms < PAIR_DONE_SHOW_MS;
    bool show_fail = p.state == PSU_PAIR_FAILED && p.age_ms < PAIR_FAIL_SHOW_MS;
    if (!busy && !show_done && !show_fail && !psu_driver_is_pairing()) return false;

    char buf[28];
    display_driver_clear();
    display_driver_set_color(1);
    if (p.state == PSU_PAIR_CONFIRM) {
        header("PSU PAIRING", ZH("電源配對"));
        // 大號配對碼，分兩半畫（字型只有數字，不依賴空白字元）
        char a[8], b[8];
        snprintf(a, sizeof a, "%03lu", (unsigned long)((p.code / 1000u) % 1000u));
        snprintf(b, sizeof b, "%03lu", (unsigned long)(p.code % 1000u));
        display_driver_font_digits();
        int wa = display_driver_str_width(a), wb = display_driver_str_width(b);
        int x = (128 - (wa + 10 + wb)) / 2;
        display_driver_draw_str(x, 40, a);
        display_driver_draw_str(x + wa + 10, 40, b);
        if (p.local_ok) {
            text(0, lang_zh() ? 52 : 52, EN_SMALL, "Waiting for the PSU...", ZH("等待電源確認..."));
            text(0, lang_zh() ? 63 : 62, EN_SMALL, "STOP = cancel", ZH("STOP=取消"));
        } else {
            text(0, 52, EN_SMALL, "Same code on the PSU?", ZH("電源上的數字一樣嗎?"));
            text(0, lang_zh() ? 63 : 62, EN_SMALL, "START = yes   STOP = no", ZH("START=是  STOP=否"));
        }
    } else if (busy || psu_driver_is_pairing()) {
        header("PSU PAIRING", ZH("電源配對"));
        if (p.state == PSU_PAIR_EXCHANGING) text(0, 29, EN_MEDIUM, "PSU found...", ZH("找到電源..."));
        else                                text(0, 29, EN_MEDIUM, "Searching...", ZH("搜尋中..."));
        if (p.legacy_seen) {
            // 收到舊版 LianMing 的純文字配對：它不會做新流程，一定配不起來
            text(0, lang_zh() ? 42 : 41, EN_SMALL, "PSU firmware too old!", ZH("電源韌體太舊"));
            text(0, lang_zh() ? 54 : 51, EN_SMALL, "Update the PSU first", ZH("請先更新電源"));
        } else {
            text(0, lang_zh() ? 42 : 41, EN_SMALL, "Start pairing on PSU", ZH("請在電源上開始配對"));
        }
        if (!p.legacy_seen || !lang_zh())
            text(0, lang_zh() ? 63 : 62, EN_SMALL, "STOP = cancel", ZH("STOP=取消"));
    } else if (show_done) {
        header("PAIRED", ZH("配對成功"));
        text(0, 30, EN_MEDIUM, "PSU linked securely", ZH("已安全連線"));
        display_driver_font_small();
        snprintf(buf, sizeof buf, "PSU %02X:%02X:%02X:%02X:%02X:%02X",
                 p.peer_mac[0], p.peer_mac[1], p.peer_mac[2],
                 p.peer_mac[3], p.peer_mac[4], p.peer_mac[5]);
        display_driver_draw_str(0, 44, buf);
    } else {
        const char *why;
        switch (p.fail) {
        case PSU_PAIR_FAIL_TIMEOUT:     why = L("Timed out",           ZH("逾時"));         break;
        case PSU_PAIR_FAIL_USER_REJECT: why = L("Cancelled",           ZH("已取消"));       break;
        case PSU_PAIR_FAIL_PEER_REJECT: why = L("Cancelled on PSU",    ZH("電源端取消"));   break;
        case PSU_PAIR_FAIL_COMMIT:
        case PSU_PAIR_FAIL_CONFIRM:     why = L("Security check fail", ZH("安全檢查失敗")); break;
        default:                        why = L("Error",               ZH("錯誤"));         break;
        }
        header("PAIRING FAILED", ZH("配對失敗"));
        text(0, 30, EN_MEDIUM, why, why);
        text(0, 44, EN_SMALL, "Nothing was saved", ZH("沒有儲存任何設定"));
        if (p.fail == PSU_PAIR_FAIL_COMMIT || p.fail == PSU_PAIR_FAIL_CONFIRM)
            text(0, lang_zh() ? 57 : 54, EN_SMALL, "Retry away from others", ZH("遠離其他裝置再試"));
    }
    display_driver_flush();
    return true;
}

// ── 工作台測試模式畫面 ─────────────────────────────────────────────────────────
//
// 測試中，繼電器與 LED 由網頁 /hw 手動控制 —— 站在機器前的人必須看得出來，
// 否則會以為「待機中怎麼繼電器在響」。所以測試中整個狀態畫面換掉；結束後
// 再留 BENCH_END_SHOW_MS 顯示原因，讓人知道控制權回來了、為什麼回來。
// 選單不受影響：測試中照樣能長按 SETTING 打開。
#define BENCH_END_SHOW_MS 3000u

static const char *bench_end_text(hwtest_block_t b)
{
    switch (b) {
    case HWTEST_BLOCK_NOT_IDLE:    return "Charger left standby";
    case HWTEST_BLOCK_EMERGENCY:   return "Emergency stop";
    case HWTEST_BLOCK_CP_PRESENT:  return "Vehicle found (CP)";
    case HWTEST_BLOCK_VEHICLE_CAN: return "Vehicle found (CAN)";
    case HWTEST_END_LEASE:         return "Web page closed/lost";
    case HWTEST_END_USER:          return "Stopped from web page";
    default:                       return "";
    }
}

// 有畫東西就回傳 true（呼叫端就不畫一般狀態畫面）
static bool render_bench(void)
{
    hwtest_status_t t = hwtest_svc_status(platform_tick_ms());
    bool ended = !t.active && t.ended_ago_ms < BENCH_END_SHOW_MS;
    if (!t.active && !ended) return false;

    // 字寬：small 5 px（一行 25 字）、medium 6 px（21 字）、bold 7 px
    char buf[28];
    display_driver_clear();
    display_driver_set_color(1);
    display_driver_font_bold();

    if (t.active) {
        // 反白標題：一眼就看得出不是平常的畫面
        display_driver_draw_box(0, 0, 128, 15);
        display_driver_set_color(0);
        display_driver_draw_str(4, 12, "BENCH TEST");
        display_driver_set_color(1);

        // 顯示腳位實際電位，不是網頁要求的值
        uint8_t o = hal_gpio_outputs_get();
        display_driver_font_small();
        display_driver_draw_str(0, 25, "Outputs set from web /hw");
        snprintf(buf, sizeof(buf), "RELAY:%s LOCK:%s VP:%s",
                 (o & HAL_OUT_RELAY) ? "ON" : "--",
                 (o & HAL_OUT_LOCK)  ? "ON" : "--",
                 (o & HAL_OUT_VP)    ? "ON" : "--");
        display_driver_draw_str(0, 37, buf);
        snprintf(buf, sizeof(buf), "LED  Y:%s  G:%s  R:%s",
                 (o & HAL_OUT_LED_STANDBY)  ? "ON" : "--",
                 (o & HAL_OUT_LED_CHARGING) ? "ON" : "--",
                 (o & HAL_OUT_LED_ERROR)    ? "ON" : "--");
        display_driver_draw_str(0, 47, buf);
        display_driver_draw_hline(0, 51, 128);
        display_driver_draw_str(0, 61, "START/plug/E-STOP = exit");
    } else {
        display_driver_draw_str(0, 12, "TEST ENDED");
        display_driver_draw_hline(0, 15, 128);
        display_driver_font_medium();
        display_driver_draw_str(0, 30, bench_end_text(t.last_end));
        display_driver_font_small();
        display_driver_draw_str(0, 46, "Outputs back to normal");
    }
    display_driver_flush();
    return true;
}

// ── Menu screen render ────────────────────────────────────────────────────────

static void render_menu(void)
{
    char buf[40];
    display_driver_clear();
    display_driver_set_color(1);

    // Header
    if (s_mode == MENU_MODE_EDIT) header("< EDIT VALUE >", ZH("< 調整數值 >"));
    else                          header("SETTINGS", ZH("設定"));

    // Items: 4 visible rows, each 12px tall。中文 12 px 剛好一列，基線在列頂 +10
    bool zh = lang_zh();
    for (int row = 0; row < MAX_VISIBLE_ROWS; row++) {
        int vis = s_scroll_top + row;
        if (vis >= s_visible_count) break;
        int item = s_visible_items[vis];

        int box_y  = HEADER_H + row * ROW_H;
        int text_y = box_y + (zh ? 10 : ROW_TEXT_OFS);
        bool selected = (vis == s_cursor);

        item_label(item, buf, sizeof(buf));

        if (zh) display_driver_font_zh(); else display_driver_font_small();
        if (selected) {
            // Highlighted row: white box, black text
            display_driver_set_color(1);
            display_driver_draw_box(0, box_y, 128, ROW_H);
            display_driver_set_color(0);
            display_driver_draw_utf8(2, text_y, buf);
            display_driver_set_color(1);
        } else {
            display_driver_draw_utf8(2, text_y, buf);
        }
    }

    // Scroll arrows (right side) when there are hidden items
    display_driver_font_small();
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
            if (s_done_showing || s_eat_set) {   // 完成畫面上 = 返回待機畫面（不改 SOC）
                s_done_dismissed = true;
                s_eat_set = false;
                break;
            }
            cycle_quick_soc();
            break;
        case EVT_BUTTON_SETTING_LONG: // long press → open settings menu
            s_eat_set = false;
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
            s_restart_armed = false;
            if (s_cursor > 0) {
                s_cursor--;
                if (s_cursor < s_scroll_top)
                    s_scroll_top = s_cursor;
            }
            break;

        case EVT_BUTTON_STOP:    // scroll DOWN
            s_restart_armed = false;
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
                } else if (actual == MENU_ITEM_RESTART) {
                    uint32_t now = platform_tick_ms();
                    if (s_restart_armed && now - s_restart_armed_ms < RESTART_CONFIRM_MS) {
                        // 未儲存的設定變更會被丟掉 —— 和 Cancel 一樣
                        if (restart_svc_request(500, "oled")) menu_close();
                        s_restart_armed = false;
                    } else if (restart_svc_allowed()) {
                        s_restart_armed    = true;
                        s_restart_armed_ms = now;
                    }
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

    if (restart_svc_pending()) {   // 網頁或選單要求重新啟動：剩不到一秒，說清楚螢幕為什麼要黑掉
        display_driver_clear();
        display_driver_set_color(1);
        text(0, 36, EN_BOLD, "Restarting...", ZH("重新啟動中..."));
        display_driver_flush();
        return;
    }
    if (s_restart_armed && platform_tick_ms() - s_restart_armed_ms >= RESTART_CONFIRM_MS) {
        s_restart_armed = false;   // 逾時沒按第二次，標籤變回 Restart
    }

    uint32_t now = platform_tick_ms();

    // 這一輪的摘要：CHARGING 期間一直記，回到 IDLE 時快照裡的時長、電量已經歸零
    if (snap.state == TES_STATE_CHARGING) {
        s_done_soc = snap.soc;
        s_done_el  = snap.elapsed_seconds;
        if (snap.energy_wh > 0.0f) s_done_wh = snap.energy_wh;
        s_done_dismissed = false;
    }
    bool done = done_active(&snap);
    if (done && !s_done_showing) s_done_since = now;   // 剛進入完成畫面：打勾動畫從頭播
    if (done && s_done_showing) {
        // 顯示滿 1 分鐘，或按了任何一顆鍵（START／STOP／SETTING 剛按下），就回到待機畫面。
        // START 照常會送給狀態機（插著槍就是再充一次），這裡只負責畫面
        static uint8_t s_done_btn;
        bool pressed = (g_btn_stable & (uint8_t)~s_done_btn & 0x0Bu) != 0;
        if (pressed || now - s_done_since >= DONE_SHOW_MS) { s_done_dismissed = true; done = false; }
        if (pressed && (g_btn_stable & 0x08u)) s_eat_set = true;
        s_done_btn = g_btn_stable;
    }
    s_done_showing = done && s_screen == DISP_SCREEN_STATUS;

    // ── 螢幕保護：任何動靜都重新計時 ──
    static uint32_t    s_last_activity;
    static uint8_t     s_prev_btn;
    static tes_state_t s_prev_state;
    static uint8_t     s_prev_cp;
    bool activity = g_btn_stable != s_prev_btn || snap.state != s_prev_state ||
                    snap.cp_state != s_prev_cp || s_screen != DISP_SCREEN_STATUS ||
                    psu_driver_is_pairing() || hwtest_svc_status(now).active || g_screen_wake;
    s_prev_btn   = g_btn_stable;
    s_prev_state = snap.state;
    s_prev_cp    = snap.cp_state;
    g_screen_wake = false;
    if (activity || s_last_activity == 0) s_last_activity = now;
    const charger_config_t *cfg2 = config_svc_get();
    bool saver = cfg2->ss_min && snap.state == TES_STATE_IDLE && s_screen == DISP_SCREEN_STATUS &&
                 now - s_last_activity >= (uint32_t)cfg2->ss_min * 60000u;
    g_screen_saver = saver;

    if (render_pair()) return;   // 配對中蓋過選單：按鈕此時是確認／取消

    if (s_screen == DISP_SCREEN_MENU) {
        render_menu();
        return;
    }
    if (render_bench()) return;

    display_driver_clear();
    display_driver_set_color(1);
    if (saver) {
        render_saver(&snap, now, done);
    } else {
        // 待機時整個畫面每分鐘偏 1 px，四個位置輪流（防烙印，一律開）。充電中畫面一直在變，不用
        if (snap.state == TES_STATE_IDLE) {
            static const int8_t ox[] = { 0, 1, 1, 0 }, oy[] = { 0, 0, 1, 1 };
            uint32_t k = (now / 60000u) % 4u;
            display_driver_set_offset(ox[k], oy[k]);
        }
        if (fault_showing(&snap)) render_fault(&snap);
        else if (done)            render_done(now);
        else                      render_status(&snap, now);
        display_driver_set_offset(0, 0);
    }
    display_driver_flush();

}
