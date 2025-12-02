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
    int32_t state;
    if (JS_ToInt32(ctx, &state, argv[0])) return JS_EXCEPTION;
    
    tes_io_set_level(&PIN_RELAY_MAIN, state);
    return JS_UNDEFINED;
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

    
    JS_SetPropertyStr(ctx, tes, "setRelay", JS_NewCFunction(ctx, js_tes_set_relay, "setRelay", 1));
    JS_SetPropertyStr(ctx, tes, "readVoltage", JS_NewCFunction(ctx, js_tes_read_voltage, "readVoltage", 0));
    JS_SetPropertyStr(ctx, tes, "readCP", JS_NewCFunction(ctx, js_tes_read_cp, "readCP", 0));
    JS_SetPropertyStr(ctx, tes, "sendCAN", JS_NewCFunction(ctx, js_tes_send_can, "sendCAN", 2));

    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "tes", tes);
    JS_FreeValue(ctx, global);

    ESP_LOGI(TAG, "Loading main.js...");
    
    char *file_script = load_file("/fs/main.js");
    
    if (file_script) {
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
    // 呼叫 JS 的 'loop' 函數
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue loop_func = JS_GetPropertyStr(ctx, global, "loop");
    
    if (JS_IsFunction(ctx, loop_func)) {
        JSValue ret = JS_Call(ctx, loop_func, global, 0, NULL);
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
    }
    
    JS_FreeValue(ctx, loop_func);
    JS_FreeValue(ctx, global);
}
