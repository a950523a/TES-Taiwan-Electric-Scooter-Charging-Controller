tes.console.log("Hardware Test Script Started");
let channelStates = [0, 0, 0];
let btnLatches = [false, false, false, false];
let allToggleState = false;
globalThis.loop = function () {
    tes.feedWatchdog();
    for (let i = 0; i < 3; i++) {
        let pressed = tes.getButton(i);
        if (pressed && !btnLatches[i]) {
            channelStates[i] = channelStates[i] ? 0 : 1;
            tes.console.log(`Channel ${i} Toggled: ${channelStates[i]}`);
            applyState(i, channelStates[i]);
            btnLatches[i] = true;
        }
        else if (!pressed) {
            btnLatches[i] = false;
        }
    }
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
    }
    else if (!btnAll) {
        btnLatches[3] = false;
    }
    if (Date.now() % 200 < 100 && tes.oled) {
        tes.oled.clear();
        tes.oled.drawText(0, 10, "HW Test Mode");
        let statusStr = `L/R: [${channelStates[0]}] [${channelStates[1]}] [${channelStates[2]}]`;
        tes.oled.drawText(0, 25, statusStr);
        let btnStr = `BTN: ${tes.getButton(0) ? 1 : 0}${tes.getButton(1) ? 1 : 0}${tes.getButton(2) ? 1 : 0}${tes.getButton(3) ? 1 : 0}`;
        tes.oled.drawText(0, 40, btnStr);
        tes.oled.update();
    }
};
function applyState(id, state) {
    tes.setLed(id, state);
    tes.setRelay(id, state);
}
