/// <reference path="./tes_api.d.ts" />

tes.console.log("Hardware Test Script Started");

// 狀態變數 (0=OFF, 1=ON)
// 對應三個通道: 0(Main/Stdby), 1(VP/Chg), 2(Lock/Err)
let channelStates = [0, 0, 0];

// 按鈕防抖動用的旗標 (簡單版 latch)
let btnLatches = [false, false, false, false];

// 全開全關模式旗標
let allToggleState = false;

(globalThis as any).loop = function() {
    
    // --- 讀取按鈕並處理邏輯 ---
    
    // 檢查 Btn 0, 1, 2 (個別控制)
    for (let i = 0; i < 3; i++) {
        let pressed = tes.getButton(i);
        
        if (pressed && !btnLatches[i]) {
            // 按下瞬間：切換狀態
            channelStates[i] = channelStates[i] ? 0 : 1;
            tes.console.log(`Channel ${i} Toggled: ${channelStates[i]}`);
            applyState(i, channelStates[i]);
            btnLatches[i] = true;
        } else if (!pressed) {
            btnLatches[i] = false; // 放開後重置
        }
    }

    // 檢查 Btn 3 (全開/全關)
    let btnAll = tes.getButton(3);
    if (btnAll && !btnLatches[3]) {
        allToggleState = !allToggleState;
        let newState = allToggleState ? 1 : 0;
        
        tes.console.log(`All Channels Set to: ${newState}`);
        
        for (let i = 0; i < 3; i++) {
            channelStates[i] = newState;
            applyState(i, newState);
        }
        btnLatches[3] = true;
    } else if (!btnAll) {
        btnLatches[3] = false;
    }

    // --- 更新 OLED 顯示 ---
    // 為了避免 I2C 頻寬塞爆，這裡每 200ms (2 ticks) 更新一次即可
    // 假設 loop 是 100ms 呼叫一次
    if (Date.now() % 200 < 100 && tes.oled) {
        tes.oled.clear();
        tes.oled.drawText(0, 10, "HW Test Mode");
        
        // 顯示狀態: [1] [0] [1]
        let statusStr = `L/R: [${channelStates[0]}] [${channelStates[1]}] [${channelStates[2]}]`;
        tes.oled.drawText(0, 25, statusStr);
        
        // 顯示按鈕狀態 (方便除錯)
        let btnStr = `BTN: ${tes.getButton(0)?1:0}${tes.getButton(1)?1:0}${tes.getButton(2)?1:0}${tes.getButton(3)?1:0}`;
        tes.oled.drawText(0, 40, btnStr);
        
        tes.oled.update();
    }
};

// 輔助函數：同時設定 LED 和 Relay
function applyState(id: number, state: number) {
    tes.setLed(id, state);
    tes.setRelay(id, state);
}