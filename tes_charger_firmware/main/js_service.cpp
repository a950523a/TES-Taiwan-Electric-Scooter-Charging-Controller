#include "js_service.h"
#include "board_bsp.h"
#include "tes_adc.h"
#include "tes_can.h"
#include "tes_i2c.h"
#include "tes_oled.h"
#include "tes_protocol.h"
#include "network_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>

extern "C" {
    #include "quickjs.h"
}

static const char *TAG = "JS_SVC";
static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;
static int64_t last_js_heartbeat = 0;
static const int64_t JS_WATCHDOG_TIMEOUT_US = 3000000; 

// Helper: 用來強制轉型函數指標，解決 C++ 編譯錯誤
#define JS_CFUNC(func, name, len) JS_NewCFunction(ctx, (JSCFunction*)(func), name, len)

static char* load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "File not found: %s", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (buf) { fread(buf, 1, len, f); buf[len] = 0; }
    fclose(f); return buf;
}

// --- Standard Functions ---

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) { ESP_LOGI("JS_CONSOLE", "%s", str); JS_FreeCString(ctx, str); }
    }
    return JS_UNDEFINED;
}

static JSValue js_tes_feed_watchdog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    last_js_heartbeat = esp_timer_get_time();
    return JS_UNDEFINED;
}

// --- Getters (統一格式: argc=0) ---

static JSValue js_vehicle_get_soc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewInt32(ctx, tes_vehicle.soc);
}
static JSValue js_vehicle_get_volt_req(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_vehicle.charge_voltage_limit_100mV / 10.0);
}
static JSValue js_vehicle_get_curr_req(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_vehicle.charge_current_request_100mA / 10.0);
}
static JSValue js_vehicle_get_charging_enabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, tes_vehicle.status_flags.bits.charging_enabled);
}
static JSValue js_vehicle_get_fault(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewInt32(ctx, tes_vehicle.fault_flags.raw);
}

// --- Getters/Setters for Charger (統一格式: argc=0 for get, argc=1 for set) ---

static JSValue js_charger_get_volt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_charger.output_voltage_100mV / 10.0);
}
static JSValue js_charger_set_volt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    double v; if (JS_ToFloat64(ctx, &v, argv[0])) return JS_EXCEPTION;
    tes_charger.output_voltage_100mV = (uint16_t)(v * 10);
    return JS_UNDEFINED;
}

static JSValue js_charger_get_curr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_charger.output_current_100mA / 10.0);
}
static JSValue js_charger_set_curr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    double c; if (JS_ToFloat64(ctx, &c, argv[0])) return JS_EXCEPTION;
    tes_charger.output_current_100mA = (uint16_t)(c * 10);
    return JS_UNDEFINED;
}

static JSValue js_charger_get_relay(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, !tes_charger.status_flags.bits.stop_control);
}
static JSValue js_charger_set_relay(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int b = JS_ToBool(ctx, argv[0]); 
    if (b < 0) return JS_EXCEPTION;
    tes_charger.status_flags.bits.stop_control = b ? 0 : 1;
    tes_charger.status_flags.bits.operating_status = b ? 1 : 0;
    return JS_UNDEFINED;
}

// --- Hardware Control ---

static JSValue js_tes_set_relay(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id, state;
    if (JS_ToInt32(ctx, &id, argv[0]) || JS_ToInt32(ctx, &state, argv[1])) return JS_EXCEPTION;
    
    const tes_io_t *io = NULL;
    switch(id) {
        case 0: io = &PIN_RELAY_MAIN; break;
        case 1: io = &PIN_RELAY_VP; break;
        case 2: io = &PIN_RELAY_LOCK; break;
    }
    if (io) tes_io_set_level(io, state);
    return JS_UNDEFINED;
}

