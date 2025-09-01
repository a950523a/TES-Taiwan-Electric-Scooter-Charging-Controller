// src/OTAManager/OTAManager.cpp

#include "OTAManager.h"
#include <ArduinoJson.h> // --- [新增] 引入 ArduinoJson 函式庫 ---
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include "Version.h"
#include <Update.h>

// --- [修改] 啟用開發者模式以測試 Beta 版 ---
#define OTA_DEVELOPER_MODE 

#define GITHUB_USER "a950523a"
#define GITHUB_REPO "TES-Taiwan-Electric-Scooter-Charging-Controller"

// --- [修改] RTC 記憶體標記，用於多步更新 ---
RTC_DATA_ATTR int ota_step = 0; // 0=Idle, 1=Need FS Update, 2=Need FW Update, 3=Need Both
RTC_DATA_ATTR char latest_version_tag[32] = {0}; // 儲存最新版本號標籤

// --- 私有變數 ---
static OTAStatus currentStatus = OTA_IDLE;
static String statusMessage = "Idle";
static bool check_requested = false;
static bool full_update_requested = false;
static int downloadProgress = 0;

// --- 私有函數原型 ---
static void perform_check();
static void perform_firmware_update();
static void perform_filesystem_update();

void ota_init() {
    Serial.println("OTA Manager Initialized.");
    if (ota_step != 0) {
        Serial.println("OTA: Detected pending update after reboot.");
        delay(3000); 
        ota_start_full_update(); 
    }
}

void ota_handle_tasks() {
    if (check_requested) {
        check_requested = false;
        perform_check();
    }
    if (full_update_requested) {
        full_update_requested = false;
        if (ota_step == 1 || ota_step == 3) { // 如果需要更新 FS (單獨或兩者都需要)
            perform_filesystem_update();
        } else if (ota_step == 2) { // 如果只需要更新 FW
            perform_firmware_update();
        }
    }
}

void ota_start_check() {
    if (currentStatus == OTA_IDLE || currentStatus == OTA_UPDATE_AVAILABLE || currentStatus == OTA_FAILED) {
        check_requested = true;
    }
}

void ota_start_full_update() {
    if (currentStatus == OTA_UPDATE_AVAILABLE || ota_step != 0) {
        full_update_requested = true;
    }
}

OTAStatus ota_get_status() { return currentStatus; }
const char* ota_get_status_message() { return statusMessage.c_str(); }
const char* ota_get_latest_version() { return latest_version_tag; }
int ota_get_progress() { return downloadProgress; }

// --- 核心邏輯 ---

static void perform_check() {
    if (WiFi.status() != WL_CONNECTED) {
        statusMessage = "WiFi not connected";
        currentStatus = OTA_FAILED;
        return;
    }

    currentStatus = OTA_CHECKING;
    statusMessage = "Checking...";
    Serial.println("OTA: Checking for updates...");

    HTTPClient http;
    String url;
    #ifdef OTA_DEVELOPER_MODE
        url = "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases";
    #else
        url = "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest";
    #endif
    
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (error) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.c_str());
            statusMessage = "JSON parse error";
            currentStatus = OTA_FAILED;
            http.end();
            return;
        }

        JsonObject release_data;
        #ifdef OTA_DEVELOPER_MODE
            release_data = doc[0].as<JsonObject>();
        #else
            release_data = doc.as<JsonObject>();
        #endif

        const char* tag_name = release_data["tag_name"];
        if (tag_name) {
            strncpy(latest_version_tag, tag_name, sizeof(latest_version_tag) - 1);
        } else {
            statusMessage = "Tag name not found";
            currentStatus = OTA_FAILED;
            http.end();
            return;
        }

        bool fw_update_needed = false;
        bool fs_update_needed = false;
        String latest_fw_version = "";
        String latest_fs_version = "";

        for (JsonVariant asset : release_data["assets"].as<JsonArray>()) {
            String asset_name = asset["name"].as<String>();
            if (asset_name.startsWith("firmware_v")) {
                latest_fw_version = asset_name;
                latest_fw_version.replace("firmware_v", "v");
                latest_fw_version.replace(".bin", "");
                if (strcmp(latest_fw_version.c_str(), FIRMWARE_VERSION) != 0) {
                    fw_update_needed = true;
                }
            }
            if (asset_name.startsWith("littlefs_v")) {
                latest_fs_version = asset_name;
                latest_fs_version.replace("littlefs_v", "v");
                latest_fs_version.replace(".bin", "");
                if (strcmp(latest_fs_version.c_str(), FILESYSTEM_VERSION) != 0) {
                    fs_update_needed = true;
                }
            }
        }

        Serial.printf("Current FW: %s, Latest FW: %s\n", FIRMWARE_VERSION, latest_fw_version.c_str());
        Serial.printf("Current FS: %s, Latest FS: %s\n", FILESYSTEM_VERSION, latest_fs_version.c_str());

        if (fw_update_needed && fs_update_needed) {
            ota_step = 3; // Both
            statusMessage = "FW & FS Update Available!";
        } else if (fw_update_needed) {
            ota_step = 2; // FW only
            statusMessage = "Firmware Update Available!";
        } else if (fs_update_needed) {
            ota_step = 1; // FS only
            statusMessage = "Web UI Update Available!";
        } else {
            ota_step = 0; // No update
            statusMessage = "No new update";
        }

        if (ota_step != 0) {
            currentStatus = OTA_UPDATE_AVAILABLE;
        } else {
            currentStatus = OTA_IDLE;
        }
        
    } else {
        statusMessage = "HTTP Error: " + String(httpCode);
        currentStatus = OTA_FAILED;
    }
    http.end();
}

