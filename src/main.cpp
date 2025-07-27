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

void setup() {
    Serial.begin(115200);
    Serial.println(F("DC Charger Controller Booting Up... (Layered Arch)"));

    // 按順序初始化各層
    hal_init_pins();
    hal_init_adc();
    hal_init_can();
    
    logic_init();        // 初始化核心邏輯層
    ui_init();           // 初始化UI層

    Serial.println(F("System Initialized."));
}

void loop() {
    // 1. 處理使用者輸入 (菜單操作等)
    ui_handle_input();

    // 2. 只有在正常模式下才運行充電邏輯
    if (ui_get_current_state() == UI_STATE_NORMAL) {
        can_protocol_handle_receive();
        logic_run_statemachine();
        logic_handle_periodic_tasks(); // 這個函數內部會處理CAN的收發
    }
    // 3. 更新硬體狀態 (LED)
    LedState current_led_state = logic_get_led_state();
    hal_update_leds(current_led_state);

    // 4. 更新顯示螢幕
    ui_update_display();
}