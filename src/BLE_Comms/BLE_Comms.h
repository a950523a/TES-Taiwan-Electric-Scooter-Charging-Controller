#ifndef BLE_COMMS_H
#define BLE_COMMS_H

// 初始化藍牙服務
void ble_comms_init();

// 在主循環中定期呼叫，用於更新需要通知(Notify)的數據
void ble_comms_handle_tasks();

#endif // BLE_COMMS_H