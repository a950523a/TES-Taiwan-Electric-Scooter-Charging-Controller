#include "BLE_Comms.h"
#include "ChargerLogic/ChargerLogic.h" // 需要與核心邏輯層交互
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- 專屬的UUID ---
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b" // 充電樁主服務

// --- 特徵UUID (每個數據點一個) ---
// 唯讀 & 通知 (ESP32 -> 手機)
#define CHAR_CHARGER_STATE_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8" // 充電狀態
#define CHAR_MEASURED_VOLTAGE_UUID "1c95d5e1-7b41-462c-b027-26a85a383a48" // 實時電壓
#define CHAR_MEASURED_CURRENT_UUID "2d42a58c-5a75-48c2-a85a-c50c5f1a50c3" // 實時電流
#define CHAR_SOC_UUID              "3f0e8b5a-4f28-4084-87b3-61a3d5a8d3f8" // 實時SOC
#define CHAR_REMAINING_TIME_UUID   "e2a4e5d6-3b24-4b7b-9a6d-7c8e5f6a7e8c" // 剩餘時間
#define CHAR_VEHICLE_REQ_CURRENT_UUID "f3b1a2c3-4d5e-6f7a-8b9c-0d1e2f3a4b5c" // 車輛請求電流

// 可讀 & 可寫 (手機 <-> ESP32)
#define CHAR_TARGET_SOC_UUID      "a1e8f5b1-51f0-4e0c-82a6-6a52de3a2a2a" // 設定目標SOC
#define CHAR_MAX_CURRENT_UUID     "b2d9c4a0-6e7f-8d9c-a1b2-c3d4e5f6a7b8" // 設定最大電流

// --- 全域的特徵物件指針 ---
static BLECharacteristic *pChargerStateCharacteristic;
static BLECharacteristic *pMeasuredVoltageCharacteristic;
// ... (為每個特徵都宣告一個指針) ...
static BLECharacteristic *pTargetSOCCharacteristic;
static BLECharacteristic *pMaxCurrentCharacteristic;

// --- 當手機APP寫入數據時的回調類 ---
class ConfigCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string uuid = pCharacteristic->getUUID().toString();
        std::string value = pCharacteristic->getValue();
        
        if (value.length() > 0) {
            // 獲取當前的設定，因為我們只修改其中一個值
            unsigned int current_v = logic_get_max_voltage_setting();
            unsigned int current_a = logic_get_max_current_setting();
            int current_soc = logic_get_target_soc_setting();

            // 根據UUID判斷是哪個設定被修改了
            if (uuid == BLEUUID(CHAR_TARGET_SOC_UUID).toString()) {
                int new_soc = atoi(value.c_str());
                Serial.print("BLE: Received new Target SOC: "); Serial.println(new_soc);
                logic_save_config(current_v, current_a, new_soc);
            } 
            else if (uuid == BLEUUID(CHAR_MAX_CURRENT_UUID).toString()) {
                float new_current_f = atof(value.c_str());
                unsigned int new_current_ui = (unsigned int)(new_current_f * 10.0);
                Serial.print("BLE: Received new Max Current: "); Serial.println(new_current_f);
                logic_save_config(current_v, new_current_ui, current_soc);
            }
        }
    }
};

class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("BLE Client Connected");
    }

    void onDisconnect(BLEServer* pServer) {
      Serial.println("BLE Client Disconnected. Restarting advertising...");
      // 在斷線後，重新開始廣播
      BLEDevice::startAdvertising();
    }
};

// --- 初始化函數 ---
void ble_comms_init() {
    BLEDevice::init("TES_Charger_ESP32"); // 您的藍牙設備名稱
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // --- 創建所有特徵 ---
    // 範例：充電狀態 (唯讀, 通知)
    pChargerStateCharacteristic = pService->createCharacteristic(
                                      CHAR_CHARGER_STATE_UUID,
                                      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                                  );
    pChargerStateCharacteristic->addDescriptor(new BLE2902()); // 啟用通知的標準做法

    // 範例：目標SOC (可讀, 可寫)
    pTargetSOCCharacteristic = pService->createCharacteristic(
                                   CHAR_TARGET_SOC_UUID,
                                   BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
                               );
    pTargetSOCCharacteristic->setCallbacks(new ConfigCallbacks()); // 綁定寫入回調


    pService->start();
    BLEDevice::startAdvertising();
    Serial.println("BLE Initialized and Advertising started.");
}

// --- 任務處理函數 ---
void ble_comms_handle_tasks() {
    static unsigned long lastBleUpdateTime = 0;
    if (millis() - lastBleUpdateTime > 1000) { // 每秒更新一次數據
        lastBleUpdateTime = millis();

        // 從核心邏輯層獲取所有需要顯示的數據
        ChargerState state = logic_get_charger_state();
        float voltage = logic_get_measured_voltage();
        // ... 獲取所有數據 ...
        int target_soc = logic_get_target_soc_setting();

        // 將數據轉換為字串並更新到特徵中
        char buffer[20];

        // 更新充電狀態
        sprintf(buffer, "%d", state);
        pChargerStateCharacteristic->setValue(buffer);
        pChargerStateCharacteristic->notify(); // 發送通知給已訂閱的手機

        // 更新電壓
        sprintf(buffer, "%.1f", voltage);
        // pMeasuredVoltageCharacteristic->setValue(buffer); // 假設您已創建它
        // pMeasuredVoltageCharacteristic->notify();

        // 更新可寫的特徵 (讓APP能讀到當前值)
        sprintf(buffer, "%d", target_soc);
        pTargetSOCCharacteristic->setValue(buffer);
        
        // ... 更新所有其他特徵 ...
    }
}