static JSValue js_tes_set_led(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id, state;
    if (JS_ToInt32(ctx, &id, argv[0]) || JS_ToInt32(ctx, &state, argv[1])) return JS_EXCEPTION;
    
    const tes_io_t *io = NULL;
    switch(id) {
        case 0: io = &PIN_LED_STANDBY; break;
        case 1: io = &PIN_LED_CHARGING; break;
        case 2: io = &PIN_LED_ERROR; break;
    }
    if (io) tes_io_set_level(io, state);
    return JS_UNDEFINED;
}

static JSValue js_tes_get_button(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    
    const tes_io_t *io = NULL;
    switch(id) {
        case 0: io = &PIN_BTN_START; break;
        case 1: io = &PIN_BTN_SETTING; break;
        case 2: io = &PIN_BTN_STOP; break;
        case 3: io = &PIN_BTN_EMERGENCY; break;
    }
    return JS_NewBool(ctx, io ? tes_io_get_level(io) : 0);
}

static JSValue js_tes_read_voltage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_adc_read_voltage_sensor());
}
static JSValue js_tes_read_cp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, tes_adc_read_cp_voltage());
}

static JSValue js_tes_send_can(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    uint8_t data[8] = {0};
    int32_t len = 0;
    if (argc > 1 && JS_IsArray(ctx, argv[1])) {
        JSValue len_val = JS_GetPropertyStr(ctx, argv[1], "length");
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > 8) len = 8;
        for (int i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[1], i);
            int32_t val;
            JS_ToInt32(ctx, &val, item);
            data[i] = (uint8_t)val;
            JS_FreeValue(ctx, item);
        }
    }
    tes_can_send(id, data, len);
    return JS_UNDEFINED;
}

static JSValue js_oled_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    tes_oled_clear();
    return JS_UNDEFINED;
}
static JSValue js_oled_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    tes_oled_update();
    return JS_UNDEFINED;
}
static JSValue js_oled_drawText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t x, y;
    const char *str;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    str = JS_ToCString(ctx, argv[2]);
    if (str) { tes_oled_draw_str(x, y, str); JS_FreeCString(ctx, str); }
    return JS_UNDEFINED;
}

static JSValue js_sys_reboot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    esp_restart();
    return JS_UNDEFINED;
}
static JSValue js_sys_reset_wifi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    network_reset_config();
    return JS_UNDEFINED;
}

void safety_monitor_task(void *arg) {
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_js_heartbeat > JS_WATCHDOG_TIMEOUT_US) {
            ESP_LOGE(TAG, "JS Watchdog Timeout! Emergency Stop!");
            tes_io_set_level(&PIN_RELAY_MAIN, 0);
            tes_io_set_level(&PIN_RELAY_VP, 0);
            tes_io_set_level(&PIN_RELAY_LOCK, 0);
            tes_oled_clear();
            tes_oled_draw_str(0, 20, "SYSTEM ERROR");
            tes_oled_draw_str(0, 40, "JS HANG");
            tes_oled_update_emergency();
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// --- Init ---

// 定義 Helper: 綁定屬性
static void define_prop(JSContext *ctx, JSValue obj, const char *name, JSCFunction *getter, JSCFunction *setter) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, obj, atom, 
        getter ? JS_NewCFunction(ctx, getter, name, 0) : JS_UNDEFINED,
        setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | (setter ? JS_PROP_WRITABLE : 0)
    );
    JS_FreeAtom(ctx, atom);
}

