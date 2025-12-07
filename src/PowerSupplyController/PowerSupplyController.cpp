#include "PowerSupplyController.h"

// 使用 UART0 與電源通訊
// 在啟用了 USB CDC 的情況下，我們需要手動宣告 HardwareSerial
HardwareSerial PowerSerial(0); 

static bool isConnected = false;
static float lastVoltage = 0.0;
static float lastCurrent = 0.0;
static unsigned long lastPacketTime = 0;
static float last_sent_voltage = -1.0;
static float last_sent_current = -1.0;

static String inputBuffer = ""; 

void psc_init() {
    // 初始化 UART0，鮑率 115200，RX=44, TX=43 (請確認您的 PCB 實際腳位)
    // 假設您的 PCB 使用的是 ESP32-S3 的預設 UART0 腳位
    // 如果您在 Config.h 中有定義，請使用 Config.h 中的定義
    // 这里假设使用默认脚位，如果不是请修改
    PowerSerial.begin(115200, SERIAL_8N1, 44, 43); 
    Serial.println("PSC: PowerSupplyController initialized on UART0.");
}

void psc_handle_task() {
    // --- [修正] 非阻塞接收邏輯 ---
    // 每次只讀取部分數據，不阻塞任務
    while (PowerSerial.available()) {
        char c = PowerSerial.read();
        
        if (c == '\n') {
            // 讀到換行符，處理整行數據
            inputBuffer.trim(); // 去除前後空白
            
            // 解析 V=...,I=...
            if (inputBuffer.startsWith("V=")) {
                int iIndex = inputBuffer.indexOf(",I=");
                if (iIndex > 0) {
                    String vStr = inputBuffer.substring(2, iIndex);
                    String iStr = inputBuffer.substring(iIndex + 3);
                    
                    lastVoltage = vStr.toFloat();
                    lastCurrent = iStr.toFloat();
                    lastPacketTime = millis();
                    
                    if (!isConnected) {
                        isConnected = true;
                        Serial.println("PSC: Connected!");
                        
                        last_sent_voltage = -1.0;
                        last_sent_current = -1.0;
                    }
                }
            }
            
            // 清空緩衝區，準備下一行
            inputBuffer = "";
        } else {
            // 不是換行符，加入緩衝區
            // 防止緩衝區過大 (例如雜訊導致一直沒有 \n)
            if (inputBuffer.length() < 128) {
                inputBuffer += c;
            } else {
                // 緩衝區滿了還沒換行，可能是垃圾數據，清空
                inputBuffer = ""; 
            }
        }
    }
    
    // 超時檢測
    if (isConnected && (millis() - lastPacketTime > 3000)) {
        isConnected = false;
        Serial.println("PSC: Connection lost!");
    }
}
void psc_set_voltage(float v) {
    if (isConnected) {
        if (abs(v - last_sent_voltage) > 0.05) { 
            PowerSerial.print("SET:V=");
            PowerSerial.println(v);
            last_sent_voltage = v;
            Serial.printf("PSC SET V: %.1f\n", v);
        }
    }
}

void psc_set_current(float a) {
    if (isConnected) {
        if (abs(a - last_sent_current) > 0.05) { 
            PowerSerial.print("SET:I=");
            PowerSerial.println(a);
            last_sent_current = a;
            Serial.printf("PSC SET I: %.1f\n", a); // Debug
        } 
    }
}

bool psc_is_connected() { return isConnected; }
float psc_get_voltage() { return lastVoltage; }
float psc_get_current() { return lastCurrent; }