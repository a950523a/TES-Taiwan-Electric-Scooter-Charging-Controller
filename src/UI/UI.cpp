// src/UI/UI.cpp

#include "UI.h"
#include "Config.h"
#include "Version.h"
#include "HAL/HAL.h"
#include <U8g2lib.h>
#include <Wire.h>

// --- 私有(static)變量 ---
// ... (此處省略了所有 static 變數，它們維持不變) ...
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
static UIState currentUIState = UI_STATE_NORMAL;
static bool isOledConnected = false;
static unsigned long lastDisplayUpdateTime = 0;
static unsigned long savedScreenStartTime = 0;
static int mainMenuSelection = 0;
static const char* mainMenuItems[] = {"Max Voltage", "Max Current", "Target SOC", "Save & Exit"};
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
    
    // --- [修正] 使用傳入的 data.chargerState 來判斷 ---
    if (currentUIState == UI_STATE_NORMAL && data.chargerState == STATE_CHG_IDLE) {
        if (settingIsPressed) {
            if (settingButtonPressedTime == 0) {
                settingButtonPressedTime = millis();
            } else if (!isSettingButtonLongPress && (millis() - settingButtonPressedTime > LONG_PRESS_DURATION_MS)) {
                isSettingButtonLongPress = true;
                Serial.println(F("UI: Long press detected. Entering settings menu."));
                tempSetting_Voltage = logic_get_max_voltage_setting();
                tempSetting_Current = logic_get_max_current_setting();
                tempSetting_SOC = logic_get_target_soc_setting();
                currentUIState = UI_STATE_MENU_MAIN;
                mainMenuSelection = 0;
            }
        } else {
            if (settingButtonPressedTime > 0 && !isSettingButtonLongPress) {
                Serial.println(F("UI: Short press detected. Cycling Target SOC."));
                int current_soc = logic_get_target_soc_setting();
                int next_soc = (current_soc < 90) ? 90 : (current_soc < 100) ? 100 : 80;
                unsigned int current_v = logic_get_max_voltage_setting();
                unsigned int current_a = logic_get_max_current_setting();
                logic_save_config(current_v, current_a, next_soc);
            }
            settingButtonPressedTime = 0;
            isSettingButtonLongPress = false;
        }
    } else {
        settingButtonPressedTime = 0;
        isSettingButtonLongPress = false;
    }

    // ... (此處省略了 handle_input 的其餘部分，它們維持不變) ...
    static bool settingKeyWasPressed = false;
    bool settingTrigger = settingIsPressed && !settingKeyWasPressed;
    settingKeyWasPressed = settingIsPressed;
    if (currentUIState == UI_STATE_MENU_SAVED) {
        if (millis() - savedScreenStartTime > SAVED_SCREEN_DURATION_MS) {
            currentUIState = UI_STATE_NORMAL;
        }
        return;
    }
    if (currentUIState == UI_STATE_NORMAL) {
        return;
    }
    bool upAction_single = false;
    bool downAction_single = false;
    bool upAction_repeat = false;
    bool downAction_repeat = false;
    unsigned long now = millis();
    if (upIsPressed) {
        if (keyRepeatStartTime == 0) {
            keyRepeatStartTime = now;
            nextRepeatTime = now + KEY_REPEAT_INITIAL_DELAY_MS;
            upAction_single = true;
        } else if (now >= nextRepeatTime) {
            nextRepeatTime = now + KEY_REPEAT_INTERVAL_MS;
            upAction_repeat = true;
        }
    } else if (downIsPressed) {
        if (keyRepeatStartTime == 0) {
            keyRepeatStartTime = now;
            nextRepeatTime = now + KEY_REPEAT_INITIAL_DELAY_MS;
            downAction_single = true;
        } else if (now >= nextRepeatTime) {
            nextRepeatTime = now + KEY_REPEAT_INTERVAL_MS;
            downAction_repeat = true;
        }
    } else {
        keyRepeatStartTime = 0;
    }
    bool action_triggered = settingTrigger || upAction_single || downAction_single || upAction_repeat || downAction_repeat;
    if (currentUIState != UI_STATE_NORMAL && action_triggered) {
        force_display_update = true;
    }
    switch (currentUIState) {
        case UI_STATE_MENU_MAIN:
            if (upAction_single) mainMenuSelection = (mainMenuSelection == 0) ? 3 : mainMenuSelection - 1;
            if (downAction_single) mainMenuSelection = (mainMenuSelection + 1) % 4;
            if (settingTrigger) {
                if (mainMenuSelection == 0) currentUIState = UI_STATE_MENU_SET_VOLTAGE;
                else if (mainMenuSelection == 1) currentUIState = UI_STATE_MENU_SET_CURRENT;
                else if (mainMenuSelection == 2) currentUIState = UI_STATE_MENU_SET_SOC;
                else if (mainMenuSelection == 3) {
                    logic_save_config(tempSetting_Voltage, tempSetting_Current, tempSetting_SOC);
                    currentUIState = UI_STATE_MENU_SAVED;
                    savedScreenStartTime = millis();
                }
            }
            break;
        case UI_STATE_MENU_SET_VOLTAGE:
            if (upAction_single) tempSetting_Voltage += 1;
            if (downAction_single) tempSetting_Voltage -= 1;
            if (upAction_repeat) tempSetting_Voltage += 10;
            if (downAction_repeat) tempSetting_Voltage -= 10;
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_Voltage = constrain(tempSetting_Voltage, HARDWARE_MIN_VOLTAGE_0_1V, HARDWARE_MAX_VOLTAGE_0_1V);
            break;
        case UI_STATE_MENU_SET_CURRENT:
            if (upAction_single) tempSetting_Current += 1;
            if (downAction_single) tempSetting_Current -= 1;
            if (upAction_repeat) tempSetting_Current += 10;
            if (downAction_repeat) tempSetting_Current -= 10;
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_Current = constrain(tempSetting_Current, HARDWARE_MIN_CURRENT_0_1A, HARDWARE_MAX_CURRENT_0_1A);
            break;
        case UI_STATE_MENU_SET_SOC:
            if (upAction_single || upAction_repeat) tempSetting_SOC += 1;
            if (downAction_single || downAction_repeat) tempSetting_SOC -= 1;
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            tempSetting_SOC = constrain(tempSetting_SOC, HARDWARE_MIN_SOC, HARDWARE_MAX_SOC);
            break;
        default: break;
    }
}

