#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include <Arduino.h>

// OTA 狀態枚舉
enum OTAStatus {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UPDATE_AVAILABLE,
    OTA_DOWNLOADING,
    OTA_INSTALLING,
    OTA_SUCCESS,
    OTA_FAILED
};

void ota_init();
void ota_handle_tasks();

// 觸發 OTA 動作的接口
void ota_start_check();
void ota_start_update();

// 提供給外部讀取狀態的接口
OTAStatus ota_get_status();
const char* ota_get_status_message();
const char* ota_get_latest_version();
int ota_get_progress();

#endif // OTAMANAGER_H