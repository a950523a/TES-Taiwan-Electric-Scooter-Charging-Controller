// tes_charger_software/src/tes_api.d.ts

// 定義全域物件 tes
declare namespace tes {
    
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

    let loop: () => void;
}