// --- ui_update_display 函式維持不變，因此省略以節省篇幅 ---
// ...
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
                case UI_STATE_MENU_MAIN:
                    u8g2.setFont(u8g2_font_ncenB10_tr);
                    u8g2.drawStr(0, 12, "Settings");
                    u8g2.drawHLine(0, 14, 128);
                    u8g2.setFont(u8g2_font_ncenB08_tr);
                    for (int i = 0; i < 4; i++) {
                        if (i == mainMenuSelection) u8g2.drawStr(5, 28 + i * 12, ">");
                        u8g2.drawStr(15, 28 + i * 12, mainMenuItems[i]);
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
            switch (data.chargerState) {
                case STATE_CHG_IDLE:
                    if (data.isFaultLatched) {
                        u8g2.setFont(u8g2_font_ncenB12_tr);
                        strWidth = u8g2.getStrWidth("ERROR!");
                        u8g2.drawStr((128 - strWidth) / 2, 30, "ERROR!");
                        u8g2.setFont(u8g2_font_ncenB08_tr);
                        strWidth = u8g2.getStrWidth("Press START to reset");
                        u8g2.drawStr((128 - strWidth) / 2, 50, "Press START to reset");
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
                    sprintf(buffer, "%.1fV / %.1fA", data.measuredVoltage, data.measuredCurrent);
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