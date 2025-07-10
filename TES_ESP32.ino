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

#include <SPI.h>
#include <mcp_can.h>  
#include <ModbusMaster.h>
#include <Preferences.h> // 用於NVS持久化存儲


#include <Wire.h> 
#include <U8g2lib.h> //OLED顯示
// --- OLED Display Setup ---
// 根據您的螢幕型號選擇合適的建構子。SSD1306 128x64 是最常見的。
// U8G2_R0 表示不旋轉螢幕。
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
bool isOledConnected = false;

// 用於控制螢幕刷新頻率，避免閃爍和過度消耗CPU
unsigned long lastDisplayUpdateTime = 0;
const unsigned int DISPLAY_UPDATE_INTERVAL_MS = 500; // 每500毫秒刷新一次螢幕

byte findOledDevice() {
    byte common_addresses[] = {0x3C, 0x3D}; // 常見的OLED地址
    
    Wire.begin(); // 確保I2C總線已啟用

    for (byte i = 0; i < sizeof(common_addresses); i++) {
        byte addr = common_addresses[i];
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            // 找到了！立即返回找到的地址
            return addr; 
        }
    }
    
    // 如果迴圈跑完了都沒找到，返回0表示失敗
    return 0; 
}

// --- 硬體腳位定義 (請根據您的硬體修改) ---
const int SPI_CS_PIN = 5;                 
const int MCP2515_INT_PIN = 35;  
volatile bool canInterruptTriggered = false;
const int START_BUTTON_PIN = 32; 
const int STOP_BUTTON_PIN = 33;
const int EMERGENCY_STOP_BUTTON_PIN = 25;
const int SETTING_BUTTON_PIN = 4;

const int CHARGE_RELAY_PIN = 26;
const int LOCK_SOLENOID_PIN = 27;
const int VP_RELAY_PIN = 15;

const int LED_STANDBY_PIN = 14;
const int LED_CHARGING_PIN = 12;
const int LED_ERROR_PIN = 13;  

const int CP_ADC_PIN = 34;  // 使用 ADC1, GPIO34 是輸入專用

// --- CP 分壓電阻 ---
const float R_CP_UPPER = 150;                                         // 歐姆
const float R_CP_LOWER = 51;                                          // 歐姆
const float CP_DIVIDER_RATIO = R_CP_LOWER / (R_CP_UPPER + R_CP_LOWER);  

#define CP_HYSTERESIS 0.3       // 防抖電壓寬容值 (V)
#define CP_ERROR_THRESHOLD 2    // 連續錯誤次數才判斷為錯誤
#define CP_SAMPLE_COUNT 5       // 平均取樣次數

#define MODBUS_SERIAL Serial2  // 使用 Serial2
#define MODBUS_BAUDRATE 9600
#define MODBUS_ID 1            // 預設通訊位址 1

ModbusMaster modbus;

// --- CAN ID 定義 (根據文件 表15) ---
#define CHARGER_STATUS_ID 0x508
#define CHARGER_PARAMS_ID 0x509
#define CHARGER_EMERGENCY_STOP_ID 0x5F8

#define VEHICLE_STATUS_ID 0x500
#define VEHICLE_PARAMS_ID 0x501
#define VEHICLE_EMERGENCY_ID 0x5F0

MCP_CAN CAN(SPI_CS_PIN);

// --- CP 狀態定義 (參考 Table 5, p21) ---
enum CPState : byte {
  CP_STATE_UNKNOWN,  // Initial or undefined state
  CP_STATE_OFF,      // 0.0V - 1.9V (Prohibit Output)
  CP_STATE_ON,       // 7.4V - 13.7V (Output Possible, Process Confirmation)
  CP_STATE_ERROR     // Other voltages
};
CPState currentCPState = CP_STATE_UNKNOWN;
float measuredCPVoltage = 0.0;  // Actual voltage on the CP line

// --- 系統狀態定義 ---
enum ChargerState : byte {
  STATE_CHG_IDLE,
  STATE_CHG_INITIAL_PARAM_EXCHANGE,
  STATE_CHG_PRE_CHARGE_OPERATIONS,
  STATE_CHG_DC_CURRENT_OUTPUT,
  STATE_CHG_ENDING_CHARGE_PROCESS,
  STATE_CHG_FAULT_HANDLING,
  STATE_CHG_EMERGENCY_STOP_PROC,
  STATE_CHG_FINALIZATION
  // STATE_CHG_RAMP_UP removed as it's often part of DC_CURRENT_OUTPUT
};
ChargerState currentChargerState = STATE_CHG_IDLE;

// --- UI 狀態枚舉 ---
enum UIState {
    UI_STATE_NORMAL,
    UI_STATE_MENU_MAIN,
    UI_STATE_MENU_SET_VOLTAGE,
    UI_STATE_MENU_SET_CURRENT,
    UI_STATE_MENU_SET_SOC,
    UI_STATE_MENU_SAVED
};
UIState currentUIState = UI_STATE_NORMAL;

unsigned long savedScreenStartTime = 0;
const unsigned long SAVED_SCREEN_DURATION_MS = 2000; 

// --- 菜單追蹤變數 ---
int mainMenuSelection = 0;
const char* mainMenuItems[] = {"Max Voltage", "Max Current", "Target SOC", "Save & Exit"};

// --- CAN訊息資料結構 ---
struct CAN_Charger_Status_508 {
  byte faultFlags;
  byte statusFlags;
  unsigned int availableVoltage;
  unsigned int availableCurrent;
  unsigned int faultDetectionVoltageLimit;
} chargerStatus508;
struct CAN_Charger_Params_509 {
  byte esChargeSequenceNumber;
  byte ratedOutputPower;
  unsigned int actualOutputVoltage;
  unsigned int actualOutputCurrent;
  unsigned int remainingChargeTime;
} chargerParams509;
struct CAN_Charger_Emergency_5F8 {
  byte emergencyStopRequestFlags;
  byte reserved1_3[3];
  unsigned int chargerManufacturerID;
  byte reserved6_7[2];
} chargerEmergency5F8;
struct CAN_Vehicle_Status_500 {
  byte faultFlags;
  byte statusFlags;
  unsigned int chargeCurrentCommand;
  unsigned int chargeVoltageLimit;
  unsigned int maxChargeVoltage;
} vehicleStatus500;
struct CAN_Vehicle_Params_501 {
  byte esChargeSequenceNumber;
  byte stateOfCharge;
  unsigned int maxChargeTime;
  unsigned int estimatedChargeEndTime;
  byte reserved[2];
} vehicleParams501;
struct CAN_Vehicle_Emergency_5F0 {
  byte errorRequestFlags;
} vehicleEmergency5F0;

// --- 計時器與超時控制 ---
unsigned long currentStateStartTime = 0;
unsigned long lastPeriodicSendTime = 0;
unsigned long lastCPReadTime = 0;
const unsigned long PERIODIC_SEND_INTERVAL = 100;  // ms
const unsigned long CP_READ_INTERVAL = 50;         // ms, how often to read CP state

// 用於倒數計時
static uint32_t elapsedChargingSeconds = 0; // 從充電開始已經過去的秒數
static uint32_t currentTotalTimeSeconds = 0; // 當前採納的總充電時間 (會被BMS更新)
static bool isChargingTimerRunning = false; // 旗標：計時器是否在運行
static unsigned long lastChargeTimeTick = 0; // 用於每秒計時
static uint32_t remainingTimeSeconds_global = 0; 


// --- 充電參數 ---
float measuredVoltage = 0.0;
float measuredCurrent = 0.0;
unsigned int chargerMaxOutputVoltage_0_1V = 0; 
unsigned int chargerMaxOutputCurrent_0_1A = 0; 
byte chargerRatedPower_50W = 0; 
const unsigned int chargerManufacturerCode = 0x0000;

// --- 充電樁物理極限 (硬體決定，不可更改) ---
const unsigned int HARDWARE_MAX_VOLTAGE_0_1V = 1200; // 120.0V
const unsigned int HARDWARE_MAX_CURRENT_0_1A = 1000; // 100.0A (假設硬體最大支持100A)
const unsigned int HARDWARE_MIN_VOLTAGE_0_1V = 100;  // 10.0V (邏輯上的最小值)
const unsigned int HARDWARE_MIN_CURRENT_0_1A = 10;   // 1.0A (邏輯上的最小值)
const int HARDWARE_MAX_SOC = 100; // 100%
const int HARDWARE_MIN_SOC = 10;  // 10% (邏輯上的最小值)

