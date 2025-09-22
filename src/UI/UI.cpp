// src/UI/UI.cpp

#include "UI.h"
#include "Config.h"
#include "Version.h"
#include "HAL/HAL.h"
#include <U8g2lib.h>
#include <Wire.h>
#include "OTAManager/OTAManager.h"

// --- 私有(static)變量 ---
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
static UIState currentUIState = UI_STATE_NORMAL;
static bool isOledConnected = false;
static unsigned long lastDisplayUpdateTime = 0;
static unsigned long savedScreenStartTime = 0;
static int mainMenuSelection = 0;
static int mainMenuOffset = 0;
const int MAX_MENU_ITEMS_ON_SCREEN = 4;
static const char* mainMenuItems[] = {"Max Voltage", "Max Current", "Target SOC", "Save & Exit", "Network", "About"};
static int aboutMenuSelection = 0; 
static int updateMenuSelection = 0; 
static const char* updateMenuItemText = "Check for Updates";
static unsigned int tempSetting_Voltage;
static unsigned int tempSetting_Current;
static int tempSetting_SOC;
static unsigned long settingButtonPressedTime = 0;
static bool isSettingButtonLongPress = false;
static unsigned long keyRepeatStartTime = 0;
static unsigned long nextRepeatTime = 0;
const unsigned long KEY_REPEAT_INITIAL_DELAY_MS = 400;
const unsigned long KEY_REPEAT_INTERVAL_MS = 80;
static bool force_display_update = false;


// --- 引用外部函數以獲取初始設定值和保存設定 ---
// 這是 UI 層唯一需要與 Logic 層直接交互的地方
extern unsigned int logic_get_max_voltage_setting();
extern unsigned int logic_get_max_current_setting();
extern int logic_get_target_soc_setting();
extern void logic_save_config(unsigned int voltage, unsigned int current, int soc);

extern bool filesystem_version_mismatch;
extern void net_reset_wifi_credentials();

static byte findOledDevice() {
    byte common_addresses[] = {0x3C, 0x3D};
    Wire.begin();
    for (byte i = 0; i < sizeof(common_addresses); i++) {
        byte addr = common_addresses[i];
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) return addr;
    }
    return 0;
}

void ui_init() {
    byte oledAddress = findOledDevice();
    if (oledAddress > 0) {
        u8g2.setI2CAddress(oledAddress * 2);
        if (u8g2.begin()) {
            isOledConnected = true;
            Serial.println(F("UI: OLED display initialized successfully."));
            u8g2.setFlipMode(1);
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_ncenB10_tr);
            uint16_t strWidth = u8g2.getStrWidth("TES Charger");
            u8g2.drawStr((128 - strWidth) / 2, 25, "TES Charger");
            u8g2.setFont(u8g2_font_ncenB08_tr);
            strWidth = u8g2.getStrWidth("Booting...");
            u8g2.drawStr((128 - strWidth) / 2, 45, "Booting...");
            u8g2.setFont(u8g2_font_6x10_tr);
            strWidth = u8g2.getStrWidth(FIRMWARE_VERSION);
            u8g2.drawStr(128 - strWidth - 2, 62, FIRMWARE_VERSION);
            u8g2.sendBuffer();
            delay(2500);
        } else {
            isOledConnected = false;
            Serial.println(F("UI: Device found, but U8g2 initialization failed."));
        }
    } else {
        isOledConnected = false;
        Serial.println(F("UI: No OLED display found."));
    }
}

UIState ui_get_current_state() {
    return currentUIState;
}

