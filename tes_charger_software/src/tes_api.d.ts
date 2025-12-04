// tes_charger_software/src/tes_api.d.ts

// 定義全域物件 tes
declare namespace tes {

    enum Button {
        START = 0,
        SETTING = 1,
        STOP = 2,
        EMERGENCY = 3
    }

    enum Relay {
        MAIN = 0,
        VP = 1,
        LOCK = 2
    }

    enum Led {
        STANDBY = 0, // Orange
        CHARGING = 1, // Green
        ERROR = 2     // Red
    }
    
    // --- 硬體能力 ---
    const caps: {
        hasScreen: boolean;
        hasLed: boolean;
    };

    // --- 控制 ---
    function setRelay(id: number, state: number): void;
    function setLed(id: number, state: number): void;
    function getButton(id: number): boolean;

    // --- 感測 ---
    function readVoltage(): number;
    function readCP(): number;
    function feedWatchdog(): void;

    // --- 通訊 ---
    // data 是數字陣列，例如 [0x01, 0x02]
    function sendCAN(id: number, data: number[]): void;

    // --- OLED (可選) ---
    const oled: {
        clear(): void;
        drawText(x: number, y: number, text: string): void;
        update(): void;
    } | undefined; // 如果沒螢幕就是 undefined

    // --- Console ---
    const console: {
        log(...args: any[]): void;
    };

    const system: {
        reboot(): void;
        resetWiFi(): void;
    };

    const vehicle: {
        readonly soc: number;
        readonly voltageRequest: number; // 單位 V
        readonly currentRequest: number; // 單位 A
        readonly chargingEnabled: boolean;
        readonly faultFlags: number;
    };

    const charger: {
        outputVoltage: number;
        outputCurrent: number;
        
        // 設定狀態 (isCharging, isLocked)
        setStatus(charging: boolean, locked: boolean): void;
        
        // 設定錯誤碼 (0 = 正常)
        setFault(flags: number): void;
        
        // 設定參數 (額定功率 W, 剩餘時間 min)
        setParams(power: number, time: number): void;
    };

    function sendCAN(id: number, data: number[]): void;

}

declare var loop: () => void;