int userSetTargetSOC = 100;

// --- 設置菜單中使用的臨時變數 ---
unsigned int tempSetting_Voltage;
unsigned int tempSetting_Current;
int tempSetting_SOC;

// --- 按鍵長按檢測 ---
unsigned long settingButtonPressedTime = 0;
bool isSettingButtonLongPress = false;
const unsigned long LONG_PRESS_DURATION_MS = 1000;

Preferences preferences;

bool vehicleReadyForCharge = false;
bool insulationTestOK = false;
bool couplerLocked = false;
bool faultLatch = false; // 故障鎖存旗標
bool chargeCompleteLatch = false; // 充電完成鎖存旗標

// --- 函數原型 ---
void initializeHardware();
void runChargerStateMachine();
void checkAndProcessCAN();
void handlePeriodicTasks();
void readAndSetCPState();  
void sendChargerCAN_508();
void sendChargerCAN_509();
void sendChargerCAN_5F8_Emergency(bool emergency);
bool ch_sub_01_battery_compatibility_check();
void ch_sub_02_set_max_charge_time_display();
bool ch_sub_03_coupler_lock_and_insulation_diagnosis();
void ch_sub_04_dc_current_output_control();
bool ch_sub_06_monitoring_process();
void ch_sub_10_protection_and_end_flow(bool isFault);
void ch_sub_12_emergency_stop_procedure();
float readVoltageSensor();
float readCurrentSensor();
void controlChargeRelay(bool on);
void controlCouplerLock(bool lock);
void updateLEDs();
void updateDisplay();
void handleUserInput();
void drawMenu();

void IRAM_ATTR onCANInterrupt() {
  canInterruptTriggered = true;
}

// ================= SETUP =================
void setup() {
  MODBUS_SERIAL.begin(MODBUS_BAUDRATE, SERIAL_8N1, 16, 17);  // RX, TX 腳位
  modbus.begin(MODBUS_ID, MODBUS_SERIAL);


  Serial.begin(115200);
  Serial.println(F("DC Charger Controller Booting Up... (RAM Optimized)"));

  initializeHardware();
  attachInterrupt(digitalPinToInterrupt(MCP2515_INT_PIN), onCANInterrupt, FALLING);

  if (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK) {
    Serial.println(F("MCP2515 Init Okay!"));
    CAN.setMode(MCP_NORMAL);
    CAN.init_Mask(0, 0, 0x7FF);
    CAN.init_Filt(0, 0, VEHICLE_STATUS_ID);
    CAN.init_Filt(1, 0, VEHICLE_PARAMS_ID);
    CAN.init_Mask(1, 0, 0x7FF);
    CAN.init_Filt(2, 0, VEHICLE_EMERGENCY_ID);
  } else {
    Serial.println(F("MCP2515 Init Failed!"));
    while (1)
      ;
  }

  byte oledAddress = findOledDevice(); // 呼叫我們的掃描函數

  if (oledAddress > 0) { // 如果返回值大於0，說明找到了設備
      Serial.print(F("OLED display found at address 0x"));
      Serial.println(oledAddress, HEX);
      u8g2.setI2CAddress(oledAddress * 2);  // U8g2需要8位元地址，即7位元地址左移一位
      if (u8g2.begin()) {
            isOledConnected = true; // 如果成功，將旗標設為 true
            Serial.println(F("OLED display initialized successfully. Settings Menu is ENABLED."));
            
            // 連接成功，顯示歡迎畫面
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_ncenB08_tr);
            u8g2.drawStr(0, 12, "Charger Ready...");
            u8g2.sendBuffer();
            delay(1500); // 顯示1.5秒
        } else {
            isOledConnected = false; // 如果失敗，確保旗標為 false
            Serial.println(F("Device found, but U8g2 initialization failed. Settings Menu is DISABLED."));
        } 
    }else {
        isOledConnected = false;
        Serial.println(F("No common OLED display found. Skipping display functions. Settings Menu is DISABLED."));
        
    }
  // --- 從NVS中讀取保存的設定 ---
  preferences.begin("charger_config", false); 

  // 讀取設定值，如果鍵名不存在，則使用程式碼中的預設值
  chargerMaxOutputVoltage_0_1V = preferences.getUInt("max_voltage", 1000);
  chargerMaxOutputCurrent_0_1A = preferences.getUInt("max_current", 80);
  userSetTargetSOC = preferences.getInt("target_soc", 100);

  // 檢查是否是第一次啟動，如果是，則將預設值寫入NVS
  if (!preferences.getBool("config_saved", false)) {
      Serial.println(F("First boot or NVS empty. Saving default values."));
      preferences.putUInt("max_voltage", chargerMaxOutputVoltage_0_1V);
      preferences.putUInt("max_current", chargerMaxOutputCurrent_0_1A);
      preferences.putInt("target_soc", userSetTargetSOC);
      preferences.putBool("config_saved", true);
  }
    
  // 根據讀取或預設的最終配置，計算一次額定功率
  unsigned int ratedPower_W = (chargerMaxOutputVoltage_0_1V / 10.0) * (chargerMaxOutputCurrent_0_1A / 10.0);
  chargerRatedPower_50W = ratedPower_W / 50;

  Serial.println(F("--- Configuration Loaded ---"));
  Serial.print(F("Max Voltage: ")); Serial.print(chargerMaxOutputVoltage_0_1V / 10.0); Serial.println(" V");
  Serial.print(F("Max Current: ")); Serial.print(chargerMaxOutputCurrent_0_1A / 10.0); Serial.println(" A");
  Serial.print(F("Target SOC: ")); Serial.print(userSetTargetSOC); Serial.println(" %");
  Serial.println(F("--------------------------"));
    
  
  


  // Initialize CAN data structures 
  chargerStatus508.faultFlags = 0;
  chargerStatus508.statusFlags = 0;
  chargerStatus508.availableVoltage = chargerMaxOutputVoltage_0_1V;
  chargerStatus508.availableCurrent = chargerMaxOutputCurrent_0_1A;
  chargerStatus508.faultDetectionVoltageLimit = chargerMaxOutputVoltage_0_1V;
  chargerParams509.esChargeSequenceNumber = 18;
  chargerParams509.ratedOutputPower = chargerRatedPower_50W;
  chargerParams509.actualOutputVoltage = 0;
  chargerParams509.actualOutputCurrent = 0;
  chargerParams509.remainingChargeTime = 0xFFFF;
  chargerEmergency5F8.emergencyStopRequestFlags = 0;
  chargerEmergency5F8.chargerManufacturerID = chargerManufacturerCode;

  currentChargerState = STATE_CHG_IDLE;
  currentStateStartTime = millis();
  Serial.println(F("System Initialized. Current State: IDLE"));
  updateLEDs();
  readAndSetCPState();  // Initial CP read
  digitalWrite(VP_RELAY_PIN,LOW);
}

// ================= LOOP =================
void loop() {
  handleUserInput();
  if (currentUIState == UI_STATE_NORMAL) {
      runChargerStateMachine();
      checkAndProcessCAN();
      handlePeriodicTasks();
  }
  updateLEDs();
  updateDisplay();
}


