#include "HAL.h"
#include "Config.h"
#include <Adafruit_ADS1X15.h>
#include "LuxBeacon/LuxBeacon.h"
#include <Preferences.h> 
#include "driver/twai.h"
#include <Wire.h>

static Adafruit_ADS1115 ads;

static bool charge_relay_state = false;

void hal_init_pins() {
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
}

void hal_init_can() {
    // 1. 通用配置: 設定工作模式為Normal, 並指定TX/RX引腳
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_NORMAL);
    
    // 2. 時序配置: 設定CAN總線鮑率為 500Kbps
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    
    // 3. 濾波器配置: 暫時先接收所有報文，方便除錯。
    //    未來可以設定精確的Filter來只接收需要的ID，以提升性能。
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // 4. 安裝並啟動TWAI驅動
    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("HAL: Failed to install TWAI driver!");
        return;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("HAL: Failed to start TWAI driver!");
        return;
    }
    Serial.println("HAL: Onboard TWAI driver started successfully.");
}


void hal_init_adc() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.printf("HAL: I2C bus initialized on SDA=%d, SCL=%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
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
    twai_message_t message;
    message.identifier = id;
    message.flags = TWAI_MSG_FLAG_NONE; // 標準幀
    message.data_length_code = len;
    
    // 複製數據
    for (int i = 0; i < len; i++) {
        message.data[i] = data[i];
    }

    // 發送報文，pdMS_TO_TICKS(100) 表示最多等待100ms
    if (twai_transmit(&message, pdMS_TO_TICKS(100)) != ESP_OK) {
        Serial.print("!!! HAL: TWAI send FAILED for ID 0x");
        Serial.println(id, HEX);
    }
}

bool hal_can_receive(unsigned long* id, byte* len, byte* buf) {
    twai_message_t message;
    
    // 檢查並接收報文，pdMS_TO_TICKS(0) 表示不等待，立刻返回
    if (twai_receive(&message, pdMS_TO_TICKS(0)) == ESP_OK) {
        *id = message.identifier;
        *len = message.data_length_code;
        
        // 複製數據
        for (int i = 0; i < *len; i++) {
            buf[i] = message.data[i];
        }
        return true;
    }
    
    return false;
}

bool hal_get_charge_relay_state() {
    return charge_relay_state;
}
