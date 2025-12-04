/// <reference path="./tes_api.d.ts" />

// --- 設定參數 ---
const MAX_VOLTAGE = 110.0; // V
const MAX_CURRENT = 10.0;  // A
const RATED_POWER = 1000;
const MANUFACTURER_ID = 0x0000; // 配合 V2 設定

// --- 狀態定義 ---
enum State {
    IDLE,           // 待機 (等待 CP)
    HANDSHAKE,      // 握手 (交換參數)
    INSULATION,     // 絕緣檢測 (VP ON, Lock ON)
    PRECHARGE,      // 預充電 (等待電池閉合)
    CHARGING,       // 正式充電
    COMPLETE,       // 充電完成
    ERROR,          // 錯誤狀態
    EMERGENCY       // 緊急停止
}

let state = State.IDLE;
let stateTimer = 0;      // 狀態計時器 (單位: 100ms)
let blinkTimer = 0;      // LED 閃爍計時
let btnLatches = [false, false, false, false];

// --- 初始化 ---
tes.console.log("TES Charger V3 Logic Started");
tes.setLed(tes.Led.STANDBY, 1); // 橘燈亮
tes.setLed(tes.Led.CHARGING, 0);
tes.setLed(tes.Led.ERROR, 0);
tes.setRelay(tes.Relay.MAIN, 0);
tes.setRelay(tes.Relay.VP, 0);
tes.setRelay(tes.Relay.LOCK, 0);

// 初始化：設定預設參數 (IDLE 狀態)
tes.charger.setParams(RATED_POWER, 0xFFFF); // 時間未知
tes.charger.setStatus(false, false);        // 未充電，未鎖定
tes.charger.setFault(0);                    // 無錯誤
tes.charger.outputVoltage = MAX_VOLTAGE;    // 告知能力
tes.charger.outputCurrent = MAX_CURRENT;

// 清空螢幕
if (tes.oled) {
    tes.oled.clear();
    tes.oled.drawText(0, 12, "TES Charger V3");
    tes.oled.drawText(0, 24, "Initializing...");
    tes.oled.update();
}

