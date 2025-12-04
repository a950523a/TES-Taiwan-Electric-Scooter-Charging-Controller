const MAX_VOLTAGE = 110.0;
const MAX_CURRENT = 10.0;
const MANUFACTURER_ID = 0x0000;
var State;
(function (State) {
    State[State["IDLE"] = 0] = "IDLE";
    State[State["HANDSHAKE"] = 1] = "HANDSHAKE";
    State[State["INSULATION"] = 2] = "INSULATION";
    State[State["PRECHARGE"] = 3] = "PRECHARGE";
    State[State["CHARGING"] = 4] = "CHARGING";
    State[State["COMPLETE"] = 5] = "COMPLETE";
    State[State["ERROR"] = 6] = "ERROR";
    State[State["EMERGENCY"] = 7] = "EMERGENCY";
})(State || (State = {}));
let state = State.IDLE;
let stateTimer = 0;
let blinkTimer = 0;
let btnLatches = [false, false, false, false];
tes.console.log("TES Charger V3 Logic Started");
tes.setLed(tes.Led.STANDBY, 1);
tes.setLed(tes.Led.CHARGING, 0);
tes.setLed(tes.Led.ERROR, 0);
tes.setRelay(tes.Relay.MAIN, 0);
tes.setRelay(tes.Relay.VP, 0);
tes.setRelay(tes.Relay.LOCK, 0);
if (tes.oled) {
    tes.oled.clear();
    tes.oled.drawText(0, 12, "TES Charger V3");
    tes.oled.drawText(0, 24, "Initializing...");
    tes.oled.update();
}
globalThis.loop = function () {
    tes.feedWatchdog();
    stateTimer++;
    blinkTimer++;
    let cpVolts = tes.readCP();
    let outVolts = tes.readVoltage();
    let isConnected = (cpVolts > 8.0 && cpVolts < 10.0);
    let btnStart = checkButton(tes.Button.START);
    let btnStop = checkButton(tes.Button.STOP);
    let btnEmg = checkButton(tes.Button.EMERGENCY);
    if (btnEmg) {
        state = State.EMERGENCY;
    }
    switch (state) {
        case State.IDLE:
            tes.setLed(tes.Led.STANDBY, 1);
            tes.setLed(tes.Led.CHARGING, 0);
            if (isConnected) {
                if (btnStart) {
                    state = State.HANDSHAKE;
                    stateTimer = 0;
                    tes.console.log("Starting Handshake...");
                }
            }
            else {
                tes.setRelay(tes.Relay.MAIN, 0);
                tes.setRelay(tes.Relay.VP, 0);
                tes.setRelay(tes.Relay.LOCK, 0);
            }
            break;
        case State.HANDSHAKE:
            if (tes.vehicle.chargingEnabled) {
                tes.console.log("Vehicle Ready. Starting Insulation Test.");
                state = State.INSULATION;
                stateTimer = 0;
            }
            if (stateTimer > 100)
                toError("Handshake Timeout");
            break;
        case State.INSULATION:
            tes.setRelay(tes.Relay.LOCK, 1);
            tes.setRelay(tes.Relay.VP, 1);
            if (stateTimer > 10) {
                state = State.PRECHARGE;
                stateTimer = 0;
            }
            break;
        case State.PRECHARGE:
            tes.charger.outputVoltage = Math.min(tes.vehicle.voltageRequest, MAX_VOLTAGE);
            tes.charger.outputCurrent = Math.min(tes.vehicle.currentRequest, MAX_CURRENT);
            if (stateTimer > 10) {
                tes.setRelay(tes.Relay.MAIN, 1);
                tes.console.log("Main Relay Closed. Charging!");
                state = State.CHARGING;
            }
            break;
        case State.CHARGING:
            tes.setLed(tes.Led.STANDBY, 0);
            tes.setLed(tes.Led.CHARGING, (blinkTimer % 10 < 5) ? 1 : 0);
            let reqV = tes.vehicle.voltageRequest;
            let reqI = tes.vehicle.currentRequest;
            if (reqV > MAX_VOLTAGE)
                reqV = MAX_VOLTAGE;
            if (reqI > MAX_CURRENT)
                reqI = MAX_CURRENT;
            tes.charger.outputVoltage = reqV;
            tes.charger.outputCurrent = reqI;
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
            tes.setLed(tes.Led.CHARGING, 1);
            tes.setRelay(tes.Relay.MAIN, 0);
            tes.charger.outputCurrent = 0;
            if (stateTimer > 30) {
                tes.setRelay(tes.Relay.VP, 0);
                tes.setRelay(tes.Relay.LOCK, 0);
                state = State.IDLE;
            }
            break;
        case State.ERROR:
        case State.EMERGENCY:
            tes.setLed(tes.Led.STANDBY, 0);
            tes.setLed(tes.Led.CHARGING, 0);
            tes.setLed(tes.Led.ERROR, 1);
            tes.setRelay(tes.Relay.MAIN, 0);
            tes.setRelay(tes.Relay.VP, 0);
            tes.setRelay(tes.Relay.LOCK, 0);
            if (btnStop) {
                state = State.IDLE;
                tes.setLed(tes.Led.ERROR, 0);
            }
            break;
    }
    if (stateTimer % 10 === 0 && tes.oled) {
        updateScreen();
    }
};
function checkButton(id) {
    let pressed = tes.getButton(id);
    if (pressed && !btnLatches[id]) {
        btnLatches[id] = true;
        return true;
    }
    else if (!pressed) {
        btnLatches[id] = false;
    }
    return false;
}
function toError(msg) {
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
    tes.oled.clear();
    let stateStr = State[state];
    tes.oled.drawText(0, 10, stateStr);
    if (state === State.CHARGING) {
        let v = tes.readVoltage();
        let i = tes.charger.outputCurrent;
        tes.oled.drawText(0, 25, `${v.toFixed(1)}V  ${i.toFixed(1)}A`);
        tes.oled.drawText(0, 40, `SOC: ${tes.vehicle.soc}%`);
    }
    else {
        tes.oled.drawText(0, 25, `CP: ${tes.readCP().toFixed(1)}V`);
    }
    tes.oled.update();
}