static void onProgress(int progress, int total) {
    downloadProgress = (progress * 100) / total;
    if (currentStatus == OTA_DOWNLOADING_FW) {
        statusMessage = "FW Download: " + String(downloadProgress) + "%";
    } else if (currentStatus == OTA_DOWNLOADING_FS) {
        statusMessage = "FS Download: " + String(downloadProgress) + "%";
    }
}

static void perform_firmware_update() {
    if (WiFi.status() != WL_CONNECTED) {
        statusMessage = "WiFi not connected";
        currentStatus = OTA_FAILED;
        ota_step = 0;
        return;
    }
    
    currentStatus = OTA_DOWNLOADING_FW;
    statusMessage = "FW Downloading...";
    downloadProgress = 0;
    Serial.println("OTA: Starting firmware update...");

    String bin_name = "firmware_" + String(latest_version_tag) + ".bin";
    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + String(latest_version_tag) + "/" + bin_name;
    
    Serial.print("OTA: Firmware URL: "); Serial.println(url);

    WiFiClient client;
    httpUpdate.onProgress(onProgress);
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_OK:
            Serial.println("OTA: FW Update successful! Rebooting...");
            ota_step = 0;
            delay(1000);
            ESP.restart();
            break;
        default:
            Serial.printf("OTA: FW Update failed! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            statusMessage = "FW Update failed: " + httpUpdate.getLastErrorString();
            currentStatus = OTA_FAILED;
            ota_step = 0;
            break;
    }
}

static void perform_filesystem_update() {
    if (WiFi.status() != WL_CONNECTED) {
        statusMessage = "Prerequisites not met";
        currentStatus = OTA_FAILED;
        return;
    }

    currentStatus = OTA_DOWNLOADING_FS;
    statusMessage = "FS Downloading...";
    downloadProgress = 0;
    Serial.println("OTA: Starting filesystem update...");

    String bin_name = "littlefs_" + String(latest_version_tag) + ".bin";
    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + String(latest_version_tag) + "/" + bin_name;

    Serial.print("OTA: Filesystem URL: "); Serial.println(url);

    HTTPClient http;
    http.begin(url);

    int httpCode = http.GET();
    if (httpCode > 0 && http.getLocation().length() > 0) {
        String redirectedUrl = http.getLocation();
        http.end();
        http.begin(redirectedUrl);
        httpCode = http.GET();
    }

    if (httpCode == HTTP_CODE_OK) {
        int len = http.getSize();
        if (len <= 0) { statusMessage = "FS file size is 0"; currentStatus = OTA_FAILED; http.end(); return; }
        if (!Update.begin(len, U_SPIFFS)) { Update.printError(Serial); statusMessage = "Not enough space for FS"; currentStatus = OTA_FAILED; http.end(); return; }
        
        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        if (written == len) {
            if (Update.end(true)) {
                Serial.println("OTA: FS Update successful! Setting flag and rebooting...");
                statusMessage = "FS Success! Rebooting...";
                if (ota_step == 3) {
                    ota_step = 2; // Next step is FW update
                } else {
                    ota_step = 0; // FS only update is complete
                }
                delay(1000);
                ESP.restart();
            } else { Update.printError(Serial); statusMessage = "FS Update failed!"; currentStatus = OTA_FAILED; ota_step = 0; }
        } else { Serial.printf("OTA: FS Update incomplete. Written: %d, Total: %d\n", written, len); statusMessage = "FS Download incomplete"; currentStatus = OTA_FAILED; ota_step = 0; }
    } else { statusMessage = "FS Download failed: " + String(httpCode); currentStatus = OTA_FAILED; Serial.printf("OTA: Filesystem download failed, error: %s\n", http.errorToString(httpCode).c_str()); ota_step = 0; }
    http.end();
}