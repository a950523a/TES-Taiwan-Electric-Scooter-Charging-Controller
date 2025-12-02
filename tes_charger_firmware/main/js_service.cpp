#include "js_service.h"
#include "board_bsp.h"
#include "tes_adc.h"
#include "tes_can.h"
#include "tes_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "tes_oled.h"
#include "network_manager.h"

extern "C" {
    #include "quickjs.h"
    // 如果有自定義的 console.log 也可以放這裡
}

static const char *TAG = "JS_SVC";
static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;

// 讀取檔案內容的 Helper 函數
static char* load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "Script file not found: %s", path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buffer = (char *)malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, f);
        buffer[length] = '\0'; // 確保 Null 結尾
    }
    fclose(f);
    return buffer;
}

// --- 綁定函數實作 ---

// tes.setRelay(state)
static JSValue js_tes_set_relay(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id, state;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &state, argv[1])) return JS_EXCEPTION;
    
    const tes_io_t *io = NULL;
    switch(id) {
        case 0: io = &PIN_RELAY_MAIN; break;
        case 1: io = &PIN_RELAY_VP; break;
        case 2: io = &PIN_RELAY_LOCK; break;
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
    
    int val = 0;
    if (io) val = tes_io_get_level(io);
    return JS_NewBool(ctx, val); // 回傳 true/false
}

// tes.readVoltage() -> float
static JSValue js_tes_read_voltage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    float v = tes_adc_read_voltage_sensor();
    return JS_NewFloat64(ctx, v);
}

// tes.readCP() -> float
static JSValue js_tes_read_cp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    float v = tes_adc_read_cp_voltage();
    return JS_NewFloat64(ctx, v);
}

// tes.sendCAN(id, [data...])
static JSValue js_tes_send_can(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;

    // 解析陣列參數
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

static void *js_malloc_func(JSMallocState *s, size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 如果 PSRAM 分配失敗，嘗試內部 RAM
    if (!ptr) ptr = malloc(size); 
    return ptr;
}

static void js_free_func(JSMallocState *s, void *ptr) {
    free(ptr);
}

static void *js_realloc_func(JSMallocState *s, void *ptr, size_t size) {
    void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_ptr) new_ptr = realloc(ptr, size);
    return new_ptr;
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            ESP_LOGI("JS_CONSOLE", "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_oled_drawText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t x, y;
    const char *str;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    str = JS_ToCString(ctx, argv[2]);
    
    tes_oled_draw_str(x, y, str);
    tes_oled_update(); // 簡單起見，畫完直接刷新
    
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// --- LED 控制 ---
static JSValue js_tes_set_led(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t id, state;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &state, argv[1])) return JS_EXCEPTION;
    
    const tes_io_t *io = NULL;
    switch(id) {
        case 0: io = &PIN_LED_STANDBY; break;
        case 1: io = &PIN_LED_CHARGING; break;
        case 2: io = &PIN_LED_ERROR; break;
    }
    
    if (io) tes_io_set_level(io, state);
    return JS_UNDEFINED;
}

// --- OLED 清除 ---
static JSValue js_oled_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    // 呼叫 OLED 驅動
    tes_oled_clear();
    return JS_UNDEFINED;
}

// --- OLED 更新 (刷新畫面) ---
static JSValue js_oled_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    // 呼叫 OLED 驅動
    tes_oled_update();
    return JS_UNDEFINED;
}

// tes.system.reboot()
static JSValue js_sys_reboot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    esp_restart();
    return JS_UNDEFINED;
}

// tes.system.resetWiFi()
static JSValue js_sys_reset_wifi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    network_reset_config(); // 呼叫我們之前寫好的函數
    return JS_UNDEFINED;
}

