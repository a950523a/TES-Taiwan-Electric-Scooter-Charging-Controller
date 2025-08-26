#ifndef CONFIG_H
#define CONFIG_H

// =================================================================
// =                 USER CONFIGURATION SECTION                    =
// =          請在此處修改配置以適應您的硬體和偏好            =
// =================================================================


// --- 硬體引腳定義 (Hardware Pin Definitions) ---
// 請根據您的實際接線修改以下GPIO編號

// 按鈕 (Buttons)
#define START_BUTTON_PIN      32
#define STOP_BUTTON_PIN       25
#define EMERGENCY_BUTTON_PIN  16
#define SETTING_BUTTON_PIN    33

// LED 指示燈
#define LED_STANDBY_PIN       14  // 橘燈
#define LED_CHARGING_PIN      12  // 綠燈
#define LED_ERROR_PIN         13  // 紅燈

// 繼電器與輸出 (Relays & Outputs)
#define CHARGE_RELAY_PIN      26  // 主充電繼電器
#define LOCK_SOLENOID_PIN     27  // 電磁鎖 (可選)
#define VP_RELAY_PIN          15  // VP 繼電器 

// CAN Bus (TJA1051T/3)
#define TWAI_TX_PIN           GPIO_NUM_5
#define TWAI_RX_PIN           GPIO_NUM_4

// I2C 總線 (OLED & ADS1115)
// 通常是固定的，除非您有特殊需求
// #define I2C_SDA_PIN           21
// #define I2C_SCL_PIN           22

// --- 分壓電阻定義 ---
const float VOLTAGE_DIVIDER_120V_R1 = 348.0; // 標稱348kΩ 需自行校準
const float VOLTAGE_DIVIDER_120V_R2 = 11.731;  // 標稱12kΩ 需自行校準
const float VOLTAGE_DIVIDER_120V_RATIO = VOLTAGE_DIVIDER_120V_R2 / (VOLTAGE_DIVIDER_120V_R1 + VOLTAGE_DIVIDER_120V_R2);

const float VOLTAGE_DIVIDER_CP_R1 = 150.0; // Ω
const float VOLTAGE_DIVIDER_CP_R2 = 51.0;  // Ω
const float VOLTAGE_DIVIDER_CP_RATIO = VOLTAGE_DIVIDER_CP_R2 / (VOLTAGE_DIVIDER_CP_R1 + VOLTAGE_DIVIDER_CP_R2);

// --- CP 參數 ---
#define CP_HYSTERESIS 0.3       // 防抖電壓寬容值 (V)
#define CP_ERROR_THRESHOLD 2    // 連續錯誤次數才判斷為錯誤
#define CP_SAMPLE_COUNT 5       // 平均取樣次數

// --- CAN ID 定義 ---
#define CHARGER_STATUS_ID 0x508
#define CHARGER_PARAMS_ID 0x509
#define CHARGER_EMERGENCY_STOP_ID 0x5F8

#define VEHICLE_STATUS_ID 0x500
#define VEHICLE_PARAMS_ID 0x501
#define VEHICLE_EMERGENCY_ID 0x5F0

// --- 計時器與超時控制 ---
const unsigned long PERIODIC_SEND_INTERVAL = 100;  // ms
const unsigned long CP_READ_INTERVAL = 50;         // ms
const unsigned long DISPLAY_UPDATE_INTERVAL_MS = 250; // ms
const unsigned long LONG_PRESS_DURATION_MS = 1000; // ms
const unsigned long SAVED_SCREEN_DURATION_MS = 2000; // ms

// --- 物理極限與安全設定 (Physical & Safety Limits) ---
// 這些值應該根據您的電源供應器和硬體能力設定
const unsigned int HARDWARE_MAX_VOLTAGE_0_1V = 1200; // 120.0V
const unsigned int HARDWARE_MAX_CURRENT_0_1A = 1000; // 100.0A (假設硬體最大支持100A)
const unsigned int HARDWARE_MIN_VOLTAGE_0_1V = 100;  // 10.0V (邏輯上的最小值)
const unsigned int HARDWARE_MIN_CURRENT_0_1A = 10;   // 1.0A (邏輯上的最小值)
const int HARDWARE_MAX_SOC = 100; // 100%
const int HARDWARE_MIN_SOC = 10;  // 10% (邏輯上的最小值)

// --- 充電參數 ---
const unsigned int chargerManufacturerCode = 0x0000;

//WiFi
#define WIFI_AP_SSID "TES_Charger_ESP32"
#define WIFI_AP_PASSWORD "12345678"

// --- 功能開關 (Feature Toggles) ---

// 是否啟用OLED顯示和設定選單功能
// 如果您沒有連接OLED，可以將下面這一行註解掉，以節省程式碼空間
#define ENABLE_OLED_SUPPORT

// --- ?????? ---
const int LUX_BEACON_TIME_UNIT_MS = 150;
#define OTA_DEVELOPER_MODE
//#define DEVELOPER_MODE 
#ifdef DEVELOPER_MODE
    #define CHARGER_RELAY_SIM_LED_PIN   2
#endif

// =================================================================
// =                END OF USER CONFIGURATION SECTION              =
// =================================================================

#endif // CONFIG_H