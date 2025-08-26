// src/NetworkServices/NetworkServices.cpp

#include "NetworkServices.h"
#include "Config.h" 
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "ArduinoJson.h"
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include "OTAManager/OTAManager.h"

// --- 私有變數 ---
static AsyncWebServer server(80);
static DNSServer dnsServer;
static DisplayData network_display_data; // 儲存最新數據的副本

// --- [新增] 引用外部的 OTA 觸發函數 ---
extern void ota_start_check();
extern void ota_start_update();

enum WiFiState {
    WIFI_STATE_INIT,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_AP_IDLE
};
static WiFiState wifiState = WIFI_STATE_INIT;
static unsigned long state_timer = 0;

// --- 函數原型 (私有函數) ---
static void startWebServer();
static void onWiFiEvent(WiFiEvent_t event);

// --- 引用外部的 logic 層函數來觸發動作 ---
extern void logic_start_button_pressed();
extern void logic_stop_button_pressed();

// --- Web伺服器路由設定函數 ---
static void startWebServer() {
    server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument json_doc;
        
        char time_buffer[20];
        if (network_display_data.isTimerRunning && network_display_data.totalTimeSeconds > 0) {
            uint16_t total_minutes = (network_display_data.remainingSeconds + 30) / 60;
            uint16_t hours = total_minutes / 60;
            uint16_t minutes = total_minutes % 60;
            if (hours > 0) {
                sprintf(time_buffer, "%d h %d min", hours, minutes);
            } else {
                sprintf(time_buffer, "%d min", minutes);
            }
        } else {
            strcpy(time_buffer, "--:--");
        }

        json_doc["charger_state"] = (int)network_display_data.chargerState;
        json_doc["soc"] = network_display_data.soc;
        json_doc["voltage"] = network_display_data.measuredVoltage;
        json_doc["current"] = network_display_data.measuredCurrent;
        json_doc["target_soc"] = network_display_data.targetSOC;
        json_doc["max_current"] = (float)network_display_data.maxCurrentSetting_0_1A / 10.0;
        json_doc["time_formatted"] = time_buffer;

        // --- [新增] 填充 OTA 數據 ---
        json_doc["current_fw_version"] = network_display_data.currentFirmwareVersion;
        json_doc["latest_fw_version"] = network_display_data.latestFirmwareVersion;
        json_doc["update_available"] = network_display_data.updateAvailable;
        json_doc["ota_progress"] = network_display_data.otaProgress;
        json_doc["ota_status_message"] = network_display_data.otaStatusMessage;
        json_doc["ip_address"] = network_display_data.ipAddress;
        
        String json_response;
        serializeJson(json_doc, json_response);
        request->send(200, "application/json", json_response);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/index.html", "text/html");
    });

    server.on("/start_charge", HTTP_POST, [](AsyncWebServerRequest *request){
        logic_start_button_pressed(); 
        request->send(200, "text/plain", "OK");
    });

    server.on("/stop_charge", HTTP_POST, [](AsyncWebServerRequest *request){
        logic_stop_button_pressed(); 
        request->send(200, "text/plain", "OK");
    });

    server.on("/ota_check", HTTP_POST, [](AsyncWebServerRequest *request){
        ota_start_check();
        request->send(200, "text/plain", "OK");
    });

    server.on("/ota_start", HTTP_POST, [](AsyncWebServerRequest *request){
        ota_start_update();
        request->send(200, "text/plain", "OK");
    });

    // --- [新增] 手動韌體上傳路由 ---
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        // 只有在沒有其他 OTA 任務時才允許上傳
        bool shouldReboot = (ota_get_status() == OTA_IDLE);
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
        response->addHeader("Connection", "close");
        request->send(response);
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            Serial.printf("Update Start: %s\n", filename.c_str());
            // 檢查是否有足夠的空間
            if(!Update.begin(UPDATE_SIZE_UNKNOWN)){
                Update.printError(Serial);
            }
        }
        // 寫入數據
        if(len){
            Update.write(data, len);
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Update Success: %uB\n", index+len);
            } else {
                Update.printError(Serial);
            }
        }
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    server.begin();
    Serial.println("Web Server started.");
}

// --- Wi-Fi事件處理函數 ---
static void onWiFiEvent(WiFiEvent_t event) {
}

// --- 初始化函數 ---
void net_init() {
    if(!LittleFS.begin(true)){ // true = format if mount failed
        Serial.println("An Error has occurred while mounting LittleFS");
        return;
    }
    WiFi.onEvent(onWiFiEvent);
    wifiState = WIFI_STATE_INIT; 
    state_timer = millis();
    Serial.println("Network Services: Initialized.");
}

// --- 任務處理函數 ---
void net_handle_tasks(const DisplayData& data) {
    memcpy(&network_display_data, &data, sizeof(DisplayData));
    dnsServer.processNextRequest();

    switch (wifiState) {
        case WIFI_STATE_INIT: {
            Preferences prefs;
            prefs.begin("wifi_config", true);
            String ssid = prefs.getString("ssid", "");
            String pass = prefs.getString("pass", "");
            prefs.end();
            if (ssid.length() > 0) {
                Serial.print("WiFi: Trying to connect to '");
                Serial.print(ssid);
                Serial.println("'...");
                WiFi.mode(WIFI_STA);
                WiFi.begin(ssid.c_str(), pass.c_str());
                wifiState = WIFI_STATE_STA_CONNECTING;
                state_timer = millis();
            } else {
                Serial.println("WiFi: Starting AP mode.");
                WiFi.mode(WIFI_AP);
                WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
                startWebServer();
                dnsServer.start(53, "*", WiFi.softAPIP());
                wifiState = WIFI_STATE_AP_IDLE;
            }
            break;
        }
        case WIFI_STATE_STA_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                Serial.print("WiFi: STA connected! IP: ");
                Serial.println(WiFi.localIP());
                wifiState = WIFI_STATE_STA_CONNECTED;
                startWebServer();
            } else if (millis() - state_timer > 20000) {
                Serial.println("WiFi: STA connection failed. Switching to AP mode.");
                WiFi.disconnect();
                wifiState = WIFI_STATE_INIT; // 回到 INIT 重新決策
            }
            break;
        case WIFI_STATE_STA_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi: STA connection lost. Re-initializing...");
                wifiState = WIFI_STATE_INIT;
            }
            break;
        case WIFI_STATE_AP_IDLE:
            break;
    }
}