// --- 初始化與註冊 ---
void js_service_init(void) {
    JSMallocFunctions mf = {
        .js_malloc = js_malloc_func,
        .js_free = js_free_func,
        .js_realloc = js_realloc_func,
        .js_malloc_usable_size = NULL // 可以留空
    };

    ESP_LOGI(TAG, "Creating JS Runtime...");
    rt = JS_NewRuntime();
    if (!rt) { ESP_LOGE(TAG, "Failed to create Runtime"); return; }

    ESP_LOGI(TAG, "Creating JS Context...");
    ctx = JS_NewContext(rt);
    if (!ctx) { ESP_LOGE(TAG, "Failed to create Context"); return; }
    
    ESP_LOGI(TAG, "Registering Globals...");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue tes = JS_NewObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue oled_obj = JS_NewObject(ctx);

    
    JS_SetPropertyStr(ctx, tes, "setRelay", JS_NewCFunction(ctx, js_tes_set_relay, "setRelay", 2));
    JS_SetPropertyStr(ctx, tes, "getButton", JS_NewCFunction(ctx, js_tes_get_button, "getButton", 1));
    JS_SetPropertyStr(ctx, tes, "setLed", JS_NewCFunction(ctx, js_tes_set_led, "setLed", 2));
    JS_SetPropertyStr(ctx, tes, "readVoltage", JS_NewCFunction(ctx, js_tes_read_voltage, "readVoltage", 0));
    JS_SetPropertyStr(ctx, tes, "readCP", JS_NewCFunction(ctx, js_tes_read_cp, "readCP", 0));
    JS_SetPropertyStr(ctx, tes, "sendCAN", JS_NewCFunction(ctx, js_tes_send_can, "sendCAN", 2));

    JS_SetPropertyStr(ctx, oled_obj, "clear", JS_NewCFunction(ctx, js_oled_clear, "clear", 0));
    JS_SetPropertyStr(ctx, oled_obj, "drawText", JS_NewCFunction(ctx, js_oled_drawText, "drawText", 3));
    JS_SetPropertyStr(ctx, oled_obj, "update", JS_NewCFunction(ctx, js_oled_update, "update", 0)); 

    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, tes, "oled", oled_obj);
    JS_SetPropertyStr(ctx, tes, "console", console);

    JS_SetPropertyStr(ctx, global, "tes", tes);
    JS_FreeValue(ctx, global);

    JSValue sys = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sys, "reboot",    JS_NewCFunction(ctx, js_sys_reboot,     "reboot",    0));
    JS_SetPropertyStr(ctx, sys, "resetWiFi", JS_NewCFunction(ctx, js_sys_reset_wifi, "resetWiFi", 0));
    JS_SetPropertyStr(ctx, tes, "system", sys);

    ESP_LOGI(TAG, "Loading main.js...");
    
    char *file_script = load_file("/fs/main.js");
    
    if (file_script) {
        ESP_LOGI(TAG, "File Size: %d bytes", strlen(file_script));
    
        // --- 新增：印出檔案內容的前 100 個字元 ---
        printf("--- SCRIPT START ---\n");
        printf("%.*s\n", 100, file_script); // 只印前100字避免洗版
        printf("--- SCRIPT END ---\n");
        ESP_LOGI(TAG, "Executing /fs/main.js");
        JSValue val = JS_Eval(ctx, file_script, strlen(file_script), "main.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
            // 使用我們在 main.cpp 定義的錯誤處理，或者在這裡再實作一次
            // js_custom_dump_error(ctx); 
            // 簡單起見，這裡先用簡單的 print
            JSValue ex = JS_GetException(ctx);
            ESP_LOGE(TAG, "JS Error in file: %s", JS_ToCString(ctx, ex));
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, val);
        free(file_script);
    } else {
        ESP_LOGW(TAG, "Using embedded fallback script");
        const char *fallback_script = 
            "console.log('Running Fallback Script');"
            "globalThis.loop = function() {"
            "  let v = tes.readVoltage();"
            "  console.log('V:', v);"
            "};";
        JS_Eval(ctx, fallback_script, strlen(fallback_script), "fallback", JS_EVAL_TYPE_GLOBAL);
    }
    
    ESP_LOGI(TAG, "JS Init Done!");
}

void js_service_loop(void) {
    if (!ctx) return;

    // 1. 取得全域物件 (Global Object)
    JSValue global = JS_GetGlobalObject(ctx);
    
    // 2. 嘗試從全域物件中找到名為 "loop" 的屬性
    JSValue loop_func = JS_GetPropertyStr(ctx, global, "loop");
    
    // 3. 檢查它是不是一個函數
    if (JS_IsFunction(ctx, loop_func)) {
        // 4. 呼叫它
        JSValue ret = JS_Call(ctx, loop_func, global, 0, NULL);
        
        // 5. 檢查執行錯誤
        if (JS_IsException(ret)) {
            JSValue exception_val = JS_GetException(ctx);
            const char *str = JS_ToCString(ctx, exception_val);
            if (str) {
                ESP_LOGE(TAG, "JS Error in loop(): %s", str);
                JS_FreeCString(ctx, str);
            }
            JS_FreeValue(ctx, exception_val);
        }
        JS_FreeValue(ctx, ret);
    } else {
        // 如果找不到函數，每秒印一次警告 (避免洗版，你可以加個計數器)
        ESP_LOGW(TAG, "Global function 'loop' not found!");
    }
    
    // 6. 清理記憶體 (非常重要！)
    JS_FreeValue(ctx, loop_func);
    JS_FreeValue(ctx, global);
}