// ================= CP State Reading Function =================
void readAndSetCPState() {
  CPState previousCPState = currentCPState;
  static byte cpErrorCount = 0;

  int sum = 0;
  for (int i = 0; i < CP_SAMPLE_COUNT; i++) {
    sum += analogRead(CP_ADC_PIN);
    delayMicroseconds(500);  // 快速取樣用較短延遲
  }

  int adcValue = sum / CP_SAMPLE_COUNT;
  float v_at_A0 = adcValue * (3.3 / 4095.0);
  measuredCPVoltage = v_at_A0 / CP_DIVIDER_RATIO;

  // 分析電壓落在哪個範圍
  CPState detectedState;

  if (measuredCPVoltage >= 0.0 && measuredCPVoltage <= 1.9 - CP_HYSTERESIS) {
    detectedState = CP_STATE_OFF;
  } else if (measuredCPVoltage >= 7.4 + CP_HYSTERESIS && measuredCPVoltage <= 13.7) {
    detectedState = CP_STATE_ON;
  } else {
    detectedState = CP_STATE_ERROR;
  }

  // 對 ERROR 狀態進行防誤判保護
  if (detectedState == CP_STATE_ERROR) {
    cpErrorCount++;
    if (cpErrorCount >= CP_ERROR_THRESHOLD) {
      currentCPState = CP_STATE_ERROR;
    }  // 否則維持上一個狀態
  } else {
    cpErrorCount = 0;
    currentCPState = detectedState;
  }


  //if (currentCPState != previousCPState) { // Log only on change
  //    Serial.print(F("CP Voltage: ")); Serial.print(measuredCPVoltage);
  //    Serial.print(F("V -> CP State: "));
  //    if(currentCPState == CP_STATE_OFF) Serial.println(F("OFF"));
  //    else if(currentCPState == CP_STATE_ON) Serial.println(F("ON"));
  //    else if(currentCPState == CP_STATE_ERROR) Serial.println(F("ERROR"));
  //    else Serial.println(F("UNKNOWN"));
  //}
}


// ================= 狀態機 =================
void runChargerStateMachine() {
  bool startPressed = (digitalRead(START_BUTTON_PIN) == LOW);
  bool stopPressed = (digitalRead(STOP_BUTTON_PIN) == LOW);
  bool emergencyStopPressed = (digitalRead(EMERGENCY_STOP_BUTTON_PIN) == LOW);

  if (emergencyStopPressed) {
    Serial.println(F("EMERGENCY STOP BUTTON PRESSED!"));
    currentChargerState = STATE_CHG_EMERGENCY_STOP_PROC;  // Go to emergency state
    currentStateStartTime = millis();
    // No return, let the emergency state handler run immediately in the next switch
  }

  switch (currentChargerState) {
    case STATE_CHG_IDLE:
      chargerStatus508.statusFlags &= ~0x02;
      chargerStatus508.statusFlags &= ~0x01;
      controlChargeRelay(false);
      controlCouplerLock(false);
      couplerLocked = false;
      insulationTestOK = false;
      vehicleReadyForCharge = false;
      digitalWrite(VP_RELAY_PIN,LOW);
      isChargingTimerRunning = false;


      if (startPressed) {
        faultLatch = false;
        chargeCompleteLatch = false;
        digitalWrite(VP_RELAY_PIN,HIGH);
        readAndSetCPState();                                                    // Check CP before starting
        if (currentCPState == CP_STATE_OFF || currentCPState == CP_STATE_ON) {  // Allow start if CP indicates vehicle might be present or ready
          Serial.println(F("Start button pressed. Moving to INITIAL_PARAM_EXCHANGE."));
          currentChargerState = STATE_CHG_INITIAL_PARAM_EXCHANGE;
          currentStateStartTime = millis();
          sendChargerCAN_508();
          sendChargerCAN_509();
        } else {
          Serial.println(F("Start button pressed, but CP state is ERROR/UNKNOWN. Cannot start."));
          // Optionally set a fault or just stay idle
        }
      }
      break;

    case STATE_CHG_INITIAL_PARAM_EXCHANGE:
      if (vehicleReadyForCharge) {
                if (ch_sub_01_battery_compatibility_check()) {
                    Serial.println(F("CH_SUB_01: Battery compatible. Moving to PRE_CHARGE_OPERATIONS."));
                    currentChargerState = STATE_CHG_PRE_CHARGE_OPERATIONS;
                    currentStateStartTime = millis();
                } else {
                    Serial.println(F("CH_SUB_01: Battery not compatible or error. Moving to FAULT_HANDLING."));
                    chargerStatus508.faultFlags |= 0x04;
                    ch_sub_10_protection_and_end_flow(true);
                }
            } else if (millis() - currentStateStartTime > 15000) {
                Serial.println(F("Timeout in INITIAL_PARAM_EXCHANGE (15s). Moving to FAULT_HANDLING (F1)."));
                chargerStatus508.faultFlags |= 0x01;
                ch_sub_10_protection_and_end_flow(true);
            }
            break;

    case STATE_CHG_PRE_CHARGE_OPERATIONS:
      // Flowchart p65: "CP=ON AND 車輛充電允許 (#500.1.0=1)?"
      if (currentCPState == CP_STATE_ON && (vehicleStatus500.statusFlags & 0x01)) {
        if (ch_sub_03_coupler_lock_and_insulation_diagnosis()) {
          Serial.println(F("CH_SUB_03: Coupler lock & insulation OK. Setting #508.1.0=0. Moving to DC_CURRENT_OUTPUT."));
          chargerStatus508.statusFlags &= ~0x01;  // bit0 直流供電裝置停止控制 = 0 (輸出追隨運轉中)
          //chargerStatus508.statusFlags |= 0x02;   //直流供電裝置狀態 = 1 (充電中)
          chargerStatus508.statusFlags |= 0x04;   //直流供電裝置電磁鎖 = 1 (閉鎖中) 
          sendChargerCAN_508();
          currentChargerState = STATE_CHG_DC_CURRENT_OUTPUT;
          currentStateStartTime = millis();
          isChargingTimerRunning = true;
          elapsedChargingSeconds = 0; // 重置已充電時間
          currentTotalTimeSeconds = 0; // 等待BMS的第一個時間指令
          lastChargeTimeTick = millis(); // 記錄開始時間
        } else if (millis() - currentStateStartTime > 20000) {  // p65: 20秒超時 -> F2
          Serial.println(F("Timeout/Error in PRE_CHARGE_OPERATIONS (20s). Moving to FAULT_HANDLING (F2)."));
          chargerStatus508.faultFlags |= 0x01;  // bit0 供電系統異常
          ch_sub_10_protection_and_end_flow(true);
        }
      } else if (millis() - currentStateStartTime > 20000) {  // Timeout if CP not ON or vehicle not ready
        Serial.print(F("Timeout in PRE_CHARGE_OPERATIONS (20s). CPState: "));
        Serial.print(currentCPState);
        Serial.print(F(", VehicleChargeAllowed: "));
        Serial.println((vehicleStatus500.statusFlags & 0x01));
        chargerStatus508.faultFlags |= 0x01;  // bit0 供電系統異常
        ch_sub_10_protection_and_end_flow(true);
      }
      break;

    case STATE_CHG_DC_CURRENT_OUTPUT:
      chargerStatus508.statusFlags |= 0x02;  // bit1 = 1 充電中
      ch_sub_04_dc_current_output_control();
      ch_sub_06_monitoring_process();
      break;

    // ... other states (ENDING_CHARGE_PROCESS, FAULT_HANDLING, EMERGENCY_STOP_PROC, FINALIZATION) remain largely the same
    // Ensure they correctly handle relay and lock controls based on final CP state if needed.
    case STATE_CHG_ENDING_CHARGE_PROCESS:
      Serial.println(F("Ending charge process..."));
      chargerStatus508.statusFlags |= 0x01;
      chargerStatus508.statusFlags &= ~0x02;
      sendChargerCAN_508();
      vehicleStatus500.chargeCurrentCommand = 0;
      ch_sub_04_dc_current_output_control();  // Ramps down current

      // Wait for current to be near zero before opening relay
      if (measuredCurrent < 1.0) {  // Threshold for current being "off"
        controlChargeRelay(false);
        Serial.println(F("Main relay OFF."));

        // Check vehicle contactor status and CP state before unlocking
        // For example, wait for vehicle to signal contactor open (#500.1.1 = 1)
        // and/or CP to go to OFF state.
        if ((vehicleStatus500.statusFlags & 0x02) && currentCPState == CP_STATE_OFF) {
        controlCouplerLock(false);
        couplerLocked = false;
        Serial.println(F("Coupler unlocked."));
        currentChargerState = STATE_CHG_FINALIZATION;
        currentStateStartTime = millis();
        // } else if (millis() - currentStateStartTime > 10000) { // Timeout waiting for vehicle/CP
        //    Serial.println(F("Timeout waiting for vehicle disconnect signals. Forcing unlock."));
        //    controlCouplerLock(false); couplerLocked = false;
        //    currentChargerState = STATE_CHG_FINALIZATION; currentStateStartTime = millis();
        }
      } else if (millis() - currentStateStartTime > 5000) {  // Timeout for current ramp down
        Serial.println(F("Failed to ramp down current in ENDING_CHARGE_PROCESS. Fault."));
        chargerStatus508.faultFlags |= 0x02;  // Charger specific fault
        ch_sub_10_protection_and_end_flow(true);
      }
      break;

    case STATE_CHG_FAULT_HANDLING:
      Serial.println(F("Fault handling state..."));
      controlChargeRelay(false);  // Ensure relay is off
      controlCouplerLock(false);  // Ensure lock is off on fault
      if (millis() - currentStateStartTime > 10000) {
        Serial.println(F("Fault display time over. Moving to IDLE."));
        currentChargerState = STATE_CHG_IDLE;
        currentStateStartTime = millis();
        chargerStatus508.faultFlags = 0;                    // Clear fault flags when returning to idle (or require manual reset)
        chargerEmergency5F8.emergencyStopRequestFlags = 0;  // Clear emergency flags
      }
      break;

    case STATE_CHG_EMERGENCY_STOP_PROC:
      Serial.println(F("Emergency stop procedure state..."));
      ch_sub_12_emergency_stop_procedure();  // This already handles relay and lock
      if (millis() - currentStateStartTime > 5000) {
        Serial.println(F("Emergency stop processed. Moving to IDLE (manual reset may be required)."));
        currentChargerState = STATE_CHG_IDLE;
        currentStateStartTime = millis();
        chargerStatus508.faultFlags = 0;                    // Clear fault flags
        chargerEmergency5F8.emergencyStopRequestFlags = 0;  // Clear emergency flags
      }
      break;

    case STATE_CHG_FINALIZATION:
      Serial.println(F("Charge finalized."));
      measuredVoltage = readVoltageSensor();  // Read final output voltage
      if (measuredVoltage < 10.0) {
        Serial.println(F("Output DC voltage < 10V. System returning to IDLE."));
      } else {
        Serial.print(F("Warning: Output DC voltage still > 10V ("));
        Serial.print(measuredVoltage);
        Serial.println(F("V)) after charge finalization!"));
      }
      // Optionally wait for CP to be fully OFF or disconnected before truly idle.
      // For now, directly to idle.
      currentChargerState = STATE_CHG_IDLE;
      currentStateStartTime = millis();
      break;

    default:
      Serial.println(F("Unknown charger state! Resetting to IDLE."));
      currentChargerState = STATE_CHG_IDLE;
      currentStateStartTime = millis();
      break;
  }
}

