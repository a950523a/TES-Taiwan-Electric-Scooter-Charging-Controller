// src/OTAManager/OTAManager.cpp

#include "OTAManager.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include "Version.h"
#include <Update.h>
#include <LittleFS.h>

//#define OTA_DEVELOPER_MODE 

#define GITHUB_USER "a950523a"
#define GITHUB_REPO "TES-Taiwan-Electric-Scooter-Charging-Controller"

RTC_DATA_ATTR int ota_step = 0; // 0=Idle, 1=Need FS, 2=Need FW, 3=Need Both
RTC_DATA_ATTR char latest_release_tag[32] = {0};
RTC_DATA_ATTR char latest_fw_filename[64] = {0};
RTC_DATA_ATTR char latest_fs_filename[64] = {0};

static OTAStatus currentStatus = OTA_IDLE;
static String statusMessage = "Idle";
static bool check_requested = false;
static bool full_update_requested = false;
static int downloadProgress = 0;

static void perform_check();
static void perform_firmware_update();
static void perform_filesystem_update();

void ota_init() {
    Serial.println("OTA Manager Initialized.");
    // --- [修正] 檢查是否有待處理的韌體更新 ---
    if (ota_step == 2 || ota_step == 3) { // 只要需要更新 FW (無論是否單獨)
        Serial.println("OTA: Detected pending firmware update after reboot.");
        // 立即設定狀態，以便 UI 顯示
        currentStatus = OTA_DOWNLOADING_FW;
        statusMessage = "Auto-starting FW update...";
        delay(2000); // 等待 WiFi 連接
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
        if (ota_step == 1 || ota_step == 3) {
            perform_filesystem_update();
        } else if (ota_step == 2) {
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
const char* ota_get_latest_version() { return latest_release_tag; }
int ota_get_progress() { return downloadProgress; }

static void perform_check() {
    // --- [修正] 如果有待處理的更新，則不執行新的檢查，防止狀態被覆寫 ---
    if (ota_step != 0) {
        Serial.println("OTA: Update is already pending, skipping check.");
        statusMessage = "Pending update...";
        currentStatus = OTA_UPDATE_AVAILABLE;
        return;
    }

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
    http.setConnectTimeout(10000);
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
            strncpy(latest_release_tag, tag_name, sizeof(latest_release_tag) - 1);
        } else {
            statusMessage = "Tag name not found";
            currentStatus = OTA_FAILED;
            http.end();
            return;
        }

        bool fw_update_needed = false;
        bool fs_update_needed = false;
        String latest_fw_version_from_asset = "";
        String latest_fs_version_from_asset = "";

        memset(latest_fw_filename, 0, sizeof(latest_fw_filename));
        memset(latest_fs_filename, 0, sizeof(latest_fs_filename));

        for (JsonVariant asset : release_data["assets"].as<JsonArray>()) {
            String asset_name = asset["name"].as<String>();
            if (asset_name.startsWith("firmware_v")) {
                latest_fw_version_from_asset = asset_name;
                latest_fw_version_from_asset.replace("firmware_v", "v");
                latest_fw_version_from_asset.replace(".bin", "");
                if (strcmp(latest_fw_version_from_asset.c_str(), FIRMWARE_VERSION) != 0) {
                    fw_update_needed = true;
                    strncpy(latest_fw_filename, asset_name.c_str(), sizeof(latest_fw_filename) - 1);
                }
            }
            if (asset_name.startsWith("littlefs_v")) {
                latest_fs_version_from_asset = asset_name;
                latest_fs_version_from_asset.replace("littlefs_v", "v");
                latest_fs_version_from_asset.replace(".bin", "");
                
                String local_fs_version = "N/A";
                File versionFile = LittleFS.open("/fs_version.txt", "r");
                if (versionFile) {
                    local_fs_version = versionFile.readStringUntil('\n');
                    versionFile.close();
                    local_fs_version.trim();
                }
                
                if (strcmp(latest_fs_version_from_asset.c_str(), local_fs_version.c_str()) != 0) {
                    fs_update_needed = true;
                    strncpy(latest_fs_filename, asset_name.c_str(), sizeof(latest_fs_filename) - 1);
                }
            }
        }
        
        String local_fs_version_for_log = "N/A";
        File versionFile = LittleFS.open("/fs_version.txt", "r");
        if (versionFile) {
            local_fs_version_for_log = versionFile.readStringUntil('\n');
            versionFile.close();
            local_fs_version_for_log.trim();
        }

        Serial.printf("Current FW: %s, Latest FW from asset: %s\n", FIRMWARE_VERSION, latest_fw_version_from_asset.c_str());
        Serial.printf("Current FS: %s, Latest FS from asset: %s\n", local_fs_version_for_log.c_str(), latest_fs_version_from_asset.c_str());

        if (fw_update_needed && fs_update_needed) {
            ota_step = 3;
            statusMessage = "FW & FS Update Available!";
        } else if (fw_update_needed) {
            ota_step = 2;
            statusMessage = "Firmware Update Available!";
        } else if (fs_update_needed) {
            ota_step = 1;
            statusMessage = "Web UI Update Available!";
        } else {
            ota_step = 0;
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

    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + String(latest_release_tag) + "/" + String(latest_fw_filename);
    Serial.print("OTA: Firmware URL: "); Serial.println(url);

    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(10000);

    int httpCode = http.GET();
    if (httpCode > 0 && http.getLocation().length() > 0) {
        String redirectedUrl = http.getLocation();
        http.end();
        http.begin(redirectedUrl);
        httpCode = http.GET();
    }

    if (httpCode == HTTP_CODE_OK) {
        int len = http.getSize();
        if (len <= 0) { statusMessage = "FW file size is 0"; currentStatus = OTA_FAILED; http.end(); return; }
        
        if (!Update.begin(len, U_FLASH)) {
            Update.printError(Serial);
            statusMessage = "Not enough space for FW";
            currentStatus = OTA_FAILED;
            http.end();
            return;
        }
        
        WiFiClient* stream = http.getStreamPtr();
        Update.onProgress(onProgress);
        size_t written = Update.writeStream(*stream);

        if (written == len) {
            if (Update.end(true)) {
                Serial.println("OTA: FW Update successful! Rebooting...");
                statusMessage = "FW Success! Rebooting...";
                ota_step = 0;
                delay(1000);
                ESP.restart();
            } else {
                Update.printError(Serial);
                statusMessage = "FW Update failed!";
                currentStatus = OTA_FAILED;
                //ota_step = 0;
            }
        } else {
            Serial.printf("OTA: FW Update incomplete. Written: %d, Total: %d\n", written, len);
            statusMessage = "FW Download incomplete";
            currentStatus = OTA_FAILED;
            //ota_step = 0;
        }
    } else {
        statusMessage = "FW Download failed: " + String(httpCode);
        currentStatus = OTA_FAILED;
        Serial.printf("OTA: Firmware download failed, error: %s\n", http.errorToString(httpCode).c_str());
        //ota_step = 0;
    }
    http.end();
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

    String url = "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/download/" + String(latest_release_tag) + "/" + String(latest_fs_filename);
    Serial.print("OTA: Filesystem URL: "); Serial.println(url);

    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(10000);

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
        Update.onProgress(onProgress);
        size_t written = Update.writeStream(*stream);

        if (written == len) {
            if (Update.end(true)) {
                Serial.println("OTA: FS Update successful! Setting flag and rebooting...");
                statusMessage = "FS Success! Rebooting...";
                if (ota_step == 3) {
                    ota_step = 2;
                } else {
                    ota_step = 0;
                }
                delay(1000);
                ESP.restart();
            } else { 
                Update.printError(Serial); 
                statusMessage = "FS Update failed!"; 
                currentStatus = OTA_FAILED; 
                //ota_step = 0; 
            }
        } else { 
            Serial.printf("OTA: FS Update incomplete. Written: %d, Total: %d\n", written, len); 
            statusMessage = "FS Download incomplete"; 
            currentStatus = OTA_FAILED; 
            //ota_step = 0; 
        }
    } else { 
        statusMessage = "FS Download failed: " + String(httpCode); 
        currentStatus = OTA_FAILED; 
        Serial.printf("OTA: Filesystem download failed, error: %s\n", http.errorToString(httpCode).c_str()); 
        //ota_step = 0; 
    }
    http.end();
}