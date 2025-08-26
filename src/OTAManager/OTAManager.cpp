// src/OTAManager/OTAManager.cpp

#include "OTAManager.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include "Version.h" // 引用 FIRMWARE_VERSION
#include <Update.h> 
#include "config.h"

// --- 設定您的 GitHub 專案 ---
#define GITHUB_USER "a950523a"
#define GITHUB_REPO "TES-Taiwan-Electric-Scooter-Charging-Controller"

RTC_DATA_ATTR int ota_step = 0;

// --- 私有變數 ---
static OTAStatus currentStatus = OTA_IDLE;
static String latestVersion = "";
static int downloadProgress = 0;
static String statusMessage = "Idle";
static bool check_requested = false;
static bool full_update_requested = false;

// --- 私有函數原型 ---
static void perform_check();
static void perform_firmware_update();
static void perform_filesystem_update();

void ota_init() {
    Serial.println("OTA Manager Initialized.");
    if (ota_step == 1) {
        Serial.println("OTA: Detected pending firmware update after FS update.");
        // 延遲一小段時間，等待 WiFi 連接
        delay(3000); 
        // 觸發韌體更新
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
        // --- [修改] 完整更新流程 ---
        if (ota_step == 0) {
            // 第一步：更新檔案系統
            perform_filesystem_update();
        } else if (ota_step == 1) {
            // 第二步：更新韌體
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
    if (currentStatus == OTA_UPDATE_AVAILABLE || ota_step == 1) {
        full_update_requested = true;
    }
}

OTAStatus ota_get_status() {
    return currentStatus;
}

const char* ota_get_status_message() {
    return statusMessage.c_str();
}

const char* ota_get_latest_version() {
    return latestVersion.c_str();
}

int ota_get_progress() {
    return downloadProgress;
}

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
        Serial.println("OTA: Developer mode enabled. Checking all releases.");
    #else
        url = "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest";
        Serial.println("OTA: Checking latest stable release.");
    #endif

    http.begin(url);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        #ifdef OTA_DEVELOPER_MODE
            int first_tag_name_index = payload.indexOf("\"tag_name\"");
            if (first_tag_name_index != -1) {
                payload = payload.substring(first_tag_name_index);
            }
        #endif

        int tag_name_index = payload.indexOf("\"tag_name\"");
        if (tag_name_index != -1) {
            int start_index = payload.indexOf("\"", tag_name_index + 12) + 1;
            int end_index = payload.indexOf("\"", start_index);
            latestVersion = payload.substring(start_index, end_index);
            
            Serial.print("OTA: Current version: "); Serial.println(FIRMWARE_VERSION);
            Serial.print("OTA: Latest version from GitHub: "); Serial.println(latestVersion);

            if (latestVersion.equalsIgnoreCase(FIRMWARE_VERSION)) {
                statusMessage = "No new update";
                currentStatus = OTA_IDLE;
            } else {
                statusMessage = "Update available!";
                currentStatus = OTA_UPDATE_AVAILABLE;
            }
        } else {
            statusMessage = "JSON parse error";
            currentStatus = OTA_FAILED;
        }
    } else {
        statusMessage = "HTTP Error: " + String(httpCode);
        currentStatus = OTA_FAILED;
        Serial.printf("OTA: HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
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
        ota_step = 0; // 清除標記
        return;
    }
    // 如果是自動觸發的，需要先獲取最新版本號
    if (latestVersion == "") {
        perform_check();
        // 如果檢查後沒有更新，或檢查失敗，則終止
        if (currentStatus != OTA_UPDATE_AVAILABLE) {
            ota_step = 0; // 清除標記
            return;
        }
    }

    currentStatus = OTA_DOWNLOADING_FW;
    statusMessage = "FW Downloading...";
    downloadProgress = 0;
    Serial.println("OTA: Starting firmware update (Step 2/2)...");

    String bin_name = "firmware_" + latestVersion + ".bin";
    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + latestVersion + "/" + bin_name;
    
    Serial.print("OTA: Firmware URL: "); Serial.println(url);

    WiFiClient client;
    httpUpdate.onProgress(onProgress);
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_OK:
            Serial.println("OTA: FW Update successful! Rebooting...");
            // 成功後會自動重啟，RTC 標記會被清除 (因為程式碼開頭沒有設定它)
            break;
        default:
            Serial.printf("OTA: FW Update failed! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            statusMessage = "FW Update failed: " + httpUpdate.getLastErrorString();
            currentStatus = OTA_FAILED;
            ota_step = 0; // 清除標記
            break;
    }
}

static void perform_filesystem_update() {
    if (WiFi.status() != WL_CONNECTED || latestVersion == "") {
        statusMessage = "Prerequisites not met";
        currentStatus = OTA_FAILED;
        return;
    }

    currentStatus = OTA_DOWNLOADING_FS;
    statusMessage = "FS Downloading...";
    downloadProgress = 0;
    Serial.println("OTA: Starting filesystem update (Step 1/2)...");

    String bin_name = "littlefs_" + latestVersion + ".bin";
    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + latestVersion + "/" + bin_name;

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
        if (len <= 0) { /* ... 錯誤處理 ... */ return; }

        if (!Update.begin(len, U_SPIFFS)) { /* ... 錯誤處理 ... */ return; }

        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        if (written == len) {
            if (Update.end(true)) {
                Serial.println("OTA: FS Update successful! Setting flag and rebooting...");
                statusMessage = "FS Success! Rebooting...";
                // --- [關鍵] 設定 RTC 標記 ---
                ota_step = 1; 
                delay(1000);
                ESP.restart();
            } else { /* ... 錯誤處理 ... */ }
        } else { /* ... 錯誤處理 ... */ }
    } else { /* ... 錯誤處理 ... */ }
    http.end();
}