void js_service_init(void) {
    rt = JS_NewRuntime();
    if (!rt) { ESP_LOGE(TAG, "Failed to create Runtime"); return; }
    JS_SetMemoryLimit(rt, 4 * 1024 * 1024);
    ctx = JS_NewContext(rt);
    last_js_heartbeat = esp_timer_get_time();

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue tes = JS_NewObject(ctx);
    
    // Core
    JS_SetPropertyStr(ctx, tes, "setRelay", JS_CFUNC(js_tes_set_relay, "setRelay", 2));
    JS_SetPropertyStr(ctx, tes, "setLed", JS_CFUNC(js_tes_set_led, "setLed", 2));
    JS_SetPropertyStr(ctx, tes, "getButton", JS_CFUNC(js_tes_get_button, "getButton", 1));
    JS_SetPropertyStr(ctx, tes, "readVoltage", JS_CFUNC(js_tes_read_voltage, "readVoltage", 0));
    JS_SetPropertyStr(ctx, tes, "readCP", JS_CFUNC(js_tes_read_cp, "readCP", 0));
    JS_SetPropertyStr(ctx, tes, "sendCAN", JS_CFUNC(js_tes_send_can, "sendCAN", 2));
    JS_SetPropertyStr(ctx, tes, "feedWatchdog", JS_CFUNC(js_tes_feed_watchdog, "feedWatchdog", 0));

    // Console
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_CFUNC(js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, tes, "console", console);

    // OLED
    JSValue oled_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, oled_obj, "clear", JS_CFUNC(js_oled_clear, "clear", 0));
    JS_SetPropertyStr(ctx, oled_obj, "drawText", JS_CFUNC(js_oled_drawText, "drawText", 3));
    JS_SetPropertyStr(ctx, oled_obj, "update", JS_CFUNC(js_oled_update, "update", 0));
    JS_SetPropertyStr(ctx, tes, "oled", oled_obj);

    // System
    JSValue sys = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sys, "reboot", JS_CFUNC(js_sys_reboot, "reboot", 0));
    JS_SetPropertyStr(ctx, sys, "resetWiFi", JS_CFUNC(js_sys_reset_wifi, "resetWiFi", 0));
    JS_SetPropertyStr(ctx, tes, "system", sys);

    // Vehicle (Read-Only)
    JSValue vehicle = JS_NewObject(ctx);
    define_prop(ctx, vehicle, "soc", js_vehicle_get_soc, NULL);
    define_prop(ctx, vehicle, "voltageRequest", js_vehicle_get_volt_req, NULL);
    define_prop(ctx, vehicle, "currentRequest", js_vehicle_get_curr_req, NULL);
    define_prop(ctx, vehicle, "chargingEnabled", js_vehicle_get_charging_enabled, NULL);
    define_prop(ctx, vehicle, "faultFlags", js_vehicle_get_fault, NULL);
    JS_SetPropertyStr(ctx, tes, "vehicle", vehicle);

    // Charger (Read/Write)
    JSValue charger = JS_NewObject(ctx);
    define_prop(ctx, charger, "outputVoltage", js_charger_get_volt, js_charger_set_volt);
    define_prop(ctx, charger, "outputCurrent", js_charger_get_curr, js_charger_set_curr);
    define_prop(ctx, charger, "relayStatus", js_charger_get_relay, js_charger_set_relay);
    JS_SetPropertyStr(ctx, tes, "charger", charger);

    JS_SetPropertyStr(ctx, global, "tes", tes);
    JS_FreeValue(ctx, global);

    // Load Script
    ESP_LOGI(TAG, "Loading main.js...");
    char *file_script = load_file("/fs/main.js");
    if (file_script) {
        JSValue val = JS_Eval(ctx, file_script, strlen(file_script), "main.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
            JSValue ex = JS_GetException(ctx);
            ESP_LOGE(TAG, "JS Init Error: %s", JS_ToCString(ctx, ex));
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, val);
        free(file_script);
    } else {
        const char *fallback = "tes.console.log('No script');";
        JS_Eval(ctx, fallback, strlen(fallback), "fallback", JS_EVAL_TYPE_GLOBAL);
    }
    
    xTaskCreate(safety_monitor_task, "safety_mon", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "JS Service Ready.");
}

void js_service_loop(void) {
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue loop_func = JS_GetPropertyStr(ctx, global, "loop");
    if (JS_IsFunction(ctx, loop_func)) {
        JSValue ret = JS_Call(ctx, loop_func, global, 0, NULL);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            ESP_LOGE(TAG, "JS Loop Error: %s", JS_ToCString(ctx, ex));
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, loop_func);
    JS_FreeValue(ctx, global);
}