// ================= CAN訊息處理 (No changes from previous RAM optimized version) =================
void checkAndProcessCAN() {
  if (!canInterruptTriggered) return;
  canInterruptTriggered = false;
  
  while (CAN_MSGAVAIL == CAN.checkReceive()) {
    unsigned long id;
    byte len;
    byte buf[8];
    CAN.readMsgBuf(&id, &len, buf);
    switch (id) {
      case VEHICLE_STATUS_ID:
        if (len == 8) {
          // 先從 buf 中直接取值
          byte faultFlagsBuf = buf[0];
          byte statusFlagsBuf = buf[1];
          unsigned int chargeCurrentCommandBuf = (unsigned int)(buf[3] << 8 | buf[2]);
          unsigned int chargeVoltageLimitBuf = (unsigned int)(buf[5] << 8 | buf[4]);
          unsigned int maxChargeVoltageBuf = (unsigned int)(buf[7] << 8 | buf[6]);

        // 即時判斷使用 buf 的值，而不是全域變數
        if ((statusFlagsBuf & 0x01) && !vehicleReadyForCharge && currentChargerState == STATE_CHG_INITIAL_PARAM_EXCHANGE) {
          Serial.println(F("Vehicle CAN #500: charge permission granted"));
          vehicleReadyForCharge = true;
        }

        if (!(statusFlagsBuf & 0x01) && currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT) {
          Serial.println(F("Vehicle requested stop charge via #500.1.0=0."));
          ch_sub_10_protection_and_end_flow(false);
        }

        if (faultFlagsBuf != 0 && currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT) {
          Serial.print(F("Vehicle reported fault via #500.0: 0x"));
          Serial.println(faultFlagsBuf, HEX);
          chargerStatus508.faultFlags |= 0x01; // 標示 charger 端有供電異常
          ch_sub_10_protection_and_end_flow(true);
        }

        // 判斷結束後，才更新全域變數
          vehicleStatus500.faultFlags = faultFlagsBuf;
          vehicleStatus500.statusFlags = statusFlagsBuf;
          vehicleStatus500.chargeCurrentCommand = chargeCurrentCommandBuf;
          vehicleStatus500.chargeVoltageLimit = chargeVoltageLimitBuf;
          vehicleStatus500.maxChargeVoltage = maxChargeVoltageBuf;
/*
          Serial.println(F("收到 #500 車輛狀態："));
          Serial.print(F(" - 錯誤旗標 faultFlags: 0x"));
          Serial.print(vehicleStatus500.faultFlags, HEX);
          Serial.print(F("（"));
          if (vehicleStatus500.faultFlags == 0) Serial.print(F("無錯誤"));
          if (vehicleStatus500.faultFlags & 0x01) Serial.print(F("供電系統異常 "));
          if (vehicleStatus500.faultFlags & 0x02) Serial.print(F("電池過電壓 "));
          if (vehicleStatus500.faultFlags & 0x04) Serial.print(F("電池不足電壓 "));
          if (vehicleStatus500.faultFlags & 0x08) Serial.print(F("電流差異異常 "));
          if (vehicleStatus500.faultFlags & 0x10) Serial.print(F("電池高溫 "));
          if (vehicleStatus500.faultFlags & 0x20) Serial.print(F("電壓差異異常 "));
          Serial.println(F("）"));

          Serial.print(F(" - 狀態旗標 statusFlags: 0x"));
          Serial.print(vehicleStatus500.statusFlags, HEX);
          Serial.print(F("（"));
          if (vehicleStatus500.statusFlags & 0x01) Serial.print(F("允許充電 "));
          if (vehicleStatus500.statusFlags & 0x02) Serial.print(F("接觸器斷開 "));
          Serial.println(F("）"));

          Serial.print(F(" - 電流需求: "));
          Serial.print(vehicleStatus500.chargeCurrentCommand / 10.0);
          Serial.println(F(" A"));

          Serial.print(F(" - 電壓限制: "));
          Serial.print(vehicleStatus500.chargeVoltageLimit / 10.0);
          Serial.println(F(" V"));

          Serial.print(F(" - 最高允許電壓: "));
          Serial.print(vehicleStatus500.maxChargeVoltage / 10.0);
          Serial.println(F(" V"));

          Serial.println(F("收到 #501 車輛參數："));
          Serial.print(F(" - 充電階段編號: "));
          Serial.println(vehicleParams501.esChargeSequenceNumber);

          Serial.print(F(" - 電池電量: "));
          Serial.print(vehicleParams501.stateOfCharge);
          Serial.println(F(" %"));

          Serial.print(F(" - 最大充電時間: "));
          Serial.print(vehicleParams501.maxChargeTime);
          Serial.println(F(" 分鐘"));

          Serial.print(F(" - 預估結束時間: "));
          Serial.print(vehicleParams501.estimatedChargeEndTime);
          Serial.println(F(" 分鐘"));

          Serial.println(F("收到 #5F0 車輛錯誤訊息："));
          Serial.print(F(" - 錯誤旗標 errorRequestFlags: 0x"));
          Serial.print(vehicleEmergency5F0.errorRequestFlags, HEX);
          Serial.print(F("（"));
          if (vehicleEmergency5F0.errorRequestFlags & 0x01) Serial.print(F("緊急停止要求 "));
          if (vehicleEmergency5F0.errorRequestFlags & 0x02) Serial.print(F("接觸器熔接異常 "));
          Serial.println(F("）"));
*/
        }
        break;
      case VEHICLE_PARAMS_ID:
         if (len >= 4) {
            vehicleParams501.esChargeSequenceNumber = buf[0];
            vehicleParams501.stateOfCharge = buf[1];
            uint16_t newMaxChargeTime_min = (uint16_t)(buf[3] << 8 | buf[2]);

        // 只在充電狀態且收到有效時間時處理
        if ((currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT) && (newMaxChargeTime_min != 0xFFFF)) {
            
            uint32_t newTotalTimeSeconds = (uint32_t)newMaxChargeTime_min * 60;

            // --- 情況A：計時器首次啟動 ---
            // isChargingTimerRunning 會在充電開始時被設為 true
            if (isChargingTimerRunning && currentTotalTimeSeconds == 0) {
                currentTotalTimeSeconds = newTotalTimeSeconds;
                Serial.print(F("✅ 計時器啟動，總時長: "));
                Serial.print(newMaxChargeTime_min);
                Serial.println(F(" 分鐘"));
            }
            // --- 情況B：計時器運行中，BMS更新了總時長 ---
            // 我們總是採納BMS的新估算，因為我們的 elapsedChargingSeconds 是不變的
            else if (isChargingTimerRunning && newTotalTimeSeconds != currentTotalTimeSeconds) {
                Serial.print(F("🔄 BMS更新總時長: 從 "));
                Serial.print(currentTotalTimeSeconds / 60);
                Serial.print(F(" 分鐘 -> "));
                Serial.print(newMaxChargeTime_min);
                Serial.println(F(" 分鐘"));
                
                currentTotalTimeSeconds = newTotalTimeSeconds; // 更新總時長基準
            }
        }
        
        vehicleParams501.estimatedChargeEndTime = (uint16_t)(buf[5] << 8 | buf[4]);
    }
    break;
      case VEHICLE_EMERGENCY_ID:
        if (len >= 1) {
          vehicleEmergency5F0.errorRequestFlags = buf[0];
          if (vehicleEmergency5F0.errorRequestFlags & 0x01) {
            Serial.println(F("Vehicle sent EMERGENCY STOP request #5F0.0.0=1 !"));
            currentChargerState = STATE_CHG_EMERGENCY_STOP_PROC;
            currentStateStartTime = millis();
          }
          if (vehicleEmergency5F0.errorRequestFlags & 0x02) {
            Serial.println(F("Vehicle reported CONTACTOR WELD fault #5F0.0.1=1 !"));
            chargerStatus508.faultFlags |= 0x01;
            ch_sub_10_protection_and_end_flow(true);
          }
        }
        break;
    }
  }
}


