// display_svc.c — OLED status screen + settings menu
// Runs in task_display at 50ms intervals.
// Button events arrive via display_svc_button() called from task_display after
// draining g_display_btn_queue.

#include "services/display_svc.h"
#include "services/config_svc.h"
#include "drivers/display_driver.h"
#include "drivers/led_driver.h"
#include "tes_protocol/tes_sm.h"
#include "services/event_bus.h"   // for EVT_BUTTON_* enum values
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

// ── Screen state ──────────────────────────────────────────────────────────────

static disp_screen_t s_screen = DISP_SCREEN_STATUS;

// ── Menu state ────────────────────────────────────────────────────────────────

typedef enum {
    MENU_ITEM_MAX_VOLTAGE = 0,
    MENU_ITEM_MAX_CURRENT,
    MENU_ITEM_TARGET_SOC,
    MENU_ITEM_BEACON,
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
static uint16_t s_edit_voltage;
static uint16_t s_edit_current;
static int8_t   s_edit_soc;
static bool     s_edit_beacon;

#define MAX_VISIBLE_ROWS  4
#define HEADER_H         16    // pixels reserved for header + separator
#define ROW_H            12    // pixels per menu row
#define ROW_TEXT_OFS      6    // text baseline offset from row top

// ── Menu helpers ──────────────────────────────────────────────────────────────

static void menu_open(void)
{
    const charger_config_t *cfg = config_svc_get();
    s_edit_voltage = cfg->max_voltage_01v;
    s_edit_current = cfg->max_current_01a;
    s_edit_soc     = cfg->target_soc;
    s_edit_beacon  = cfg->beacon_unlocked;
    s_cursor       = 0;
    s_scroll_top   = 0;
    s_mode         = MENU_MODE_NAV;
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
    config_svc_set_charging(s_edit_voltage, s_edit_current, s_edit_soc);
    config_svc_set_beacon(s_edit_beacon);
    ESP_LOGI(TAG, "saved: %u.%uV %u.%uA SOC=%d beacon=%d",
             s_edit_voltage / 10, s_edit_voltage % 10,
             s_edit_current / 10, s_edit_current % 10,
             s_edit_soc, (int)s_edit_beacon);
}

static void item_label(int item, char *buf, size_t bufsz)
{
    switch ((menu_item_id_t)item) {
    case MENU_ITEM_MAX_VOLTAGE:
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
    case MENU_ITEM_BEACON:
        snprintf(buf, bufsz, "LuxBeacon: %s",
                 s_edit_beacon ? "ON" : "OFF");
        break;
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
    return (item == MENU_ITEM_MAX_VOLTAGE ||
            item == MENU_ITEM_MAX_CURRENT ||
            item == MENU_ITEM_TARGET_SOC  ||
            item == MENU_ITEM_BEACON);
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
    case MENU_ITEM_BEACON:
        s_edit_beacon = !s_edit_beacon;
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

static void render_status(const tes_snapshot_t *snap)
{
    char buf[24];
    display_driver_clear();
    display_driver_set_color(1);

    display_driver_font_bold();
    display_driver_draw_str(0, 12, state_name(snap->state));
    display_driver_draw_hline(0, 15, 128);

    display_driver_font_medium();
    snprintf(buf, sizeof(buf), "%.1fV  %.1fA",
             snap->output_voltage, snap->output_current);
    display_driver_draw_str(0, 28, buf);

    snprintf(buf, sizeof(buf), "SOC:%d/%d%%", snap->soc, (int)snap->target_soc);
    display_driver_draw_str(0, 42, buf);

    uint32_t rem_min = snap->timer_running
        ? (snap->remaining_seconds + 30) / 60 : 0;
    snprintf(buf, sizeof(buf), "%luh%02lum", rem_min / 60, rem_min % 60);
    display_driver_draw_str(80, 42, buf);

    display_driver_font_small();
    if (snap->state == TES_STATE_FAULT || snap->state == TES_STATE_EMERGENCY) {
        snprintf(buf, sizeof(buf), "ERR:0x%02X", snap->last_fault_flags);
        display_driver_draw_str(0, 56, buf);
    } else {
        if (snap->vehicle_req_voltage > 0.5f || snap->vehicle_req_current > 0.1f) {
            snprintf(buf, sizeof(buf), "REQ:%.0fV %.0fA",
                     snap->vehicle_req_voltage, snap->vehicle_req_current);
        } else {
            snprintf(buf, sizeof(buf), "REQ: --V --A");
        }
        display_driver_draw_str(0, 56, buf);
        snprintf(buf, sizeof(buf), "S:SOC L:CFG");
        display_driver_draw_str(68, 56, buf);
    }

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
        int item = s_scroll_top + row;
        if (item >= MENU_ITEM_COUNT) break;

        int box_y  = HEADER_H + row * ROW_H;
        int text_y = box_y + ROW_TEXT_OFS;
        bool selected = (item == s_cursor);

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
    if (s_scroll_top + MAX_VISIBLE_ROWS < MENU_ITEM_COUNT)
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
            if (s_cursor < MENU_ITEM_COUNT - 1) {
                s_cursor++;
                if (s_cursor >= s_scroll_top + MAX_VISIBLE_ROWS)
                    s_scroll_top = s_cursor - MAX_VISIBLE_ROWS + 1;
            }
            break;

        case EVT_BUTTON_SETTING: // CONFIRM / execute
            if (s_cursor == MENU_ITEM_SAVE) {
                menu_save();
                menu_close();
            } else if (s_cursor == MENU_ITEM_CANCEL) {
                menu_close();
            } else if (item_is_editable(s_cursor)) {
                s_mode = MENU_MODE_EDIT;
            }
            break;

        default:
            break;
        }
    } else { // MENU_MODE_EDIT
        switch (evt) {
        case EVT_BUTTON_START:      value_step(s_cursor, +1);  break; // fine   +0.1
        case EVT_BUTTON_STOP:       value_step(s_cursor, -1);  break; // fine   -0.1
        case EVT_BUTTON_START_LONG: value_step(s_cursor, +10); break; // coarse +1
        case EVT_BUTTON_STOP_LONG:  value_step(s_cursor, -10); break; // coarse -1
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

    // Update LED state
    led_state_t led;
    switch (snap.state) {
    case TES_STATE_CHARGING:
        led = LED_STATE_CHARGING;
        break;
    case TES_STATE_FAULT:
    case TES_STATE_EMERGENCY:
        led = LED_STATE_FAULT;
        break;
    default:
        led = snap.charge_complete ? LED_STATE_COMPLETE : LED_STATE_STANDBY;
        break;
    }
    led_driver_set_state(led);

    const charger_config_t *cfg = config_svc_get();
    led_driver_set_beacon_enable(cfg->beacon_unlocked);
    led_driver_set_beacon_soc(snap.soc);
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