void ui_handle_input(const DisplayData& data) {
    if (!isOledConnected) return;

    bool settingIsPressed = hal_get_button_state(BUTTON_SETTING);
    bool upIsPressed = hal_get_button_state(BUTTON_START);
    bool downIsPressed = hal_get_button_state(BUTTON_STOP);
    
    bool upShortPressTrigger = false;
    bool downShortPressTrigger = false;
    bool upRepeatTrigger = false;
    bool downRepeatTrigger = false;
    bool settingShortPressTrigger = false;
    bool settingLongPressTrigger = false;

    unsigned long now = millis();

    // 處理 上/下 按鈕的短按與重複
    if (upIsPressed) {
        if (keyRepeatStartTime == 0) {
            keyRepeatStartTime = now;
            nextRepeatTime = now + KEY_REPEAT_INITIAL_DELAY_MS;
            upShortPressTrigger = true;
        } else if (now >= nextRepeatTime) {
            nextRepeatTime = now + KEY_REPEAT_INTERVAL_MS;
            upRepeatTrigger = true;
        }
    } else if (downIsPressed) {
        if (keyRepeatStartTime == 0) {
            keyRepeatStartTime = now;
            nextRepeatTime = now + KEY_REPEAT_INITIAL_DELAY_MS;
            downShortPressTrigger = true;
        } else if (now >= nextRepeatTime) {
            nextRepeatTime = now + KEY_REPEAT_INTERVAL_MS;
            downRepeatTrigger = true;
        }
    } else {
        keyRepeatStartTime = 0;
    }

    // 處理 Setting 按鈕的長短按
    if (settingIsPressed) {
        if (settingButtonPressedTime == 0) {
            settingButtonPressedTime = now;
            isSettingButtonLongPress = false;
        }
        if (!isSettingButtonLongPress && (now - settingButtonPressedTime > LONG_PRESS_DURATION_MS)) {
            settingLongPressTrigger = true;
            isSettingButtonLongPress = true;
        }
    } else {
        if (settingButtonPressedTime > 0 && !isSettingButtonLongPress) {
            settingShortPressTrigger = true;
        }
        settingButtonPressedTime = 0;
        isSettingButtonLongPress = false;
    }

    // 如果有任何按鍵動作，強制刷新螢幕
    if (upShortPressTrigger || downShortPressTrigger || upRepeatTrigger || downRepeatTrigger || settingShortPressTrigger || settingLongPressTrigger) {
        force_display_update = true;
    }
    switch (currentUIState) {
        case UI_STATE_NORMAL:
            if (data.chargerState == STATE_CHG_IDLE) {
                if (settingLongPressTrigger) {
                    Serial.println(F("UI: Long press detected. Entering settings menu."));
                    tempSetting_Voltage = logic_get_max_voltage_setting();
                    tempSetting_Current = logic_get_max_current_setting();
                    tempSetting_SOC = logic_get_target_soc_setting();
                    currentUIState = UI_STATE_MENU_MAIN;
                    mainMenuSelection = 0;
                    mainMenuOffset = 0;
                } else if (settingShortPressTrigger) {
                    Serial.println(F("UI: Short press detected. Cycling Target SOC."));
                    int current_soc = logic_get_target_soc_setting();
                    int next_soc = (current_soc < 90) ? 90 : (current_soc < 100) ? 100 : 80;
                    unsigned int current_v = logic_get_max_voltage_setting();
                    unsigned int current_a = logic_get_max_current_setting();
                    logic_save_config(current_v, current_a, next_soc);
                }
            }
            break;

        case UI_STATE_MENU_MAIN: {
            const int totalMenuItems = sizeof(mainMenuItems) / sizeof(mainMenuItems[0]);
            if (upShortPressTrigger) mainMenuSelection = (mainMenuSelection == 0) ? (totalMenuItems - 1) : (mainMenuSelection - 1);
            if (downShortPressTrigger) mainMenuSelection = (mainMenuSelection + 1) % totalMenuItems;
            if (mainMenuSelection < mainMenuOffset) mainMenuOffset = mainMenuSelection;
            else if (mainMenuSelection >= mainMenuOffset + MAX_MENU_ITEMS_ON_SCREEN) mainMenuOffset = mainMenuSelection - MAX_MENU_ITEMS_ON_SCREEN + 1;
            
            if (settingShortPressTrigger) {
                if (mainMenuSelection == 0) currentUIState = UI_STATE_MENU_SET_VOLTAGE;
                else if (mainMenuSelection == 1) currentUIState = UI_STATE_MENU_SET_CURRENT;
                else if (mainMenuSelection == 2) currentUIState = UI_STATE_MENU_SET_SOC;
                else if (mainMenuSelection == 3) {
                    logic_save_config(tempSetting_Voltage, tempSetting_Current, tempSetting_SOC);
                    currentUIState = UI_STATE_MENU_SAVED;
                    savedScreenStartTime = millis();
                }
                else if (mainMenuSelection == 4) {
                    currentUIState = UI_STATE_MENU_NETWORK;
                }
                else if (mainMenuSelection == 5) {
                    currentUIState = UI_STATE_MENU_ABOUT;
                }
            }
            break;
        }

        case UI_STATE_MENU_NETWORK:
            if (settingShortPressTrigger) {
                // 觸發 Wi-Fi 重置
                net_reset_wifi_credentials();
            }
            if (settingLongPressTrigger) {
                currentUIState = UI_STATE_MENU_MAIN;
            }
            break;
        
        case UI_STATE_MENU_ABOUT:
            if (settingShortPressTrigger) {
                currentUIState = UI_STATE_MENU_UPDATE_OPTIONS;
            }
            if (settingLongPressTrigger) {
                currentUIState = UI_STATE_MENU_MAIN;
            }
            break;

        case UI_STATE_MENU_UPDATE_OPTIONS:
            // --- [修正] 簡化輸入處理邏輯 ---
            // 在這個頁面，"上/下" 按鈕沒有作用，因為只有一個選項
            
            if (settingShortPressTrigger) {
                // 根據是否有可用更新，來決定執行哪個動作
                if (data.updateAvailable) {
                    ota_start_full_update(); // 如果有更新，就執行更新
                } else {
                    ota_start_check(); // 如果沒有更新，就執行檢查
                }
            }
            if (settingLongPressTrigger) {
                currentUIState = UI_STATE_MENU_ABOUT;
            }
            break;

        case UI_STATE_MENU_SET_VOLTAGE:
            if (upShortPressTrigger) tempSetting_Voltage += 1;
            if (downShortPressTrigger) tempSetting_Voltage -= 1;
            if (upRepeatTrigger) tempSetting_Voltage += 10;
            if (downRepeatTrigger) tempSetting_Voltage -= 10;
            if (settingLongPressTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_Voltage = constrain(tempSetting_Voltage, HARDWARE_MIN_VOLTAGE_0_1V, HARDWARE_MAX_VOLTAGE_0_1V);
            break;
        case UI_STATE_MENU_SET_CURRENT:
            if (upShortPressTrigger) tempSetting_Current += 1;
            if (downShortPressTrigger) tempSetting_Current -= 1;
            if (upRepeatTrigger) tempSetting_Current += 10;
            if (downRepeatTrigger) tempSetting_Current -= 10;
            if (settingLongPressTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_Current = constrain(tempSetting_Current, HARDWARE_MIN_CURRENT_0_1A, HARDWARE_MAX_CURRENT_0_1A);
            break;
        case UI_STATE_MENU_SET_SOC:
            if (upShortPressTrigger || upRepeatTrigger) tempSetting_SOC += 1;
            if (downShortPressTrigger || downRepeatTrigger) tempSetting_SOC -= 1;
            if (settingLongPressTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_SOC = constrain(tempSetting_SOC, HARDWARE_MIN_SOC, HARDWARE_MAX_SOC);
            break;
        
        case UI_STATE_MENU_SAVED:
            if (millis() - savedScreenStartTime > SAVED_SCREEN_DURATION_MS) {
                currentUIState = UI_STATE_NORMAL;
            }
            break;
    }
}

void ui_update_display(const DisplayData& data) {
    if (!isOledConnected) return;
    if ((millis() - lastDisplayUpdateTime >= DISPLAY_UPDATE_INTERVAL_MS) || force_display_update) {
        lastDisplayUpdateTime = millis();
        force_display_update = false;
        u8g2.clearBuffer();
        char buffer[32];
        uint16_t strWidth;
        if (currentUIState != UI_STATE_NORMAL) {
            switch (currentUIState) {
                case UI_STATE_MENU_MAIN: { 
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Settings");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    const int totalMenuItems = sizeof(mainMenuItems) / sizeof(mainMenuItems[0]);
                    for (int i = 0; i < MAX_MENU_ITEMS_ON_SCREEN; i++) {
                        int itemIndex = mainMenuOffset + i;
                        if (itemIndex < totalMenuItems) {
                            int yPos = 28 + i * 12;
                            if (itemIndex == mainMenuSelection) {
                                u8g2.drawStr(5, yPos, ">");
                            }
                            u8g2.drawStr(15, yPos, mainMenuItems[itemIndex]);
                        }
                    }
                    if (totalMenuItems > MAX_MENU_ITEMS_ON_SCREEN) {
                        int scrollBarHeight = 64 - 16;
                        int handleHeight = scrollBarHeight * MAX_MENU_ITEMS_ON_SCREEN / totalMenuItems;
                        int handleY = 16 + scrollBarHeight * mainMenuOffset / totalMenuItems;
                        u8g2.drawFrame(126, 16, 2, scrollBarHeight);
                        u8g2.drawBox(126, handleY, 2, handleHeight);
                    }
                    break;
                }

                case UI_STATE_MENU_ABOUT:
                    if (data.filesystemMismatch) {
                        u8g2.setFont(u8g2_font_ncenB10_tr);
                        u8g2.drawStr(0, 12, "System Error");
                        u8g2.drawHLine(0, 14, 128);
                        u8g2.setFont(u8g2_font_5x8_tr); // 使用更小的字體
                        u8g2.drawStr(0, 26, "Web UI version mismatch.");
                        u8g2.drawStr(0, 36, "Please update filesystem.");
                        
                        // 仍然提供進入 Update Options 的入口
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        u8g2.drawStr(5, 60, ">");
                        u8g2.drawStr(15, 60, "Update Options");
                    } else {
                        u8g2.setFont(u8g2_font_ncenB10_tr);
                        u8g2.drawStr(0, 12, "About");
                        u8g2.drawHLine(0, 14, 128);
                        u8g2.setFont(u8g2_font_5x8_tr);
                        sprintf(buffer, "FW: %s", data.currentFirmwareVersion);
                        u8g2.drawStr(0, 24, buffer);
                        sprintf(buffer, "FS: %s", data.filesystemVersion);
                        u8g2.drawStr(0, 33, buffer);
                        u8g2.drawStr(0, 42, "Copyright (c) 2025 C.H.");
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        u8g2.drawStr(5, 60, ">");
                        u8g2.drawStr(15, 60, "Update Options");
                    }
                    break;

                case UI_STATE_MENU_NETWORK:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Network Settings");
                    u8g2.drawHLine(0, 14, 128);

                    u8g2.setFont(u8g2_font_6x10_tr);
                    sprintf(buffer, "Mode: %s", data.wifiMode);
                    u8g2.drawStr(0, 28, buffer);
                    sprintf(buffer, "SSID: %s", data.wifiSSID);
                    u8g2.drawStr(0, 40, buffer);
                    sprintf(buffer, "IP: %s", data.ipAddress);
                    u8g2.drawStr(0, 52, buffer);

                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    u8g2.drawStr(5, 62, ">");
                    u8g2.drawStr(15, 62, "Reset WiFi");
                    break;
                
                case UI_STATE_MENU_UPDATE_OPTIONS:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Update Options");
                    u8g2.drawHLine(0, 14, 128);

                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    
                    // --- [修改] 合併 OTA 選項 ---
                    u8g2.drawStr(5, 38, ">");
                    if (data.updateAvailable) {
                        sprintf(buffer, "Update to %s", data.latestFirmwareVersion);
                        u8g2.drawStr(15, 38, buffer);
                    } else {
                        u8g2.drawStr(15, 38, "Check for Updates");
                    }

                    u8g2.setFont(u8g2_font_5x8_tr);
                    strWidth = u8g2.getStrWidth(data.otaStatusMessage);
                    u8g2.drawStr((128 - strWidth) / 2, 62, data.otaStatusMessage);

                    if ((ota_get_status() == OTA_DOWNLOADING_FW || ota_get_status() == OTA_DOWNLOADING_FS) && data.otaProgress > 0) {
                        u8g2.drawBox(0, 56, (int)(128 * (data.otaProgress / 100.0)), 8);
                    }
                    break;
                
                case UI_STATE_MENU_SAVED:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    strWidth = u8g2.getStrWidth("Settings Saved!");
                    u8g2.drawStr((128 - strWidth) / 2, 12, "Settings Saved!");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_7x13_tr);
                    sprintf(buffer, "Max V: %.1f V", data.maxVoltageSetting_0_1V / 10.0);
                    u8g2.drawStr(5, 30, buffer);
                    sprintf(buffer, "Max A: %.1f A", data.maxCurrentSetting_0_1A / 10.0);
                    u8g2.drawStr(5, 45, buffer);
                    sprintf(buffer, "SOC  : %d %%", data.targetSOC);
                    u8g2.drawStr(5, 60, buffer);
                    break;
                case UI_STATE_MENU_SET_VOLTAGE:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Set Max Voltage");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_7x13B_tr);
                    sprintf(buffer, "%.1f V", tempSetting_Voltage / 10.0);
                    u8g2.drawStr(20, 40, buffer);
                    break;
                case UI_STATE_MENU_SET_CURRENT:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Set Max Current");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_7x13B_tr);
                    sprintf(buffer, "%.1f A", tempSetting_Current / 10.0);
                    u8g2.drawStr(20, 40, buffer);
                    break;
                case UI_STATE_MENU_SET_SOC:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Set Target SOC");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_7x13B_tr);
                    sprintf(buffer, "%d %%", tempSetting_SOC);
                    u8g2.drawStr(30, 40, buffer);
                    break;
                default: break;
            }
        } else {
            if (filesystem_version_mismatch) {
                u8g2.setFont(u8g2_font_unifont_t_symbols);
                u8g2.drawGlyph(118, 12, 0x26a0); // 警告符號 ⚠
            }
            switch (data.chargerState) {
                case STATE_CHG_IDLE:
                    if (data.isFaultLatched) {
                        u8g2.setFont(u8g2_font_ncenB12_tr);
                        u8g2.drawStr(0, 12, "ERROR!");
                        u8g2.setFont(u8g2_font_6x10_tr);
                        sprintf(buffer, "Last Req: %.1fA", data.lastValidRequestedCurrent);
                        u8g2.drawStr(0, 28, buffer);
                        sprintf(buffer, "Fault Flags: 0x%02X", data.lastFaultFlags);
                        u8g2.drawStr(0, 40, buffer);
                        
                        // 新增 Ready 和 Target SOC 顯示
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        sprintf(buffer, "Ready (SOC:%d%%)", data.targetSOC);
                        uint16_t strWidth = u8g2.getStrWidth(buffer);
                        u8g2.drawStr((128 - strWidth) / 2, 60, buffer);
                    } else if (data.isChargeComplete) {
                        u8g2.setFont(u8g2_font_ncenB10_tr);
                        strWidth = u8g2.getStrWidth("Charge Complete");
                        u8g2.drawStr((128 - strWidth) / 2, 25, "Charge Complete");
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        sprintf(buffer, "Target SOC: %d%%", data.targetSOC);
                        strWidth = u8g2.getStrWidth(buffer);
                        u8g2.drawStr((128 - strWidth) / 2, 45, buffer);
                        u8g2.setFont(u8g2_font_6x10_tr);
                        strWidth = u8g2.getStrWidth("Ready for Next Charge");
                        u8g2.drawStr((128 - strWidth) / 2, 62, "Ready for Next Charge");
                    } else {
                        u8g2.setFont(u8g2_font_ncenB10_tr);
                        strWidth = u8g2.getStrWidth("Ready to Charge");
                        u8g2.drawStr((128 - strWidth) / 2, 25, "Ready to Charge");
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        sprintf(buffer, "Target SOC: %d%%", data.targetSOC);
                        strWidth = u8g2.getStrWidth(buffer);
                        u8g2.drawStr((128 - strWidth) / 2, 45, buffer);
                        u8g2.setFont(u8g2_font_6x10_tr);
                        strWidth = u8g2.getStrWidth("Connect Scooter");
                        u8g2.drawStr((128 - strWidth) / 2, 62, "Connect Scooter");
                    }
                    break;
                case STATE_CHG_INITIAL_PARAM_EXCHANGE:
                case STATE_CHG_PRE_CHARGE_OPERATIONS:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    strWidth = u8g2.getStrWidth("Connecting...");
                    u8g2.drawStr((128 - strWidth) / 2, 30, "Connecting...");
                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    strWidth = u8g2.getStrWidth("Handshaking with BMS");
                    u8g2.drawStr((128 - strWidth) / 2, 50, "Handshaking with BMS");
                    break;
                case STATE_CHG_DC_CURRENT_OUTPUT:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    sprintf(buffer, "SOC: %d %%", data.soc);
                    u8g2.drawStr(5, 18, buffer);
                    if (data.isTimerRunning && data.totalTimeSeconds > 0) {
                        uint16_t remainingMinutes = (data.remainingSeconds + 30) / 60;
                        uint16_t hours = remainingMinutes / 60;
                        uint16_t minutes = remainingMinutes % 60;
                        if (hours > 0) sprintf(buffer, "Time: %dh %dm", hours, minutes);
                        else sprintf(buffer, "Time: %d min", remainingMinutes);
                    } else {
                        strcpy(buffer, "Time: ...");
                    }
                    u8g2.drawStr(5, 40, buffer);
                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    sprintf(buffer, "%.1fV/%.1fA/Req:%.1fA", data.measuredVoltage, data.measuredCurrent, data.vehicleRequestedCurrent);
                    u8g2.drawStr(5, 60, buffer);
                    break;
                case STATE_CHG_ENDING_CHARGE_PROCESS:
                case STATE_CHG_FINALIZATION:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    strWidth = u8g2.getStrWidth("Finalizing...");
                    u8g2.drawStr((128 - strWidth) / 2, 38, "Finalizing...");
                    break;
                default:
                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    strWidth = u8g2.getStrWidth("System Fault");
                    u8g2.drawStr((128 - strWidth) / 2, 38, "System Fault");
                    break;
            }
        }
        u8g2.sendBuffer();
    }
}