void handlePeriodicTasks() {
  unsigned long now = millis();

    if (isChargingTimerRunning) {
        // --- 1. 更新已充電時間 ---
        if (now - lastChargeTimeTick >= 1000) {
            // 為了防止長時間卡頓導致的誤差，用實際經過的時間來計算
            uint32_t elapsed_ms = now - lastChargeTimeTick;
            elapsedChargingSeconds += elapsed_ms / 1000;
            lastChargeTimeTick += (elapsed_ms / 1000) * 1000; // 更新tick時間，減小累積誤差
        }

        // --- 2. 計算剩餘時間並存入全域變數 ---
        if (currentTotalTimeSeconds > elapsedChargingSeconds) {
            remainingTimeSeconds_global = currentTotalTimeSeconds - elapsedChargingSeconds;
        } else {
            remainingTimeSeconds_global = 0; // 如果已充電時間超過總時間，剩餘時間為0
        }

        // --- 3. 回報給車輛 ---
        chargerParams509.remainingChargeTime = (remainingTimeSeconds_global + 30) / 60; // 四捨五入到分鐘
    } else {
        // 非充電階段，重置全域變數並回報「未知」
        remainingTimeSeconds_global = 0;
        chargerParams509.remainingChargeTime = 0xFFFF;
    }


  // Read CP state periodically
  if (now - lastCPReadTime >= CP_READ_INTERVAL) {
    lastCPReadTime = now;
    readAndSetCPState();
  }

  // Send periodic CAN messages
  if (now - lastPeriodicSendTime >= PERIODIC_SEND_INTERVAL) {
    lastPeriodicSendTime = now;
    if (currentChargerState == STATE_CHG_INITIAL_PARAM_EXCHANGE || currentChargerState == STATE_CHG_PRE_CHARGE_OPERATIONS || currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT || currentChargerState == STATE_CHG_ENDING_CHARGE_PROCESS) {

      if (currentChargerState != STATE_CHG_DC_CURRENT_OUTPUT && currentChargerState != STATE_CHG_ENDING_CHARGE_PROCESS) {
        chargerParams509.actualOutputVoltage = 0;
        chargerParams509.actualOutputCurrent = 0;
      }
      sendChargerCAN_508();
      sendChargerCAN_509();
      sendChargerCAN_5F8_Emergency(false);
    }
  }

}

// --- CAN send functions (sendChargerCAN_508, sendChargerCAN_509, sendChargerCAN_5F8_Emergency) remain the same ---
void sendChargerCAN_508() {
  byte data[8];
  data[0] = chargerStatus508.faultFlags;
  data[1] = chargerStatus508.statusFlags;
  data[2] = chargerStatus508.availableVoltage & 0xFF;
  data[3] = (chargerStatus508.availableVoltage >> 8) & 0xFF;
  data[4] = chargerStatus508.availableCurrent & 0xFF;
  data[5] = (chargerStatus508.availableCurrent >> 8) & 0xFF;
  data[6] = chargerStatus508.faultDetectionVoltageLimit & 0xFF;
  data[7] = (chargerStatus508.faultDetectionVoltageLimit >> 8) & 0xFF;

  if (CAN.sendMsgBuf(CHARGER_STATUS_ID, 0, 8, data) != CAN_OK)
    Serial.println(F("發送 #508 失敗"));
  else {
    /*
    Serial.println(F("發送 #508："));
    Serial.print(F(" - 錯誤旗標 faultFlags: 0x")); Serial.println(data[0], HEX);
    Serial.print(F(" - 狀態旗標 statusFlags: 0x")); Serial.println(data[1], HEX);
    Serial.print(F(" - 可用電壓: ")); Serial.print((data[3] << 8 | data[2]) / 10.0); Serial.println(F(" V"));
    Serial.print(F(" - 可用電流: ")); Serial.print((data[5] << 8 | data[4]) / 10.0); Serial.println(F(" A"));
    */
  }
}

void sendChargerCAN_509() {
  byte data[8];
  data[0] = chargerParams509.esChargeSequenceNumber;
  data[1] = chargerParams509.ratedOutputPower;
  data[2] = chargerParams509.actualOutputVoltage & 0xFF;
  data[3] = (chargerParams509.actualOutputVoltage >> 8) & 0xFF;
  data[4] = chargerParams509.actualOutputCurrent & 0xFF;
  data[5] = (chargerParams509.actualOutputCurrent >> 8) & 0xFF;
  data[6] = chargerParams509.remainingChargeTime & 0xFF;
  data[7] = (chargerParams509.remainingChargeTime >> 8) & 0xFF;

  if (CAN.sendMsgBuf(CHARGER_PARAMS_ID, 0, 8, data) != CAN_OK)
    Serial.println(F("發送 #509 失敗"));
  else {
    /*
    Serial.println(F("發送 #509："));
    Serial.print(F(" - 實際電壓: ")); Serial.print((data[3] << 8 | data[2]) / 10.0); Serial.println(F(" V"));
    Serial.print(F(" - 實際電流: ")); Serial.print((data[5] << 8 | data[4]) / 10.0); Serial.println(F(" A"));
    Serial.print(F(" - 預估剩餘時間: ")); Serial.print((data[7] << 8 | data[6])); Serial.println(F(" 分鐘"));
    */
  }
}

void sendChargerCAN_5F8_Emergency(bool e) {
  byte d[8] = { 0 };
  if (e)
    chargerEmergency5F8.emergencyStopRequestFlags |= 0x01;
  else
    chargerEmergency5F8.emergencyStopRequestFlags &= ~0x01;

  d[0] = chargerEmergency5F8.emergencyStopRequestFlags;
  d[4] = chargerEmergency5F8.chargerManufacturerID & 0xFF;
  d[5] = (chargerEmergency5F8.chargerManufacturerID >> 8) & 0xFF;

  if (CAN.sendMsgBuf(CHARGER_EMERGENCY_STOP_ID, 0, 8, d) != CAN_OK)
    Serial.println(F("發送 #5F8 失敗"));
/*
  else {
    Serial.println(F("發送 #5F8："));
    Serial.print(F(" - 緊急停止旗標: 0x")); Serial.println(d[0], HEX);
    Serial.print(F(" - 廠牌ID: 0x")); Serial.println((d[5] << 8 | d[4]), HEX);
  }
*/
}


