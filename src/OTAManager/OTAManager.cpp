// src/OTAManager/OTAManager.cpp

#include "OTAManager.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include "Version.h" // 引用 FIRMWARE_VERSION
#include "config.h"

// --- 設定您的 GitHub 專案 ---
#define GITHUB_USER "a950523a"
#define GITHUB_REPO "TES-Taiwan-Electric-Scooter-Charging-Controller"

// --- 私有變數 ---
static OTAStatus currentStatus = OTA_IDLE;
static String latestVersion = "";
static int downloadProgress = 0;
static String statusMessage = "Idle";
static bool check_requested = false;
static bool update_requested = false;

// --- 私有函數原型 ---
static void perform_check();
static void perform_update();

void ota_init() {
    Serial.println("OTA Manager Initialized.");
}

void ota_handle_tasks() {
    if (check_requested) {
        check_requested = false;
        perform_check();
    }
    if (update_requested) {
        update_requested = false;
        perform_update();
    }
}

void ota_start_check() {
    if (currentStatus == OTA_IDLE || currentStatus == OTA_UPDATE_AVAILABLE || currentStatus == OTA_FAILED) {
        check_requested = true;
    }
}

void ota_start_update() {
    if (currentStatus == OTA_UPDATE_AVAILABLE) {
        update_requested = true;
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
    statusMessage = "Downloading: " + String(downloadProgress) + "%";
}

static void perform_update() {
    if (WiFi.status() != WL_CONNECTED || latestVersion == "") {
        statusMessage = "Prerequisites not met";
        currentStatus = OTA_FAILED;
        return;
    }

    currentStatus = OTA_DOWNLOADING;
    statusMessage = "Downloading...";
    downloadProgress = 0;
    Serial.println("OTA: Starting firmware update...");

    String bin_name = "firmware_" + latestVersion + ".bin";
    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + latestVersion + "/" + bin_name;
    
    Serial.print("OTA: Firmware URL: "); Serial.println(url);

    WiFiClient client;
    httpUpdate.onProgress(onProgress);
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_OK:
            Serial.println("OTA: Update successful! Rebooting...");
            break;
        case HTTP_UPDATE_FAILED:
            Serial.printf("OTA: Update failed! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            statusMessage = "Update failed: " + httpUpdate.getLastErrorString();
            currentStatus = OTA_FAILED;
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("OTA: No updates available.");
            statusMessage = "No new update";
            currentStatus = OTA_IDLE;
            break;
    }
}