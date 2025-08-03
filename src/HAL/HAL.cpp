#include "HAL.h"
#include "Config.h"
#include <mcp_can.h>
#include <Adafruit_ADS1X15.h>
#include "LuxBeacon/LuxBeacon.h"
#include <Preferences.h> 

static MCP_CAN CAN(SPI_CS_PIN);
static Adafruit_ADS1115 ads;
static volatile bool canInterruptTriggered = false;

static bool charge_relay_state = false;

static void IRAM_ATTR onCANInterrupt() {
    canInterruptTriggered = true;
}

void hal_init_pins() {
    pinMode(MCP2515_INT_PIN, INPUT);
    pinMode(SPI_CS_PIN, OUTPUT);
    digitalWrite(SPI_CS_PIN, HIGH);
    pinMode(START_BUTTON_PIN, INPUT_PULLUP);
    pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);
    pinMode(EMERGENCY_BUTTON_PIN, INPUT_PULLUP);
    pinMode(SETTING_BUTTON_PIN, INPUT_PULLUP);
    pinMode(CHARGE_RELAY_PIN, OUTPUT);
    digitalWrite(CHARGE_RELAY_PIN, LOW);
    pinMode(LOCK_SOLENOID_PIN, OUTPUT);
    digitalWrite(LOCK_SOLENOID_PIN, LOW);
    pinMode(VP_RELAY_PIN, OUTPUT);
    digitalWrite(VP_RELAY_PIN, LOW);
    pinMode(LED_STANDBY_PIN, OUTPUT);
    pinMode(LED_CHARGING_PIN, OUTPUT);
    pinMode(LED_ERROR_PIN, OUTPUT);

    attachInterrupt(digitalPinToInterrupt(MCP2515_INT_PIN), onCANInterrupt, FALLING);

    pinMode(CHARGER_RELAY_SIM_LED_PIN, OUTPUT);
    digitalWrite(CHARGER_RELAY_SIM_LED_PIN, LOW);

}

void hal_init_can() {
    if (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK) {
        Serial.println(F("HAL: MCP2515 Init Okay!"));
        CAN.setMode(MCP_NORMAL);
        CAN.init_Mask(0, 0, 0x7FF);
        CAN.init_Filt(0, 0, VEHICLE_STATUS_ID);
        CAN.init_Filt(1, 0, VEHICLE_PARAMS_ID);
        CAN.init_Mask(1, 0, 0x7FF);
        CAN.init_Filt(2, 0, VEHICLE_EMERGENCY_ID);
    } else {
        Serial.println(F("HAL: MCP2515 Init Failed! Halting."));
        while (1);
    }
}

void hal_init_adc() {
    if (!ads.begin()) {
        Serial.println("HAL: Failed to initialize ADS1115. Halting.");
        while (1);
    }
    ads.setGain(GAIN_ONE);
    Serial.println("HAL: ADS1115 initialized successfully.");
}

bool hal_get_button_state(ButtonType button) {
    switch (button) {
        case BUTTON_START: return (digitalRead(START_BUTTON_PIN) == LOW);
        case BUTTON_STOP: return (digitalRead(STOP_BUTTON_PIN) == LOW);
        case BUTTON_EMERGENCY: return (digitalRead(EMERGENCY_BUTTON_PIN) == LOW);
        case BUTTON_SETTING: return (digitalRead(SETTING_BUTTON_PIN) == LOW);
    }
    return false;
}

float hal_read_voltage_sensor() {
    if (digitalRead(CHARGE_RELAY_PIN) == LOW) {
        return 0.0;
    }
    int16_t adc_raw = ads.readADC_Differential_0_1();
    float differential_voltage = ads.computeVolts(adc_raw);
    return abs(differential_voltage) / VOLTAGE_DIVIDER_120V_RATIO;
}

float hal_read_cp_voltage() {
    int16_t adc_raw = ads.readADC_Differential_2_3();
    float differential_voltage = ads.computeVolts(adc_raw);
    return abs(differential_voltage) / VOLTAGE_DIVIDER_CP_RATIO;
}

void hal_control_vp_relay(bool on) {
    digitalWrite(VP_RELAY_PIN, on ? HIGH : LOW);
}

void hal_control_charge_relay(bool on) {
    digitalWrite(CHARGE_RELAY_PIN, on ? HIGH : LOW);
    digitalWrite(CHARGER_RELAY_SIM_LED_PIN, on ? HIGH : LOW);
    charge_relay_state = on;
}

void hal_control_coupler_lock(bool lock) {
    digitalWrite(LOCK_SOLENOID_PIN, lock ? HIGH : LOW);
}

void hal_update_leds(LedState state) {
    digitalWrite(LED_STANDBY_PIN, HIGH); // 橘燈始終常亮
    Preferences prefs;
    prefs.begin("charger_config", true); 
    bool beaconUnlocked = prefs.getBool("beacon_unlocked", false);
    prefs.end();
    #ifdef DEVELOPER_MODE
        beaconUnlocked = true;
    #endif

    switch(state) {
        case LED_STATE_STANDBY:
            digitalWrite(LED_CHARGING_PIN, LOW);
            digitalWrite(LED_ERROR_PIN, LOW);
            break;
        case LED_STATE_CHARGING:
            if (beaconUnlocked) {
                digitalWrite(LED_CHARGING_PIN, beacon_get_led_state());
            } else {
                digitalWrite(LED_CHARGING_PIN, (millis() / 500) % 2);
            }
            digitalWrite(LED_ERROR_PIN, LOW);
            break;
        case LED_STATE_COMPLETE:
            digitalWrite(LED_CHARGING_PIN, HIGH); // 常亮
            digitalWrite(LED_ERROR_PIN, LOW);
            break;
        case LED_STATE_FAULT:
            digitalWrite(LED_CHARGING_PIN, LOW);
            digitalWrite(LED_ERROR_PIN, HIGH); // 常亮
            break;
    }
}

void hal_can_send(unsigned long id, byte* data, byte len) {
    byte result = CAN.sendMsgBuf(id, 0, len, data);
    if (result == CAN_OK) {
        /*
        Serial.print(F("CAN send OK for ID 0x"));
        Serial.println(id, HEX);
        */
    } else {
        Serial.print(F("!!! CAN send FAILED for ID 0x"));
        Serial.print(id, HEX);
        Serial.print(F(", Error code: "));
        Serial.println(result);
    }
}
bool hal_can_receive(unsigned long* id, byte* len, byte* buf) {
    //if (!canInterruptTriggered) {
        //return false;
    //}
    //canInterruptTriggered = false; // 清除中斷旗標
    
    if (CAN_MSGAVAIL == CAN.checkReceive()) {
        CAN.readMsgBuf(id, len, buf);
        /*
        Serial.print("CAN received! ID: 0x");
        Serial.print(*id, HEX); // 打印出讀到的ID
        Serial.print(", Len: ");
        Serial.println(*len);
        */
        return true;
    }
    return false;
}
bool hal_is_can_interrupt_pending() {
    if (canInterruptTriggered) {
        canInterruptTriggered = false; // 讀取即清除
        return true;
    }
    return false;
}

bool hal_get_charge_relay_state() {
    return charge_relay_state;
}