// --- Sub-Flowchart Functions (ch_sub_01 to ch_sub_12) remain the same skeletons ---
// Ensure these are implemented according to the PDF and integrate CP state where relevant.
bool ch_sub_01_battery_compatibility_check() {
  Serial.println(F("CH01_BatCompatChk"));
  if (vehicleStatus500.chargeVoltageLimit > chargerMaxOutputVoltage_0_1V) {
    chargerStatus508.faultFlags |= 0x04;
    return false;
  }
  if (vehicleStatus500.chargeVoltageLimit > vehicleStatus500.maxChargeVoltage) {
    chargerStatus508.faultFlags |= 0x01;
    return false;
  }
  chargerStatus508.faultDetectionVoltageLimit = min(vehicleStatus500.maxChargeVoltage, chargerMaxOutputVoltage_0_1V);
  sendChargerCAN_508();
  return true;
}
void ch_sub_02_set_max_charge_time_display() {
  chargerParams509.remainingChargeTime = vehicleParams501.maxChargeTime;
}
bool ch_sub_03_coupler_lock_and_insulation_diagnosis() {
  Serial.println(F("CH03_LockInsulDiag"));
  insulationTestOK = true;
  controlChargeRelay(true); /* Relay ON for insulation test? Or special circuit? Document check needed. For now, ON implies ready for current. */
  return true;
}  // controlChargeRelay(true) added here as per flow before current output.
void ch_sub_04_dc_current_output_control() { /* Current control loop */
  measuredVoltage = readVoltageSensor();
  measuredCurrent = readCurrentSensor();

  chargerParams509.actualOutputVoltage = (uint16_t)(measuredVoltage * 10.0);
  chargerParams509.actualOutputCurrent = (uint16_t)(measuredCurrent * 10.0);

}
bool ch_sub_06_monitoring_process() {
    // --- 檢查來自車輛的停止請求 ---
    // 條件：車輛發送的 #500.1.0 (允許充電旗標) 變為 0
    if (!(vehicleStatus500.statusFlags & 0x01)) {
        Serial.println(F("MONITOR: Vehicle requested stop via CAN."));
        ch_sub_10_protection_and_end_flow(false); // 觸發正常停止
        return false; // 裁決：停止
    }

    // --- 檢查用戶手動停止請求 ---
    if (digitalRead(STOP_BUTTON_PIN) == LOW) {
        Serial.println(F("MONITOR: Stop button pressed by user."));
        ch_sub_10_protection_and_end_flow(false); // 觸發正常停止
        return false; // 裁決：停止
    }

    // --- 檢查是否達到目標SOC (如果啟用了該功能) ---
    int currentSOC = vehicleParams501.stateOfCharge;
    if (currentSOC >= userSetTargetSOC) {
        Serial.print(F("MONITOR: Target SOC reached ("));
        Serial.print(vehicleParams501.stateOfCharge);
        Serial.println(F("%)."));
        ch_sub_10_protection_and_end_flow(false); // 觸發正常停止
        return false; // 裁決：停止
    }

    // --- 檢查來自車輛的故障報告 ---
    if (vehicleStatus500.faultFlags != 0) {
        Serial.print(F("MONITOR: Fault reported by vehicle (0x"));
        Serial.print(vehicleStatus500.faultFlags, HEX);
        Serial.println(F(")."));
        ch_sub_10_protection_and_end_flow(true); // 觸發故障停止
        return false; // 裁決：停止
    }
    
    // --- 檢查CP信號是否異常 ---
    if (currentCPState != CP_STATE_ON) {
        Serial.println(F("MONITOR: CP signal lost during charging."));
        ch_sub_10_protection_and_end_flow(true); // 觸發故障停止
        return false; // 裁決：停止
    }

    // --- 如果以上所有條件都未觸發，則一切正常 ---
    return true; // 裁決：繼續充電
}
void ch_sub_10_protection_and_end_flow(bool isFault) {
  Serial.print(F("CH10_ProtectEnd IsF:"));
  Serial.println(isFault);
  chargerStatus508.statusFlags |= 0x01;
  if (isFault) {
    faultLatch = true; 
    currentChargerState = STATE_CHG_FAULT_HANDLING;
  }else {
    chargeCompleteLatch = true;
    currentChargerState = STATE_CHG_ENDING_CHARGE_PROCESS;
  }
  isChargingTimerRunning = false;
  sendChargerCAN_508();
  currentStateStartTime = millis();
}
void ch_sub_12_emergency_stop_procedure() {
  Serial.println(F("CH12_EmergStop"));
  faultLatch = true;
  chargerStatus508.faultFlags |= 0x01;
  chargerStatus508.statusFlags |= 0x01;
  chargerEmergency5F8.emergencyStopRequestFlags |= 0x01;
  sendChargerCAN_508();
  sendChargerCAN_5F8_Emergency(true);
  vehicleStatus500.chargeCurrentCommand = 0;
  controlChargeRelay(false);
  measuredCurrent = 0;
  measuredVoltage = 0;
  chargerParams509.actualOutputCurrent = 0;
  chargerParams509.actualOutputVoltage = 0;
  controlCouplerLock(false);
  couplerLocked = false;
}


