#include "NetworkServices.h"
#include "Config.h" 
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "ArduinoJson.h"
#include "ChargerLogic/ChargerLogic.h"
#include "ChargerLogic/ChargerLogic.h"
#include "CAN_Protocol/CAN_Protocol.h"

// --- 私有變數 ---
static AsyncWebServer server(80);
static DNSServer dnsServer;

enum WiFiState {
    WIFI_STATE_INIT,
    WIFI_STATE_DUAL_MODE_SEARCHING,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_AP_LOCKED,
    WIFI_STATE_AP_IDLE
};
static WiFiState wifiState = WIFI_STATE_INIT;
static unsigned long state_timer = 0;

// --- 函數原型 (私有函數) ---
static void startWebServer();
static void onWiFiEvent(WiFiEvent_t event);

// --- Web伺服器路由設定函數 ---
static void startWebServer() {
    server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument json_doc;
        // --- 從核心邏輯層獲取數據 ---
        uint32_t remaining_sec = logic_get_remaining_seconds();
        bool timer_running = logic_is_timer_running();
        uint32_t total_time_sec = logic_get_total_time_seconds();

        // --- 格式化時間字符串 ---
        char time_buffer[20]; // 緩衝區給大一點更安全
        if (timer_running && total_time_sec > 0) {
            uint16_t total_minutes = (remaining_sec + 30) / 60; // 四捨五入到分鐘
            uint16_t hours = total_minutes / 60;
            uint16_t minutes = total_minutes % 60;
            
            if (hours > 0) {
                sprintf(time_buffer, "%d h %d min", hours, minutes);
            } else {
                sprintf(time_buffer, "%d min", minutes);
            }
        } else {
            // 如果計時器沒在跑，就顯示一個橫槓
            strcpy(time_buffer, "--:--");
        }

        json_doc["charger_state"] = logic_get_charger_state();
        json_doc["soc"] = logic_get_soc();
        json_doc["voltage"] = logic_get_measured_voltage();
        json_doc["current"] = logic_get_measured_current();
        json_doc["target_soc"] = logic_get_target_soc_setting();
        json_doc["max_current"] = (float)logic_get_max_current_setting() / 10.0;
        json_doc["time_formatted"] = time_buffer;
        // ... 您可以添加任何您想顯示的數據 ...
        
        CAN_Vehicle_Status_500 v_status = can_protocol_get_vehicle_status();
        
        json_doc["flags"]["fault_flags"] = v_status.faultFlags;
        json_doc["flags"]["status_flags"] = v_status.statusFlags;
        
        JsonObject decoded = json_doc["flags"]["status_flags_decoded"].to<JsonObject>();
        decoded["charge_permission"] = (v_status.statusFlags & 0x01) ? true : false;
        decoded["contactor_open"] = (v_status.statusFlags & 0x02) ? true : false;

        String json_response;
        serializeJson(json_doc, json_response);

        // 發送JSON響應
        request->send(200, "application/json", json_response);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = R"rawliteral(
            <!DOCTYPE html>
            <html>
            <head>
                <title>TES Charger Control</title>
                <meta name="viewport" content="width=device-width, initial-scale=1">
                <style>
                    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 0; padding: 8px; background-color: #f5f5f5; }
                    .container { max-width: 800px; margin: auto; }
                    .card { background-color: white; box-shadow: 0 2px 4px 0 rgba(0,0,0,0.1); border-radius: 8px; padding: 16px; margin-bottom: 16px; }
                    h1, h2 { color: #333; }
                    .data-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px 20px; }
                    .data-item label { color: #666; }
                    .data-item .value { font-size: 1.8em; font-weight: bold; color: #000; }
                    .flags-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 5px 15px; font-family: monospace; font-size: 0.9em; }
                    .flag { display: flex; justify-content: space-between; }
                    .flag .name { color: #555; }
                    .flag .on { color: #4CAF50; font-weight: bold; }
                    .flag .off { color: #F44336; }
                    .button-group { display: flex; gap: 10px; margin-top: 16px; }
                    .btn { padding: 12px 24px; border: none; border-radius: 5px; font-size: 1em; cursor: pointer; }
                    .btn-start { background-color: #4CAF50; color: white; }
                    .btn-stop { background-color: #F44336; color: white; }
                    .btn-settings { background-color: #008CBA; color: white; }
                </style>
            </head>
            <body>
                <div class="container">
                    <h1>TES Charger Status</h1>

                    <div class="card">
                        <h2>Live Data</h2>
                        <div class="data-grid">
                            <div class="data-item">
                                <label>Voltage</label>
                                <div><span id="voltage" class="value">--</span> V</div>
                            </div>
                            <div class="data-item">
                                <label>Current</label>
                                <div><span id="current" class="value">--</span> A</div>
                            </div>
                            <div class="data-item">
                                <label>SOC</label>
                                <div><span id="soc" class="value">--</span> %</div>
                            </div>
                            <div class="data-item">
                                <label>Time Left</label>
                                <div><span id="time" class="value">--:--</span></div>
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h2>Controls & Settings</h2>
                        <div class="data-grid">
                            <div class="data-item">
                                <label>Target SOC</label>
                                <div><span id="target_soc">--</span> %</div>
                            </div>
                            <div class="data-item">
                                <label>Max Current</label>
                                <div><span id="max_current">--</span> A</div>
                            </div>
                        </div>
                        <div class="button-group">
                            <button class="btn btn-start" onclick="sendAction('/start_charge')">START</button>
                            <button class="btn btn-stop" onclick="sendAction('/stop_charge')">STOP</button>
                            <button class="btn btn-settings" onclick="goToSettings()">SETTINGS</button>
                        </div>
                    </div>

                    <div class="card">
                        <h2>CAN Bus Details</h2>
                        <div class="flags-grid">
                            <div class="flag">
                                <span class="name">Charge Permission:</span>
                                <span id="flag_perm" class="off">OFF</span>
                            </div>
                            <div class="flag">
                                <span class="name">Contactor Status:</span>
                                <span id="flag_contactor" class="off">CLOSED</span>
                            </div>
                            <!-- 在這裡添加更多旗標的顯示元素 -->
                        </div>
                    </div>
                </div>

                <script>
                    function sendAction(path) {
                        var xhttp = new XMLHttpRequest();
                        xhttp.open("POST", path, true);
                        xhttp.send();
                    }

                    function goToSettings() {
                        // 暫時先用一個alert，未來可以跳轉到一個新的設定頁面
                        alert("Settings page is not implemented yet.");
                        // window.location.href = "/settings.html";
                    }

                    function updateStatus() {
                        var xhttp = new XMLHttpRequest();
                        xhttp.onreadystatechange = function() {
                            if (this.readyState == 4 && this.status == 200) {
                                var data = JSON.parse(this.responseText);
                                
                                // 更新 Live Data
                                document.getElementById("voltage").innerHTML = data.voltage.toFixed(1);
                                document.getElementById("current").innerHTML = data.current.toFixed(1);
                                document.getElementById("soc").innerHTML = data.soc;
                                document.getElementById("time").innerHTML = data.time_formatted;

                                // 更新 Settings
                                document.getElementById("target_soc").innerHTML = data.target_soc;
                                document.getElementById("max_current").innerHTML = data.max_current.toFixed(1);

                                // 更新 CAN Flags
                                var permFlag = document.getElementById("flag_perm");
                                if (data.flags.status_flags_decoded.charge_permission) {
                                    permFlag.textContent = "ON";
                                    permFlag.className = "on";
                                } else {
                                    permFlag.textContent = "OFF";
                                    permFlag.className = "off";
                                }

                                var contactorFlag = document.getElementById("flag_contactor");
                                if (data.flags.status_flags_decoded.contactor_open) {
                                    contactorFlag.textContent = "OPEN";
                                    contactorFlag.className = "on";
                                } else {
                                    contactorFlag.textContent = "CLOSED";
                                    contactorFlag.className = "off";
                                }
                            }
                        };
                        xhttp.open("GET", "/status.json", true);
                        xhttp.send();
                    }

                    window.onload = function() { updateStatus(); };
                    setInterval(updateStatus, 2000);
                </script>
            </body>
            </html>
            )rawliteral";
            request->send(200, "text/html", html);
    });

    server.on("/start_charge", HTTP_POST, [](AsyncWebServerRequest *request){
        logic_start_button_pressed(); 
        request->send(200, "text/plain", "OK");
    });

    server.on("/stop_charge", HTTP_POST, [](AsyncWebServerRequest *request){
        logic_stop_button_pressed(); 
        request->send(200, "text/plain", "OK");
    });
    
    server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request){
        // ...
        // CAN_Vehicle_Status_500 v_status = can_protocol_get_vehicle_status(); // ✅ 呼叫CAN層的接口
        // ...
    });

    server.on("/wifi_setup", HTTP_GET, [](AsyncWebServerRequest *request){
        String form = "<h2>WiFi Setup</h2><form action='/save_wifi' method='POST'>";
        form += "SSID: <input type='text' name='ssid'><br>";
        form += "Pass: <input type='password' name='pass'><br>";
        form += "<input type='submit' value='Save'></form>";
        request->send(200, "text/html", form);
    });

    server.on("/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid = request->arg("ssid");
        String pass = request->arg("pass");
        
        Preferences prefs; 
        prefs.begin("wifi_config", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();

        request->send(200, "text/html", "<h1>Settings Saved!</h1><p>Device will restart...</p>");
        delay(2000);
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    server.begin();
    Serial.println("Web Server started.");
}

// --- Wi-Fi事件處理函數 ---
static void onWiFiEvent(WiFiEvent_t event) {
    switch(event) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("WiFi AP: Client connected");
            if (wifiState == WIFI_STATE_AP_IDLE) {
                wifiState = WIFI_STATE_AP_LOCKED;
            }
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("WiFi AP: Client disconnected");
            if (wifiState == WIFI_STATE_AP_LOCKED && WiFi.softAPgetStationNum() == 0) {
                wifiState = WIFI_STATE_AP_IDLE;
            }
            break;
        default:
            break;
    }
}

// --- 初始化函數 ---
void net_init() {
    WiFi.onEvent(onWiFiEvent); // 註冊事件回調

    // 初始化時，直接進入 INIT 狀態，讓狀態機來決定下一步
    wifiState = WIFI_STATE_INIT; 
    state_timer = millis(); // 啟動計時器

    Serial.println("Network Services: Initialized. State machine will take over.");
}

// --- 任務處理函數 ---
void net_handle_tasks() {
    dnsServer.processNextRequest(); // 處理Captive Portal

    switch (wifiState) {
        case WIFI_STATE_INIT: {
            // --- 步驟1：嘗試從NVS讀取用戶配置 ---
            Preferences prefs;
            prefs.begin("wifi_config", true); // true = 只讀
            String ssid = prefs.getString("ssid", "");
            String pass = prefs.getString("pass", "");
            prefs.end();

            // --- 步驟2：做出決策 ---
            if (ssid.length() > 0) {
                // 如果NVS中有已保存的配置，則進入STA連接模式
                Serial.print("WiFi: Found saved config. Trying to connect to '");
                Serial.print(ssid);
                Serial.println("'...");
                
                WiFi.mode(WIFI_STA);
                WiFi.begin(ssid.c_str(), pass.c_str());
                
                wifiState = WIFI_STATE_STA_CONNECTING;
                state_timer = millis(); // 開始連接超時計時
            } else {
                // 如果NVS中沒有配置，則直接進入AP模式
                Serial.println("WiFi: No saved config found. Starting AP mode.");
                WiFi.mode(WIFI_AP);
                WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD); // ❗ 使用Config.h中的預設值
                
                startWebServer(); // 立刻啟動Web服務
                dnsServer.start(53, "*", WiFi.softAPIP()); // 啟動DNS
                wifiState = WIFI_STATE_AP_IDLE;
            }
            break;
        }

        case WIFI_STATE_STA_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                // 連接成功！
                Serial.print("WiFi: STA connected! IP: ");
                Serial.println(WiFi.localIP());
                wifiState = WIFI_STATE_STA_CONNECTED;
                startWebServer(); // 啟動Web服務
            } else if (millis() - state_timer > 20000) { // 20秒連接超時
                // 連接失敗！
                Serial.println("WiFi: STA connection failed. Switching to AP mode for configuration.");
                WiFi.disconnect(); // 停止嘗試STA連接
                WiFi.mode(WIFI_AP);
                WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD); // ❗ 使用Config.h中的預設值
                
                startWebServer();
                dnsServer.start(53, "*", WiFi.softAPIP());
                wifiState = WIFI_STATE_AP_IDLE;
            }
            break;

        case WIFI_STATE_STA_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi: STA connection lost. Re-initializing...");
                wifiState = WIFI_STATE_INIT; // 回到初始狀態，重新決策
            }
            // 在這裡可以加入長按按鈕強制進入AP模式的邏輯
            // if (force_ap_mode_triggered) { wifiState = WIFI_STATE_INIT; /* 擦除NVS配置 */ }
            break;

        case WIFI_STATE_AP_IDLE:
        case WIFI_STATE_AP_LOCKED:
            // 在AP模式下，Web伺服器和DNS伺服器會持續運行
            // 等待用戶通過網頁提交新的Wi-Fi配置
            // onWiFiEvent 會自動處理 AP_IDLE 和 AP_LOCKED 之間的切換
            break;
    }
}