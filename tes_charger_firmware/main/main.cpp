#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"

// 引入驅動
#include "board_bsp.h"
#include "tes_can.h"
#include "tes_i2c.h"
#include "tes_adc.h"
#include "tes_oled.h"
#include "network_manager.h"

// 引入 QuickJS
#include "quickjs.h"
#include "js_service.h"


static const char *TAG = "MAIN";

// --- 自定義錯誤列印函數 (取代 js_std_dump_error) ---
void js_custom_dump_error(JSContext *ctx) {
    JSValue exception_val = JS_GetException(ctx);
    
    // 取得錯誤訊息
    const char *str = JS_ToCString(ctx, exception_val);
    if (str) {
        ESP_LOGE(TAG, "JS Exception: %s", str);
        JS_FreeCString(ctx, str);
    }
    
    // 嘗試取得 Stack Trace
    if (JS_IsError(ctx, exception_val)) {
        JSValue stack_val = JS_GetPropertyStr(ctx, exception_val, "stack");
        if (!JS_IsUndefined(stack_val)) {
            const char *stack = JS_ToCString(ctx, stack_val);
            if (stack) {
                ESP_LOGE(TAG, "Stack Trace:\n%s", stack);
                JS_FreeCString(ctx, stack);
            }
        }
        JS_FreeValue(ctx, stack_val);
    }
    
    JS_FreeValue(ctx, exception_val);
}

// --- 簡單的 JS 綁定函數範例: print() ---
static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            ESP_LOGI("JS_PRINT", "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

void init_filesystem() {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",         // 掛載點，以後存取檔案用 /fs/main.js
        .partition_label = "vfs",   // 必須對應 partitions.csv 裡的名字
        .format_if_mount_failed = true, // 如果第一次掛載失敗(未格式化)，自動格式化
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS Mounted: Total: %d KB, Used: %d KB", total / 1024, used / 1024);
    }
}

extern "C" void app_main(void)
{
    // 初始化硬體
    nvs_flash_init(); 
    init_filesystem();
    tes_io_init(&PIN_LED_STATUS, true);
    tes_io_init(&PIN_RELAY_MAIN, true);
    tes_can_init();
    tes_i2c_init();
    tes_adc_init();
    tes_oled_init();

    const char *default_html = 
    "<!DOCTYPE html><html><body>"
    "<h2>TES Charger</h2>"
    "<a href='/api/scan'>Scan WiFi</a><br>"
    "<form action='/api/connect' method='post'>SSID:<input name='ssid'><br>PASS:<input name='pass'><input type='submit'></form>"
    "<hr><h3>Upload JS</h3><form action='/api/upload_js' method='post' enctype='text/plain'><input type='file' name='file'><input type='submit'></form>"
    "</body></html>";

    // 在 main 中檢查
    FILE *f = fopen("/fs/index.html", "r");
    if (!f) {
        f = fopen("/fs/index.html", "w");
        fprintf(f, default_html);
        fclose(f);
    }
    network_init(); 

    // 啟動 JS 服務
    js_service_init();

    while (1) {
        // 讓 JS 引擎有機會執行 (如果有實作 Event Loop)
        js_service_loop(); 
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
