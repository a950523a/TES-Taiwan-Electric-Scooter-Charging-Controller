/************************************************************************************
 * TES-0D-02-01 Compatible DC Charger Controller for ESP32
 * 
 * Copyright (c) 2025 黃丞左(Chris Huang)
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 
 * International License as published by Creative Commons.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * license for more details.
 *
 * You should have received a copy of the Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 * International License along with this program. If not, see
 * <http://creativecommons.org/licenses/by-nc-sa/4.0/>.
 *
 * --- 授權聲明 (中文) ---
 * 
 * 本程式碼，TES-0D-02-01 相容之直流充電樁控制器 (ESP32版)，由 黃丞左(Chris Huang) 於 2025 年創建。
 * 
 * 本程式碼採用「創用CC 姓名標示-非商業性-相同方式分享 4.0 國際」授權條款 (CC BY-NC-SA 4.0) 進行許可。
 * 
 * 您可以自由地：
 * - 分享 (Share)：在任何媒介以任何形式複製、發行本程式碼。
 * - 改作 (Adapt)：重混、轉換、及依本程式碼建立衍生作品。
 * 
 * 惟需遵照下列條件：
 * - 姓名標示 (Attribution)：您必須以適當方式標示本作品的創作者，提供授權條款的連結，並指出是否（及如何）對作品進行了變更。
 * - 非商業性 (NonCommercial)：您不得將本程式碼及衍生作品用於商業目的。
 * - 相同方式分享 (ShareAlike)：若您重混、轉換、或依本程式碼建立衍生作品，您必須採用與本程式碼相同的授權條款來散布您的作品。
 * 
 * 本程式碼是基於「按原樣」提供，不提供任何明示或暗示的保證，包括但不限於適銷性或特定用途適用性的保證。
 * 完整授權條款請參閱：<http://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh_TW>
 *
 ************************************************************************************/
/************************************************************************************
 * TES-0D-02-01 電動機車直流供電裝置 控制程式碼框架 (ESP32 + MCP2515)
 * RAM Optimized Version
 * * 注意:
 * 1. 本程式碼是一個根據標準文件流程圖提供的框架。
 * 2. 硬體相關的部分 (腳位定義, 感測器讀取, 繼電器控制) 需要根據實際硬體修改。
 * 3. 錯誤處理和所有的時序控制需要仔細實現和測試。
 * 4. 強烈建議在連接到實際高壓硬體前，在模擬環境或低壓環境下充分測試。
 * 5. 本程式碼僅供參考，使用者需對其應用負全部責任。
 ************************************************************************************/
#include <Arduino.h>
#include "Config.h"
#include "HAL/HAL.h"
#include "UI/UI.h"
#include "ChargerLogic/ChargerLogic.h"
#include "CAN_Protocol/CAN_Protocol.h"
#include "LuxBeacon/LuxBeacon.h"

// --- FreeRTOS 任務函數原型 ---
void can_task(void *pvParameters);
void logic_task(void *pvParameters);
void ui_task(void *pvParameters);
void monitor_task(void *pvParameters);

// --- FreeRTOS 同步工具 ---
// 為CAN數據創建一個互斥鎖
SemaphoreHandle_t canDataMutex;

TaskHandle_t canTaskHandle = NULL;
TaskHandle_t logicTaskHandle = NULL;
TaskHandle_t uiTaskHandle = NULL;

void setup() {
    Serial.begin(115200);
    Serial.println(F("DC Charger Controller Booting Up..."));

    canDataMutex = xSemaphoreCreateMutex();
    if (canDataMutex == NULL) {
        Serial.println("FATAL: Failed to create canDataMutex!");
    }

    // 按順序初始化各層
    hal_init_pins();
    hal_init_adc();
    hal_init_can();

    logic_init();        
    ui_init();           

    beacon_init();

    Serial.println(F("System Initialized. Creating FreeRTOS tasks..."));

    xTaskCreate(
        can_task,
        "CAN_Task",
        3072, // 堆疊大小 (Bytes)
        NULL,
        5,    // 優先級 (數字越大越高)
        &canTaskHandle
    );

    xTaskCreate(
        logic_task,
        "Logic_Task",
        4096,
        NULL,
        4,
        &logicTaskHandle
    );

    xTaskCreate(
        ui_task,
        "UI_Task",
        4096,
        NULL,
        3,
        &uiTaskHandle
    );

    xTaskCreate(
        monitor_task, 
        "Monitor_Task", 
        2048, 
        NULL, 
        1, 
        NULL
    );

    vTaskDelete(NULL);
}

void loop() {
}

void can_task(void *pvParameters) {
    Serial.println("CAN Task started.");
    for (;;) {
        can_protocol_handle_receive();
        
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

void logic_task(void *pvParameters) {
    Serial.println("Logic Task started.");
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20); 

    for (;;) {
        if (ui_get_current_state() == UI_STATE_NORMAL) {
            logic_run_statemachine();
            logic_handle_periodic_tasks();
        }
        
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void ui_task(void *pvParameters) {
    Serial.println("UI Task started.");
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50); 

    for (;;) {
        ui_handle_input();

        LedState current_led_state = logic_get_led_state();
        hal_update_leds(current_led_state);
        ui_update_display();

        beacon_handle_tasks();
        
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void monitor_task(void *pvParameters) {
    Serial.println("System Monitor Task started.");
    for (;;) {
        // 每10秒打印一次報告
        vTaskDelay(pdMS_TO_TICKS(10000));

        UBaseType_t can_stack_hwm = uxTaskGetStackHighWaterMark(canTaskHandle);
        UBaseType_t logic_stack_hwm = uxTaskGetStackHighWaterMark(logicTaskHandle);
        UBaseType_t ui_stack_hwm = uxTaskGetStackHighWaterMark(uiTaskHandle);

        Serial.println("\n--- RTOS STATUS ---");
        // 打印的是剩餘的最小值，單位是字(4 bytes)
        Serial.printf("CAN Task Stack HWM: %u words (%u bytes)\n", can_stack_hwm, can_stack_hwm * 4);
        Serial.printf("Logic Task Stack HWM: %u words (%u bytes)\n", logic_stack_hwm, logic_stack_hwm * 4);
        Serial.printf("UI Task Stack HWM: %u words (%u bytes)\n", ui_stack_hwm, ui_stack_hwm * 4);
        Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
        Serial.println("-------------------\n");
    }
}