// --- 主迴圈 (每 100ms 執行一次) ---
(globalThis as any).loop = function() {
    tes.feedWatchdog(); // 餵狗
    stateTimer++;
    blinkTimer++;

    // 1. 讀取輸入
    let cpVolts = tes.readCP();
    let outVolts = tes.readVoltage();
    let isConnected = (cpVolts > 8.0 && cpVolts < 10.0); // CP=9V 代表連接
    
    // 讀取按鈕 (含簡單 Latch)
    let btnStart = checkButton(tes.Button.START);
    let btnStop = checkButton(tes.Button.STOP);
    let btnEmg = checkButton(tes.Button.EMERGENCY);

    // 2. 緊急停止檢查 (最高優先級)
    if (btnEmg) {
        state = State.EMERGENCY;
    }

    // 3. 狀態機邏輯
    switch (state) {
        case State.IDLE:
            tes.setLed(tes.Led.STANDBY, 1);
            tes.setLed(tes.Led.CHARGING, 0);
            
            if (isConnected) {
                // 如果連接上且按下 Start，或者 V2 邏輯是自動開始？
                // 這裡假設按 Start 才開始
                if (btnStart) {
                    state = State.HANDSHAKE;
                    stateTimer = 0;
                    tes.console.log("Starting Handshake...");
                }
            } else {
                // 未連接時，確保繼電器全關
                tes.setRelay(tes.Relay.MAIN, 0);
                tes.setRelay(tes.Relay.VP, 0);
                tes.setRelay(tes.Relay.LOCK, 0);
            }
            break;

        case State.HANDSHAKE:
            // 這裡 C++ 底層會自動發送 0x508
            // 我們只需要檢查車輛是否有回應 (0x500)
            // tes.vehicle.chargingEnabled 來自 0x500 的 Status Flag
            if (tes.vehicle.chargingEnabled) {
                tes.console.log("Vehicle Ready. Starting Insulation Test.");
                state = State.INSULATION;
                stateTimer = 0;
            }
            
            // 超時保護 (10秒)
            if (stateTimer > 100) toError("Handshake Timeout");
            break;

        case State.INSULATION:
            // 鎖定槍頭，開啟 VP
            tes.setRelay(tes.Relay.LOCK, 1);
            tes.setRelay(tes.Relay.VP, 1);
            
            // 模擬絕緣檢測時間 (1秒)
            if (stateTimer > 10) {
                state = State.PRECHARGE;
                stateTimer = 0;
            }
            break;

        case State.PRECHARGE:
            // 設定輸出電壓與電流 (告知車輛)
            tes.charger.outputVoltage = Math.min(tes.vehicle.voltageRequest, MAX_VOLTAGE);
            tes.charger.outputCurrent = Math.min(tes.vehicle.currentRequest, MAX_CURRENT);
            
            // 等待車輛閉合接觸器 (通常透過 0x500 status 判斷，這裡簡化)
            // 延遲 1 秒後閉合主繼電器
            if (stateTimer > 10) {
                tes.setRelay(tes.Relay.MAIN, 1);
                tes.console.log("Main Relay Closed. Charging!");
                state = State.CHARGING;
            }
            break;

        case State.CHARGING:
            tes.setLed(tes.Led.STANDBY, 0);
            // 綠燈閃爍 (每 500ms)
            tes.setLed(tes.Led.CHARGING, (blinkTimer % 10 < 5) ? 1 : 0);

            // 持續更新輸出命令
            let reqV = tes.vehicle.voltageRequest;
            let reqI = tes.vehicle.currentRequest;
            
            // 安全限制
            if (reqV > MAX_VOLTAGE) reqV = MAX_VOLTAGE;
            if (reqI > MAX_CURRENT) reqI = MAX_CURRENT;

            tes.charger.outputVoltage = reqV;
            tes.charger.outputCurrent = reqI;

            // 停止條件
            if (btnStop) {
                state = State.COMPLETE;
                tes.console.log("User Stop");
            }
            if (!isConnected) {
                toError("Cable Disconnected");
            }
            if (outVolts > MAX_VOLTAGE + 5) {
                toError("Over Voltage Protection");
            }
            if (!tes.vehicle.chargingEnabled) {
                state = State.COMPLETE;
                tes.console.log("Vehicle Stop Request");
            }
            break;

        case State.COMPLETE:
            tes.setLed(tes.Led.CHARGING, 1); // 綠燈常亮
            tes.setRelay(tes.Relay.MAIN, 0); // 斷開主電
            tes.charger.outputCurrent = 0;
            
            // 延遲後斷開 VP 和 Lock
            if (stateTimer > 30) { // 3秒後
                tes.setRelay(tes.Relay.VP, 0);
                tes.setRelay(tes.Relay.LOCK, 0);
                state = State.IDLE;
            }
            break;

        case State.ERROR:
        case State.EMERGENCY:
            tes.setLed(tes.Led.STANDBY, 0);
            tes.setLed(tes.Led.CHARGING, 0);
            tes.setLed(tes.Led.ERROR, 1); // 紅燈亮
            
            // 切斷所有輸出
            tes.setRelay(tes.Relay.MAIN, 0);
            tes.setRelay(tes.Relay.VP, 0);
            tes.setRelay(tes.Relay.LOCK, 0);
            
            // 按 Stop 復歸
            if (btnStop) {
                state = State.IDLE;
                tes.setLed(tes.Led.ERROR, 0);
            }
            break;
    }

    // 4. UI 更新 (每秒一次)
    if (stateTimer % 10 === 0 && tes.oled) {
        updateScreen();
    }
};

// --- 輔助函數 ---

function checkButton(id: tes.Button): boolean {
    let pressed = tes.getButton(id);
    if (pressed && !btnLatches[id]) {
        btnLatches[id] = true;
        return true; // Rising Edge
    } else if (!pressed) {
        btnLatches[id] = false;
    }
    return false;
}

function toError(msg: string) {
    tes.console.log("Error: " + msg);
    state = State.ERROR;
    if (tes.oled) {
        tes.oled.clear();
        tes.oled.drawText(0, 20, "ERROR");
        tes.oled.drawText(0, 40, msg);
        tes.oled.update();
    }
}

function updateScreen() {
    tes.oled!.clear();
    
    let stateStr = State[state]; // Enum 轉字串
    tes.oled!.drawText(0, 10, stateStr);
    
    if (state === State.CHARGING) {
        let v = tes.readVoltage();
        let i = tes.charger.outputCurrent; // 這裡讀回設定值 (或者是感測值，看你 C++ 怎麼綁)
        tes.oled!.drawText(0, 25, `${v.toFixed(1)}V  ${i.toFixed(1)}A`);
        tes.oled!.drawText(0, 40, `SOC: ${tes.vehicle.soc}%`);
    } else {
        tes.oled!.drawText(0, 25, `CP: ${tes.readCP().toFixed(1)}V`);
    }
    
    tes.oled!.update();
}