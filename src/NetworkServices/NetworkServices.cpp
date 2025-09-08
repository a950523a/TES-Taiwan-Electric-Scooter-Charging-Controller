// src/NetworkServices/NetworkServices.cpp

#include "NetworkServices.h"
#include "Config.h" 
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "ArduinoJson.h"
#include <LittleFS.h>
#include <Update.h>
#include "OTAManager/OTAManager.h"

// --- 私有變數 ---
static AsyncWebServer server(80);
static DNSServer dnsServer;
static DisplayData network_display_data; // 儲存最新數據的副本
static bool should_reboot = false;

// --- [新增] 引用外部的 OTA 觸發函數 ---
extern void ota_start_check();
extern void ota_start_update();
extern void logic_save_web_settings(unsigned int current, int soc);

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

extern void check_filesystem_version();

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
        json_doc["max_voltage"] = network_display_data.maxVoltageSetting_0_1V;
        json_doc["max_current"] = (float)network_display_data.maxCurrentSetting_0_1A / 10.0;
        json_doc["time_formatted"] = time_buffer;
        json_doc["ip_address"] = network_display_data.ipAddress;

        // --- [新增] 填充 OTA 數據 ---
        json_doc["current_fw_version"] = network_display_data.currentFirmwareVersion;
        json_doc["latest_fw_version"] = network_display_data.latestFirmwareVersion;
        json_doc["update_available"] = network_display_data.updateAvailable;
        json_doc["ota_progress"] = network_display_data.otaProgress;
        json_doc["ota_status_message"] = network_display_data.otaStatusMessage;
        json_doc["filesystem_version"] = network_display_data.filesystemVersion;
        json_doc["wifi_mode"] = network_display_data.wifiMode;
        json_doc["wifi_ssid"] = network_display_data.wifiSSID;
        
        String json_response;
        serializeJson(json_doc, json_response);
        request->send(200, "application/json", json_response);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/index.html", "text/html");
    });

    server.on("/save_settings", HTTP_POST, [](AsyncWebServerRequest *request){
        unsigned int current = 0;
        int soc = 0;

        if (request->hasParam("max_current", true)) {
            current = request->getParam("max_current", true)->value().toFloat() * 10;
        }
        if (request->hasParam("target_soc", true)) {
            soc = request->getParam("target_soc", true)->value().toInt();
        }

        // 呼叫 logic 層的函式來儲存設定
        logic_save_web_settings(current, soc);

        request->send(200, "text/plain", "OK");
    });

    server.on("/wifi_setup.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/wifi_setup.html", "text/html");
    });

        server.on("/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid = "";
        String pass = "";
        if (request->hasParam("ssid", true)) {
            ssid = request->getParam("ssid", true)->value();
        }
        if (request->hasParam("pass", true)) {
            pass = request->getParam("pass", true)->value();
        }

        Serial.println("NET: Saving new WiFi credentials...");
        Serial.print("SSID: "); Serial.println(ssid);
        Serial.print("Password: "); Serial.println(pass);

        Preferences prefs;
        prefs.begin("wifi_config", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();

        String response_html = R"rawliteral(
            <!DOCTYPE html>
            <html>
            <head><title>Rebooting...</title></head>
            <body>
                <h1>Settings Saved!</h1>
                <p>The device will now reboot and try to connect to the new WiFi network.</p>
                <p>Please reconnect your phone/computer to your normal WiFi network and find the device's new IP address.</p>
            </body>
            </html>
        )rawliteral";
        request->send(200, "text/html", response_html);
        
        delay(1000);
        ESP.restart();
    });

    server.on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        net_reset_wifi_credentials();
        request->send(200, "text/plain", "OK");
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
        ota_start_full_update(); 
        request->send(200, "text/plain", "OK"); 
    });

    server.on("/update_fw", HTTP_POST, [](AsyncWebServerRequest *request){
        // 上傳完成後，請求重啟
        should_reboot = true;
        request->send(200, "text/plain", "OK");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            Serial.printf("Manual FW Update Start: %s\n", filename.c_str());
            // 明確指定更新韌體 (U_FLASH)
            if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)){
                Update.printError(Serial);
            }
        }
        if(len){
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Manual FW Update Success: %uB\n", index+len);
            } else {
                Update.printError(Serial);
            }
        }
    });

    // --- [新增] 手動檔案系統上傳路由 ---
    server.on("/update_fs", HTTP_POST, [](AsyncWebServerRequest *request){
        // 上傳完成後，請求重啟
        should_reboot = true;
        request->send(200, "text/plain", "OK");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            Serial.printf("Manual FS Update Start: %s\n", filename.c_str());
            // 明確指定更新檔案系統 (U_SPIFFS)
            if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)){
                Update.printError(Serial);
            }
        }
        if(len){
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Manual FS Update Success: %uB\n", index+len);
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
void net_handle_tasks(DisplayData& data) {
    if (WiFi.status() == WL_CONNECTED) {
        data.wifiMode = "STA (Client)";
        strncpy(data.wifiSSID, WiFi.SSID().c_str(), 32);
        strncpy(data.ipAddress, WiFi.localIP().toString().c_str(), 15);
    } else if (WiFi.getMode() == WIFI_AP) {
        data.wifiMode = "AP (Hotspot)";
        dnsServer.processNextRequest();
        strncpy(data.wifiSSID, WIFI_AP_SSID, 32);
        strncpy(data.ipAddress, WiFi.softAPIP().toString().c_str(), 15);
    } else {
        data.wifiMode = "Connecting...";
        strncpy(data.wifiSSID, "", 32);
        strncpy(data.ipAddress, "N/A", 15);
    }
    data.wifiSSID[32] = '\0';
    data.ipAddress[15] = '\0';

    memcpy(&network_display_data, &data, sizeof(DisplayData));

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
                LittleFS.begin();
                check_filesystem_version();
                WiFi.begin(ssid.c_str(), pass.c_str());
                wifiState = WIFI_STATE_STA_CONNECTING;
                state_timer = millis();
            } else {
                Serial.println("WiFi: Starting AP mode.");
                WiFi.mode(WIFI_AP);
                LittleFS.begin();
                check_filesystem_version();
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
                dnsServer.stop();
                startWebServer();
            } else if (millis() - state_timer > 20000) {
                Serial.println("WiFi: STA connection failed. Switching to AP mode.");
                WiFi.disconnect();
                server.end();
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
    if (should_reboot) {
        Serial.println("Reboot requested by manual update. Rebooting in 1 second...");
        should_reboot = false; // 清除旗標
        delay(1000); // 等待 Web Server 回應完成
        ESP.restart();
    }
}

void net_reset_wifi_credentials() {
    Serial.println("NET: Resetting WiFi credentials...");
    Preferences prefs;
    prefs.begin("wifi_config", false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
    Serial.println("NET: WiFi credentials cleared. Rebooting...");
    delay(1000);
    ESP.restart();
}