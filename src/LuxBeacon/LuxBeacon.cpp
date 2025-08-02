// src/LuxBeacon/LuxBeacon.cpp

#include "LuxBeacon/LuxBeacon.h"
#include "Config.h"
#include "HAL/HAL.h"
#include "ChargerLogic/ChargerLogic.h"
#include <Preferences.h> 


// --- 內部代碼，使用代號命名 ---
// Signal Patterns for Glyphs 0-9
static bool current_led_output_state = false;
static const char* GLYPH_PATTERNS[] = {
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."
};

enum SignalState {
    STATE_IDLE,
    STATE_START_GLYPH,
    STATE_EMITTING_PULSE,
    STATE_PULSE_GAP,
    STATE_GLYPH_GAP,
    STATE_SEQUENCE_GAP
};

static SignalState signalState = STATE_IDLE;
static unsigned long signalTimer = 0;
static int currentDataValue = -1;
static int currentGlyphIndex = 0;
static int currentPulseIndex = 0;
static char dataBuffer[4]; // SOC最多3位數 + 結束符

// --- 接口函數的實現 ---

void beacon_init() {
    Serial.println("LuxBeacon Module Initialized.");
}

bool beacon_get_led_state() {
    return current_led_output_state;
}

void beacon_handle_tasks() {

    if (logic_get_charger_state() != STATE_CHG_DC_CURRENT_OUTPUT) {
        current_led_output_state = false;
        signalState = STATE_IDLE;
        currentDataValue = -1;
        return;
    }

 
    int latest_data = logic_get_soc();
    if (latest_data < 0 || latest_data > 100) {
        return;
    }

    if (latest_data != currentDataValue || signalState == STATE_IDLE) {
        currentDataValue = latest_data;
        sprintf(dataBuffer, "%d", currentDataValue);
        currentGlyphIndex = 0;
        signalState = STATE_START_GLYPH;
        signalTimer = millis(); 
    }

    unsigned long now = millis();

    switch (signalState) {
        case STATE_START_GLYPH:
            if (dataBuffer[currentGlyphIndex] == '\0') { 
                signalState = STATE_SEQUENCE_GAP; 
            } else {
                currentPulseIndex = 0;
                signalState = STATE_EMITTING_PULSE;
                signalTimer = now; 
            }
            break;

        case STATE_EMITTING_PULSE: {
            current_led_output_state = true;
            
            int digit = dataBuffer[currentGlyphIndex] - '0';
            char pulse = GLYPH_PATTERNS[digit][currentPulseIndex];
            
            unsigned long duration = (pulse == '.') ? (LUX_BEACON_TIME_UNIT_MS * 1) : (LUX_BEACON_TIME_UNIT_MS * 3);
            
            if (now - signalTimer >= duration) {
                signalState = STATE_PULSE_GAP;
                signalTimer = now;
            }
            break;
        }

        case STATE_PULSE_GAP:
            current_led_output_state = false;
            if (now - signalTimer >= LUX_BEACON_TIME_UNIT_MS * 1) { 
                currentPulseIndex++;
                int digit = dataBuffer[currentGlyphIndex] - '0';
                if (GLYPH_PATTERNS[digit][currentPulseIndex] == '\0') {
                    signalState = STATE_GLYPH_GAP; 
                } else {
                    signalState = STATE_EMITTING_PULSE; 
                }
                signalTimer = now;
            }
            break;

        case STATE_GLYPH_GAP:
            current_led_output_state = false;
            if (now - signalTimer >= LUX_BEACON_TIME_UNIT_MS * 3) { 
                currentGlyphIndex++;
                signalState = STATE_START_GLYPH;
                signalTimer = now;
            }
            break;

        case STATE_SEQUENCE_GAP:
            current_led_output_state = false;
            if (now - signalTimer >= LUX_BEACON_TIME_UNIT_MS * 7) { 
                signalState = STATE_IDLE;
            }
            break;
        
        case STATE_IDLE:
        default:
            break;
    }
}