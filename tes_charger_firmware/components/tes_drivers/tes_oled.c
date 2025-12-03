#include "tes_oled.h"
#include "u8g2.h"
#include "board_bsp.h" // 讀取 BOARD_OLED_CONFIG
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tes_i2c.h"

static const char *TAG = "TES_OLED";

// 引用 HAL 回調函數 (在 tes_u8g2_hal.c)
extern uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
extern uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

static u8g2_t u8g2;
static TaskHandle_t oled_task_handle = NULL;
static SemaphoreHandle_t update_sem = NULL;

// --- 背景刷新任務 ---
// 這個任務會一直等待信號，收到信號才透過 I2C 傳送資料
static void oled_refresh_task(void *arg) {
    while (1) {
        // 等待信號
        if (xSemaphoreTake(update_sem, portMAX_DELAY) == pdTRUE) {
            
            // 1. 嘗試拿 I2C 鎖
            if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                // 2. 拿到鎖了，安全發送
                u8g2_SendBuffer(&u8g2);
                
                // 3. 釋放鎖
                xSemaphoreGive(i2c_mutex);
            } else {
                // 拿不到鎖 (ADC 在忙)，這幀就丟掉，不要硬送
                // 這樣就不會報錯了
            }
        }
    }
}

// --- 內部函數：設定旋轉角度 ---
static void apply_rotation(tes_oled_rotation_t rotation) {
    const u8g2_cb_t *rot_cb;
    switch (rotation) {
        case OLED_ROTATION_90:  rot_cb = U8G2_R1; break;
        case OLED_ROTATION_180: rot_cb = U8G2_R2; break;
        case OLED_ROTATION_270: rot_cb = U8G2_R3; break;
        default:                rot_cb = U8G2_R0; break; // 0度
    }
    u8g2_SetDisplayRotation(&u8g2, rot_cb);
}

void tes_oled_init(void) {
    // 1. U8g2 初始化
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2, 
        U8G2_R0, // 初始方向，稍後會蓋掉
        u8g2_esp32_i2c_byte_cb, 
        u8g2_esp32_gpio_and_delay_cb
    );

    // 2. 應用 BSP 定義的旋轉角度 (關鍵！)
    apply_rotation(BOARD_OLED_CONFIG.rotation);

    // 3. 啟動螢幕
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0); // 喚醒
    
    // 4. 清除雜訊並設定字型
    u8g2_ClearBuffer(&u8g2);
    u8g2_SendBuffer(&u8g2); // 第一次同步發送，確保開機黑屏
    u8g2_SetFont(&u8g2, u8g2_font_ncenB08_tr);

    // 5. 建立背景任務 (實現非同步刷新)
    if (update_sem == NULL) {
        update_sem = xSemaphoreCreateBinary();
        // Stack Size 給 4096 比較保險，Priority 不需要太高
        xTaskCreate(oled_refresh_task, "oled_task", 4096, NULL, 10, &oled_task_handle);
    }
    
    ESP_LOGI(TAG, "OLED Initialized (Rotation: %d)", BOARD_OLED_CONFIG.rotation);
}

void tes_oled_clear(void) {
    // 只操作 RAM，不操作 I2C，速度極快
    u8g2_ClearBuffer(&u8g2);
}

void tes_oled_draw_str(int x, int y, const char *str) {
    // 只操作 RAM
    if (str) u8g2_DrawStr(&u8g2, x, y, str);
}

void tes_oled_update(void) {
    // 發送信號給背景任務，立即返回，不會卡住 JS 迴圈
    if (update_sem) {
        xSemaphoreGive(update_sem);
    }
}

void tes_oled_update_emergency(void) {
    // 強制直接呼叫 U8g2 發送，不經過 Task
    u8g2_SendBuffer(&u8g2);
}