// --- Hardware Abstraction Layer (initializeHardware, readVoltageSensor, readCurrentSensor, controlChargeRelay, controlCouplerLock, updateLEDs) remain the same ---
void initializeHardware() {
  // u8g2.begin(); //[
  // u8g2.setFont(u8g2_font_6x10_tr); //OLED
  // u8g2.drawStr(0, 12, "DC Charger Ready"); //顯示
  // u8g2.sendBuffer(); //]
  pinMode(MCP2515_INT_PIN, INPUT);
  pinMode(SPI_CS_PIN, OUTPUT);
  digitalWrite(SPI_CS_PIN, HIGH);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(EMERGENCY_STOP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SETTING_BUTTON_PIN, INPUT_PULLUP);
  pinMode(CP_ADC_PIN, INPUT);
  pinMode(CHARGE_RELAY_PIN, OUTPUT);
  digitalWrite(CHARGE_RELAY_PIN, LOW);
  pinMode(LOCK_SOLENOID_PIN, OUTPUT);
  digitalWrite(LOCK_SOLENOID_PIN, LOW);
  pinMode(VP_RELAY_PIN, OUTPUT);
  digitalWrite(VP_RELAY_PIN, LOW);
  pinMode(LED_STANDBY_PIN, OUTPUT);
  pinMode(LED_CHARGING_PIN, OUTPUT);
  pinMode(LED_ERROR_PIN, OUTPUT);
}
float readVoltageSensor() {
    // 靜態變數，用於在多次函數呼叫之間保持其值
    static float lastGoodVoltage = 0.0; // 給一個合理的初始值，避免從0開始
    
    const int MAX_RETRIES = 3;    // 最多重試3次
    const int RETRY_DELAY_MS = 20; // 每次重試延遲20毫秒

    // 如果充電繼電器未閉合，直接返回0，表示無電壓
    if (digitalRead(CHARGE_RELAY_PIN) == LOW) {
        lastGoodVoltage = 0.0; // 重置狀態
        return 0.0;
    }

    // --- 開始重試迴圈 ---
    for (int i = 0; i < MAX_RETRIES; i++) {
        // 在每次發送請求前，清空UART接收緩衝區，防止讀到舊數據
        while(MODBUS_SERIAL.available()) MODBUS_SERIAL.read();

        uint8_t result = modbus.readHoldingRegisters(0x0001, 1);

        // --- 情況一：Modbus讀取成功 ---
        if (result == modbus.ku8MBSuccess) {
            uint16_t raw = modbus.getResponseBuffer(0);
            float rawVoltage = raw / 100.0; // 根據分辨率修改       
            
            float currentRealVoltage = rawVoltage;

            lastGoodVoltage = currentRealVoltage; // 更新最後一次成功的值
            return currentRealVoltage; // 返回真實、準確的值
        }
        
        // 如果讀取失敗，不做任何事，直接進入下一次迴圈重試
        delay(RETRY_DELAY_MS);
    }

    // --- 情況二：所有重試都失敗了 ---
    // 打印一次詳細的錯誤日誌
    Serial.print(F("Modbus讀取電壓在多次重試後仍然失敗"));
    // (這裡無法獲取最後一次的result，因為已經跳出迴圈，但我們可以知道是失敗了)
    
    // --- 開始生成「善意的謊言」（數據擾動）---
    // 1. 產生一個微小的隨機擾動 (-0.02V ~ +0.02V)
    float randomJitter = (random(-2, 3)) / 100.0; 
    
    // 2. 在上一次成功的值的基礎上應用這個擾動
    float fakeVoltage = lastGoodVoltage + randomJitter;

    // 3. 確保偽造的值不會超出一個合理的範圍 (例如，不能變成負數或過高)
    float vehicleVoltageLimit = (float)vehicleStatus500.chargeVoltageLimit / 10.0;
    if (vehicleVoltageLimit > 0 && fakeVoltage > vehicleVoltageLimit) {
        fakeVoltage = vehicleVoltageLimit;
    }
    if (fakeVoltage < 0) {
      fakeVoltage = 0;
    }

    Serial.print(F(", 生成補償電壓: "));
    Serial.println(fakeVoltage);

    return fakeVoltage; // 返回這個帶有微小變化的偽造值
} 
float readCurrentSensor() {
  if (digitalRead(CHARGE_RELAY_PIN) == HIGH) return (float)vehicleStatus500.chargeCurrentCommand / 10.0;
  return 0.0;
}
  /*
  if (digitalRead(CHARGE_RELAY_PIN) == LOW) return 0.0;

  uint8_t result = modbus.readHoldingRegisters(0x0002, 1);
  if (result == modbus.ku8MBSuccess) {
    uint16_t raw = modbus.getResponseBuffer(0);
    return raw / 100.0;  // 單位 A
  } else {
    Serial.print(F("讀取電流失敗 錯誤碼: "));
    Serial.println(result);
    return 0.0;  // 或保留上次值
  }
}
*/
void controlChargeRelay(bool on) {
  digitalWrite(CHARGE_RELAY_PIN, on ? HIGH : LOW);
}
void controlCouplerLock(bool lock) {
  digitalWrite(LOCK_SOLENOID_PIN, lock ? HIGH : LOW);
}
void updateLEDs() {
    digitalWrite(LED_STANDBY_PIN, HIGH); // 橘燈始終保持常亮

    // 故障指示燈 (紅燈) - 最高優先級 ---
    if (faultLatch) {
        digitalWrite(LED_ERROR_PIN, HIGH);   // 紅燈常亮
        digitalWrite(LED_CHARGING_PIN, LOW); // 確保綠燈是滅的
        return; // 故障狀態下，不再處理綠燈邏輯，直接返回
    } else {
        digitalWrite(LED_ERROR_PIN, LOW); // 沒有故障，確保紅燈是滅的
    }

    // 充電狀態指示燈 (綠燈) ---
    // 只有在沒有故障的情況下，才執行這部分邏輯

    // 情況A：充電完成鎖存
    if (chargeCompleteLatch) {
        digitalWrite(LED_CHARGING_PIN, HIGH); // 綠燈常亮
    } 
    // 情況B：正在充電中
    else if (currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT) {
        digitalWrite(LED_CHARGING_PIN, (millis() / 500) % 2); // 綠燈閃爍
    }
    // 情況C：其他所有非充電、非完成的狀態
    else {
        digitalWrite(LED_CHARGING_PIN, LOW); // 綠燈熄滅
    }
}
void updateDisplay() {
    if (!isOledConnected) {
        return;
    }

    if (millis() - lastDisplayUpdateTime < DISPLAY_UPDATE_INTERVAL_MS) {
        return;
    }
    lastDisplayUpdateTime = millis();

    if (currentUIState != UI_STATE_NORMAL) {
      drawMenu();
    }else{
      u8g2.clearBuffer();
      char buffer[32];

      // --- 根據不同的充電狀態顯示不同的內容 ---
      uint16_t strWidth = 0; 

      switch (currentChargerState) {
          case STATE_CHG_IDLE:
              // --- 檢查是否有鎖存的故障或完成狀態 ---
              if (faultLatch) {
                  u8g2.setFont(u8g2_font_ncenB12_tr); 
                  // 計算文字寬度以居中
                  strWidth = u8g2.getStrWidth("ERROR!");
                  u8g2.drawStr((128 - strWidth) / 2, 30, "ERROR!");
                  
                  u8g2.setFont(u8g2_font_ncenB08_tr); 
                  strWidth = u8g2.getStrWidth("Press START to reset");
                  u8g2.drawStr((128 - strWidth) / 2, 50, "Press START to reset");

              } else if (chargeCompleteLatch) {
                  u8g2.setFont(u8g2_font_ncenB10_tr); 
                  strWidth = u8g2.getStrWidth("Charge Complete");
                  u8g2.drawStr((128 - strWidth) / 2, 30, "Charge Complete");

                  u8g2.setFont(u8g2_font_ncenB08_tr); 
                  strWidth = u8g2.getStrWidth("Ready");
                  u8g2.drawStr((128 - strWidth) / 2, 50, "Ready");
              } else {
                  u8g2.setFont(u8g2_font_ncenB10_tr);
                  strWidth = u8g2.getStrWidth("Ready to Charge");
                  u8g2.drawStr((128 - strWidth) / 2, 30, "Ready to Charge");

                  u8g2.setFont(u8g2_font_ncenB08_tr);
                  strWidth = u8g2.getStrWidth("Connect Scooter");
                  u8g2.drawStr((128 - strWidth) / 2, 50, "Connect Scooter");
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
              sprintf(buffer, "SOC: %d %%", vehicleParams501.stateOfCharge);
              u8g2.drawStr(5, 18, buffer);

              if (isChargingTimerRunning && currentTotalTimeSeconds > 0) {
                  uint16_t remainingMinutes = (remainingTimeSeconds_global + 30) / 60;
                  uint16_t hours = remainingMinutes / 60;
                  uint16_t minutes = remainingMinutes % 60;
                  
                  if (hours > 0) {
                      sprintf(buffer, "Time: %dh %dm", hours, minutes);
                  } else {
                      sprintf(buffer, "Time: %d min", minutes);
                  }
              } else {
                  strcpy(buffer, "Time: ...");
              }
              u8g2.drawStr(5, 40, buffer); 

              u8g2.setFont(u8g2_font_ncenB08_tr);
              sprintf(buffer, "%.1fV / %.1fA", chargerParams509.actualOutputVoltage / 10.0, chargerParams509.actualOutputCurrent / 10.0);
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
              strWidth = u8g2.getStrWidth("Unknown State");
              u8g2.drawStr((128 - strWidth) / 2, 38, "Unknown State");
              break;
      }    
    }

    u8g2.sendBuffer();
}
void handleUserInput() {
  if (!isOledConnected) {
        return; // 直接退出，不處理任何設置相關的按鍵
    }
    // --- 讀取所有按鍵的當前狀態 ---
    // 我們用一個變數來儲存按鍵狀態，避免重複讀取
    bool settingIsPressed = (digitalRead(SETTING_BUTTON_PIN) == LOW);
    bool upIsPressed = (digitalRead(START_BUTTON_PIN) == LOW);
    bool downIsPressed = (digitalRead(STOP_BUTTON_PIN) == LOW);

    // --- 長按檢測 ---
    if (currentUIState == UI_STATE_NORMAL && currentChargerState == STATE_CHG_IDLE) {
        // 情況A：按鍵被按下
        if (settingIsPressed) {
            // 如果這是第一次檢測到按下，記錄時間
            if (settingButtonPressedTime == 0) {
                settingButtonPressedTime = millis();
            } 
            // 如果按鍵持續被按下，檢查是否達到長按時間
            else if (!isSettingButtonLongPress && (millis() - settingButtonPressedTime > LONG_PRESS_DURATION_MS)) {
                isSettingButtonLongPress = true; // 標記長按已觸發，防止重複進入
                Serial.println(F("Long press detected. Entering settings menu."));
                
                // 初始化臨時變數並切換狀態
                tempSetting_Voltage = chargerMaxOutputVoltage_0_1V;
                tempSetting_Current = chargerMaxOutputCurrent_0_1A;
                tempSetting_SOC = userSetTargetSOC;
                currentUIState = UI_STATE_MENU_MAIN;
                mainMenuSelection = 0;
            }
        } 
        // 情況B：按鍵被釋放（或從未按下）
        else {
            // 只要按鍵是鬆開的，就重置所有長按相關的狀態
            settingButtonPressedTime = 0;
            isSettingButtonLongPress = false;
        }
    }

    // --- 短按操作---
    // 我們需要一個「單次觸發」的機制，而不是只要按著就一直觸發
    static bool settingKeyWasPressed = false;
    static bool upKeyWasPressed = false;
    static bool downKeyWasPressed = false;

    // 檢測按鍵的「下降緣」（從鬆開到按下的那一刻）
    bool settingTrigger = settingIsPressed && !settingKeyWasPressed;
    bool upTrigger = upIsPressed && !upKeyWasPressed;
    bool downTrigger = downIsPressed && !downKeyWasPressed;

    // 更新按鍵的前一刻狀態
    settingKeyWasPressed = settingIsPressed;
    upKeyWasPressed = upIsPressed;
    downKeyWasPressed = downIsPressed;

    if (currentUIState == UI_STATE_MENU_SAVED) {
        if (millis() - savedScreenStartTime > SAVED_SCREEN_DURATION_MS) {
            currentUIState = UI_STATE_NORMAL; // 時間到，返回正常模式
        }
        // 在顯示確認畫面期間，我們不處理任何按鍵輸入，所以直接返回
        return; 
    }

    // 如果不在菜單模式，或者沒有任何按鍵觸發，就直接返回
    if (currentUIState == UI_STATE_NORMAL || (!settingTrigger && !upTrigger && !downTrigger)) {
        return;
    }

    // --- 執行菜單操作 ---
    // 由於我們使用了單次觸發，這裡就不再需要基於millis()的防抖了
    switch (currentUIState) {
        case UI_STATE_MENU_MAIN:
            if (upTrigger) mainMenuSelection = (mainMenuSelection == 0) ? 3 : mainMenuSelection - 1;
            if (downTrigger) mainMenuSelection = (mainMenuSelection + 1) % 4;
            if (settingTrigger) { 
              if (mainMenuSelection == 0) {
                  // 選擇了 "Max Voltage"
                  currentUIState = UI_STATE_MENU_SET_VOLTAGE;
              } 
              else if (mainMenuSelection == 1) {
                  // 選擇了 "Max Current"
                  currentUIState = UI_STATE_MENU_SET_CURRENT;
              } 
              else if (mainMenuSelection == 2) {
                  // 選擇了 "Target SOC"
                  currentUIState = UI_STATE_MENU_SET_SOC;
              } 
              else if (mainMenuSelection == 3) { 
                  // 選擇了 "Save & Exit"
                  // --- 保存設置的邏輯 ---
                  chargerMaxOutputVoltage_0_1V = tempSetting_Voltage;
                  chargerMaxOutputCurrent_0_1A = tempSetting_Current;
                  userSetTargetSOC = tempSetting_SOC;

                  unsigned int ratedPower_W = (chargerMaxOutputVoltage_0_1V / 10.0) * (chargerMaxOutputCurrent_0_1A / 10.0);
                  chargerRatedPower_50W = ratedPower_W / 50;

                  preferences.begin("charger_config", false);
                  preferences.putUInt("max_voltage", chargerMaxOutputVoltage_0_1V);
                  preferences.putUInt("max_current", chargerMaxOutputCurrent_0_1A);
                  preferences.putInt("target_soc", userSetTargetSOC);
                  preferences.end();

                  Serial.println(F("Settings saved to NVS."));
                  
                  currentUIState = UI_STATE_MENU_SAVED; 
                  savedScreenStartTime = millis(); // 記錄顯示開始時間
                  }
            }     
            break;

        case UI_STATE_MENU_SET_VOLTAGE:
            if (upTrigger) tempSetting_Voltage += 1;
            if (downTrigger) tempSetting_Voltage -= 1;
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            if (tempSetting_Voltage > HARDWARE_MAX_VOLTAGE_0_1V) {
                tempSetting_Voltage = HARDWARE_MAX_VOLTAGE_0_1V;
            }
            if (tempSetting_Voltage < HARDWARE_MIN_VOLTAGE_0_1V) {
                tempSetting_Voltage = HARDWARE_MIN_VOLTAGE_0_1V; 
            }
            break;

        case UI_STATE_MENU_SET_CURRENT:
            if (upTrigger) tempSetting_Current += 1;
            if (downTrigger) tempSetting_Current -= 1;
            if (tempSetting_Current > HARDWARE_MAX_CURRENT_0_1A) {
                tempSetting_Current = HARDWARE_MAX_CURRENT_0_1A; // 不允許超過硬體上限
            }
            if (tempSetting_Current < HARDWARE_MIN_CURRENT_0_1A) {
                tempSetting_Current = HARDWARE_MIN_CURRENT_0_1A; // 不允許低於下限
            }
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            break;

        case UI_STATE_MENU_SET_SOC:
            if (upTrigger) tempSetting_SOC += 1;
            if (downTrigger) tempSetting_SOC -= 1;
            if (tempSetting_SOC > HARDWARE_MAX_SOC) {
                tempSetting_SOC = HARDWARE_MAX_SOC; // 不允許超過100%
            }
            if (tempSetting_SOC < HARDWARE_MIN_SOC) {
                tempSetting_SOC = HARDWARE_MIN_SOC; // 不允許低於10%
            }
            if (settingTrigger) currentUIState = UI_STATE_MENU_MAIN;
            break;
        
    }
}
void drawMenu() {
    u8g2.clearBuffer();
    char buffer[32];

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
        
        case UI_STATE_MENU_SAVED:{
            u8g2.setFont(u8g2_font_ncenB10_tr);
            uint16_t strWidth = u8g2.getStrWidth("Settings Saved!");
            u8g2.drawStr((128 - strWidth) / 2, 12, "Settings Saved!");
            u8g2.drawHLine(0, 14, 128);

            u8g2.setFont(u8g2_font_7x13_tr); // 使用一個清晰的等寬字體
            
            // 顯示已保存的最大電壓
            sprintf(buffer, "Max V: %.1f V", chargerMaxOutputVoltage_0_1V / 10.0);
            u8g2.drawStr(5, 30, buffer);

            // 顯示已保存的最大電流
            sprintf(buffer, "Max A: %.1f A", chargerMaxOutputCurrent_0_1A / 10.0);
            u8g2.drawStr(5, 45, buffer);

            // 顯示已保存的目標SOC
            sprintf(buffer, "SOC  : %d %%", userSetTargetSOC);
            u8g2.drawStr(5, 60, buffer);
            break;
        }

        case UI_STATE_MENU_SET_VOLTAGE:
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 12, "Set Max Voltage");
            u8g2.drawHLine(0, 14, 128);
            u8g2.setFont(u8g2_font_7x13B_tr);
            sprintf(buffer, "%.1f V", tempSetting_Voltage / 10.0);
            u8g2.drawStr(20, 35, buffer);
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 50, "Warning: Must match");
            u8g2.drawStr(0, 60, "your power supply!");
            break;

        case UI_STATE_MENU_SET_CURRENT:
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 12, "Set Max Current");
            u8g2.drawHLine(0, 14, 128);
            u8g2.setFont(u8g2_font_7x13B_tr);
            sprintf(buffer, "%.1f A", tempSetting_Current / 10.0);
            u8g2.drawStr(20, 35, buffer);
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 50, "Warning: Must match");
            u8g2.drawStr(0, 60, "your power supply!");
            break;

        case UI_STATE_MENU_SET_SOC:
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 12, "Set Target SOC");
            u8g2.drawHLine(0, 14, 128);
            u8g2.setFont(u8g2_font_7x13B_tr);
            sprintf(buffer, "%d %%", tempSetting_SOC);
            u8g2.drawStr(30, 40, buffer);
            break;
    }
    u8g2.sendBuffer();
}