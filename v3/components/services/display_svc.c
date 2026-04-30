#include "services/display_svc.h"
#include "services/config_svc.h"
#include "drivers/display_driver.h"
#include "drivers/led_driver.h"
#include "tes_protocol/tes_sm.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display_svc";
extern tes_snapshot_t g_snapshot;
extern SemaphoreHandle_t g_snapshot_mutex;

static disp_screen_t s_screen = DISP_SCREEN_STATUS;

void display_svc_init(void)
{
    s_screen = DISP_SCREEN_STATUS;
    ESP_LOGI(TAG, "init");
}

static void render_status(const tes_snapshot_t *snap)
{
    char buf[32];
    display_driver_clear();

    // Line 1: state name
    display_driver_font_bold();
    const char *state_str;
    switch (snap->state) {
    case TES_STATE_IDLE:           state_str = "STANDBY";      break;
    case TES_STATE_PARAM_EXCHANGE: state_str = "CONNECTING";   break;
    case TES_STATE_PRE_CHARGE:     state_str = "PRE-CHARGE";   break;
    case TES_STATE_CHARGING:       state_str = "CHARGING";     break;
    case TES_STATE_ENDING:         state_str = "ENDING";       break;
    case TES_STATE_FAULT:          state_str = "FAULT";        break;
    case TES_STATE_EMERGENCY:      state_str = "EMERGENCY";    break;
    case TES_STATE_FINALIZE:       state_str = "FINALIZING";   break;
    default:                       state_str = "---";          break;
    }
    display_driver_draw_str(0, 12, state_str);
    display_driver_draw_hline(0, 14, 128);

    // Line 2: voltage / current
    display_driver_font_medium();
    snprintf(buf, sizeof(buf), "%.1fV  %.1fA",
             snap->output_voltage, snap->output_current);
    display_driver_draw_str(0, 28, buf);

    // Line 3: SOC + time
    snprintf(buf, sizeof(buf), "SOC:%3d%%", snap->soc);
    display_driver_draw_str(0, 42, buf);

    uint32_t elapsed = snap->elapsed_seconds;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", elapsed / 60, elapsed % 60);
    display_driver_draw_str(80, 42, buf);

    // Line 4: fault or target
    if (snap->state == TES_STATE_FAULT) {
        snprintf(buf, sizeof(buf), "ERR:0x%02X", snap->last_fault_flags);
        display_driver_draw_str(0, 56, buf);
    } else {
        const charger_config_t *cfg = config_svc_get();
        snprintf(buf, sizeof(buf), "TGT:%dV %dA",
                 cfg->max_voltage_01v / 10, cfg->max_current_01a / 10);
        display_driver_draw_str(0, 56, buf);
    }

    display_driver_flush();
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

    // Update LED state to match TES state
    led_state_t led;
    switch (snap.state) {
    case TES_STATE_CHARGING:       led = LED_STATE_CHARGING; break;
    case TES_STATE_FAULT:
    case TES_STATE_EMERGENCY:      led = LED_STATE_FAULT;    break;
    default:
        led = snap.charge_complete ? LED_STATE_COMPLETE : LED_STATE_STANDBY;
        break;
    }
    led_driver_set_state(led);
    led_driver_set_beacon_soc(snap.soc);
    led_driver_tick();

    if (!display_driver_is_ok()) return;

    switch (s_screen) {
    case DISP_SCREEN_STATUS:
        render_status(&snap);
        break;
    case DISP_SCREEN_MENU:
    case DISP_SCREEN_FAULT:
        // TODO: implement menu/fault screens
        render_status(&snap);
        break;
    }
}

void display_svc_nav_next(void)   { /* TODO: menu navigation */ }
void display_svc_nav_select(void) { /* TODO: menu confirm    */ }
void display_svc_nav_back(void)   { /* TODO: menu back       */ }
