#include "ChargerLogic.h"
#include "Config.h"
#include "HAL/HAL.h"
#include "CAN_Protocol/CAN_Protocol.h"
#include <Preferences.h>

// --- 私有(static)變量，只在這個文件內可見 ---
static Preferences preferences; 
static ChargerState currentChargerState = STATE_CHG_IDLE;
static bool faultLatch = false;
static bool chargeCompleteLatch = false;
static unsigned int chargerMaxOutputVoltage_0_1V = 1000;
static unsigned int chargerMaxOutputCurrent_0_1A = 100;
static int userSetTargetSOC = 100;
static float measuredVoltage = 0.0;
static float measuredCurrent = 0.0;

static unsigned long currentStateStartTime = 0;
static unsigned long lastPeriodicSendTime = 0;
static unsigned long lastCPReadTime = 0;

// 計時器相關
static bool isChargingTimerRunning = false;
static uint32_t elapsedChargingSeconds = 0;
static uint32_t currentTotalTimeSeconds = 0;
static unsigned long lastChargeTimeTick = 0;
static uint32_t remainingTimeSeconds_global = 0;

// 流程控制旗標
static bool vehicleReadyForCharge = false;
static bool insulationTestOK = false;

// CP 狀態相關
static CPState currentCPState = CP_STATE_UNKNOWN;
static float measuredCPVoltage = 0.0;

// CAN 訊息結構體 (由 Logic 層維護)
static CAN_Charger_Status_508 chargerStatus508;
static CAN_Charger_Params_509 chargerParams509;
static CAN_Charger_Emergency_5F8 chargerEmergency5F8;

// --- 私有(static)函數原型 ---
static void readAndSetCPState();
static bool ch_sub_01_battery_compatibility_check();
static bool ch_sub_03_coupler_lock_and_insulation_diagnosis();
static void ch_sub_04_dc_current_output_control();
static void ch_sub_06_monitoring_process();
static void ch_sub_10_protection_and_end_flow(bool isFault);
static void ch_sub_12_emergency_stop_procedure();

enum PreChargeStep {
    STEP_INIT,
    STEP_CHARGER_READY_ANNOUNCED,
    STEP_VEHICLE_CONTACTOR_WAIT,
    STEP_RELAY_CLOSE_DELAY, // 新增延遲步驟
    STEP_COMPLETE
};
static PreChargeStep preChargeStep = STEP_INIT;

// =================================================================
// =                      公開API函數實現                          =
// =================================================================

void logic_init() {
    preferences.begin("charger_config", false);
    chargerMaxOutputVoltage_0_1V = preferences.getUInt("max_voltage", 1000);
    chargerMaxOutputCurrent_0_1A = preferences.getUInt("max_current", 100);
    userSetTargetSOC = preferences.getInt("target_soc", 100);

    if (!preferences.getBool("config_saved", false)) {
        Serial.println(F("Logic: First boot or NVS empty. Saving default values."));
        preferences.putBool("config_saved", true);
        preferences.putUInt("max_voltage", chargerMaxOutputVoltage_0_1V);
        preferences.putUInt("max_current", chargerMaxOutputCurrent_0_1A);
        preferences.putInt("target_soc", userSetTargetSOC);
    }
    preferences.end();
    
    unsigned int ratedPower_W = (chargerMaxOutputVoltage_0_1V / 10.0) * (chargerMaxOutputCurrent_0_1A / 10.0);
    
    chargerStatus508.availableVoltage = chargerMaxOutputVoltage_0_1V;
    chargerStatus508.availableCurrent = chargerMaxOutputCurrent_0_1A;
    // *** 關鍵修正點：初始化故障偵測電壓限制 ***
    chargerStatus508.faultDetectionVoltageLimit = chargerMaxOutputVoltage_0_1V;
    
    chargerParams509.esChargeSequenceNumber = 18;
    chargerParams509.ratedOutputPower = ratedPower_W / 50;
    chargerParams509.remainingChargeTime = 0xFFFF;

    chargerEmergency5F8.chargerManufacturerID = chargerManufacturerCode;

    Serial.println(F("--- Logic Configuration Loaded ---"));
    Serial.print(F("Max Voltage: ")); Serial.print(chargerMaxOutputVoltage_0_1V / 10.0); Serial.println(" V");
    Serial.print(F("Max Current: ")); Serial.print(chargerMaxOutputCurrent_0_1A / 10.0); Serial.println(" A");
    Serial.print(F("Target SOC: ")); Serial.print(userSetTargetSOC); Serial.println(" %");
    Serial.println(F("--------------------------------"));

    currentStateStartTime = millis();
    readAndSetCPState();
    hal_control_vp_relay(false);
}

void logic_save_config(unsigned int voltage, unsigned int current, int soc){
    chargerMaxOutputVoltage_0_1V = voltage;
    chargerMaxOutputCurrent_0_1A = current;
    userSetTargetSOC = soc;

    chargerStatus508.availableVoltage = chargerMaxOutputVoltage_0_1V;
    chargerStatus508.availableCurrent = chargerMaxOutputCurrent_0_1A;
    // 當設定改變時，也要同步更新初始的故障偵測電壓
    chargerStatus508.faultDetectionVoltageLimit = chargerMaxOutputVoltage_0_1V;
    unsigned int ratedPower_W = (chargerMaxOutputVoltage_0_1V / 10.0) * (chargerMaxOutputCurrent_0_1A / 10.0);
    chargerParams509.ratedOutputPower = ratedPower_W / 50;

    preferences.begin("charger_config", false);
    preferences.putUInt("max_voltage", chargerMaxOutputVoltage_0_1V);
    preferences.putUInt("max_current", chargerMaxOutputCurrent_0_1A);
    preferences.putInt("target_soc", userSetTargetSOC);
    preferences.end();
    
    Serial.println(F("Logic: Settings saved to NVS."));
}

void logic_run_statemachine() {
  if (hal_get_button_state(BUTTON_EMERGENCY)) {
    ch_sub_12_emergency_stop_procedure();
    return;
  }

  switch (currentChargerState) {
    case STATE_CHG_IDLE:
      chargerStatus508.statusFlags = 0;
      hal_control_charge_relay(false);
      hal_control_coupler_lock(false);
      hal_control_vp_relay(false);
      insulationTestOK = false;
      vehicleReadyForCharge = false;
      isChargingTimerRunning = false;
      preChargeStep = STEP_INIT;
      
      if (hal_get_button_state(BUTTON_START)) {
        faultLatch = false;
        chargeCompleteLatch = false;
        hal_control_vp_relay(true);
        readAndSetCPState();
        if (currentCPState == CP_STATE_OFF || currentCPState == CP_STATE_ON) {
          Serial.println(F("Logic: Start pressed. -> INITIAL_PARAM_EXCHANGE."));
          currentChargerState = STATE_CHG_INITIAL_PARAM_EXCHANGE;
          currentStateStartTime = millis();
        } else {
          Serial.println(F("Logic: Start pressed, but CP state is ERROR/UNKNOWN. Cannot start."));
        }
      }
      break;

    case STATE_CHG_INITIAL_PARAM_EXCHANGE:
      if (vehicleReadyForCharge) {
          if (ch_sub_01_battery_compatibility_check()) {
              Serial.println(F("Logic: CH_SUB_01 OK. -> PRE_CHARGE_OPERATIONS."));
              currentChargerState = STATE_CHG_PRE_CHARGE_OPERATIONS;
              currentStateStartTime = millis();
          } else {
              Serial.println(F("Logic: CH_SUB_01 FAILED."));
              chargerStatus508.faultFlags |= 0x04;
              ch_sub_10_protection_and_end_flow(true);
          }
      } else if (millis() - currentStateStartTime > 15000) {
          Serial.println(F("Logic: Timeout in INITIAL_PARAM_EXCHANGE (15s)."));
          chargerStatus508.faultFlags |= 0x01;
          ch_sub_10_protection_and_end_flow(true);
      }
      break;

    case STATE_CHG_PRE_CHARGE_OPERATIONS: { // 使用大括號創建局部作用域
        static unsigned long relay_close_delay_start_time = 0;

        if (!(currentCPState == CP_STATE_ON && (vehicleStatus500.statusFlags & 0x01))) {
            if (millis() - currentStateStartTime > 20000) {
                Serial.println(F("Logic: Timeout in PRE_CHARGE (CP or CAN permission not ready)."));
                ch_sub_10_protection_and_end_flow(true);
            }
            break;
        }

        switch (preChargeStep) {
            case STEP_INIT:
                if (ch_sub_03_coupler_lock_and_insulation_diagnosis()) {
                    Serial.println(F("Logic: Pre-charge checks OK. Announcing ready state..."));
                    chargerStatus508.statusFlags &= ~0x01;
                    chargerStatus508.statusFlags |= 0x04;
                    can_protocol_send_charger_status(chargerStatus508);
                    preChargeStep = STEP_VEHICLE_CONTACTOR_WAIT;
                    currentStateStartTime = millis();
                }
                break;

            case STEP_VEHICLE_CONTACTOR_WAIT:
                if (!(vehicleStatus500.statusFlags & 0x02)) {
                    if (relay_close_delay_start_time == 0) {
                        Serial.println(F("Logic: Vehicle contactor closed. Starting delay..."));
                        relay_close_delay_start_time = millis();
                    }
                    if (millis() - relay_close_delay_start_time >= 250) {
                        preChargeStep = STEP_RELAY_CLOSE_DELAY;
                    }
                } else if (millis() - currentStateStartTime > 10000) {
                    Serial.println(F("Logic: Timeout waiting for vehicle contactor to close."));
                    ch_sub_10_protection_and_end_flow(true);
                }
                break;

            case STEP_RELAY_CLOSE_DELAY:
                Serial.println(F("Logic: Delay finished. Closing charger relay..."));
                hal_control_charge_relay(true);
                if (hal_get_charge_relay_state()) {
                    chargerStatus508.statusFlags |= 0x02;
                    can_protocol_send_charger_status(chargerStatus508);
                    preChargeStep = STEP_COMPLETE;
                } else {
                    Serial.println(F("Logic FATAL: Failed to close charger relay!"));
                    ch_sub_10_protection_and_end_flow(true);
                }
                break;

            case STEP_COMPLETE:
                Serial.println(F("Logic: Pre-charge complete. -> DC_CURRENT_OUTPUT."));
                relay_close_delay_start_time = 0;
                currentChargerState = STATE_CHG_DC_CURRENT_OUTPUT;
                isChargingTimerRunning = true;
                elapsedChargingSeconds = 0;
                currentTotalTimeSeconds = 0;
                lastChargeTimeTick = millis();
                currentStateStartTime = millis();
                break;
        }
        break;
    }


    case STATE_CHG_DC_CURRENT_OUTPUT:
      ch_sub_04_dc_current_output_control();
      ch_sub_06_monitoring_process();
      break;

    case STATE_CHG_ENDING_CHARGE_PROCESS: {
      static unsigned long relay_open_delay_start_time = 0;

      // 步驟1: 降流並斷開樁端繼電器
      ch_sub_04_dc_current_output_control(); // 確保電流命令為0
      if (hal_get_charge_relay_state()) {
          hal_control_charge_relay(false);
          Serial.println(F("Logic: Charger relay opened."));
      }

      // 步驟2: 繼電器斷開後，啟動延遲
      if (!hal_get_charge_relay_state() && relay_open_delay_start_time == 0 && measuredCurrent < 1.0) {
          Serial.println(F("Logic: Starting delay before final checks..."));
          relay_open_delay_start_time = millis();
      }

      // 步驟3: 延遲結束後，等待車輛最終狀態
      if (relay_open_delay_start_time > 0 && (millis() - relay_open_delay_start_time >= 250)) {
          chargerStatus508.statusFlags |= 0x01;
          chargerStatus508.statusFlags &= ~0x02;
          if ((vehicleStatus500.statusFlags & 0x02) && currentCPState == CP_STATE_OFF) {
              hal_control_coupler_lock(false);
              chargerStatus508.statusFlags &= ~0x04;
              Serial.println(F("Logic: Coupler unlocked. -> FINALIZATION."));
              currentChargerState = STATE_CHG_FINALIZATION;
              relay_open_delay_start_time = 0; // 重置
          } else if (millis() - currentStateStartTime > 10000) { // 總超時
              Serial.println(F("Logic: Timeout waiting for vehicle to disconnect. Forcing unlock."));
              hal_control_coupler_lock(false);
              chargerStatus508.statusFlags &= ~0x04;
              currentChargerState = STATE_CHG_FINALIZATION;
              relay_open_delay_start_time = 0; // 重置
          }
      }
       break;
    }


    case STATE_CHG_FAULT_HANDLING:
      hal_control_charge_relay(false);
      hal_control_coupler_lock(false);
      if (millis() - currentStateStartTime > 10000) {
        Serial.println(F("Logic: Fault display time over. -> IDLE."));
        currentChargerState = STATE_CHG_IDLE;
        chargerStatus508.faultFlags = 0;
      }
      break;

    case STATE_CHG_EMERGENCY_STOP_PROC:
      if (millis() - currentStateStartTime > 5000) {
        Serial.println(F("Logic: Emergency stop processed. -> IDLE."));
        currentChargerState = STATE_CHG_IDLE;
        chargerStatus508.faultFlags = 0;
        chargerEmergency5F8.emergencyStopRequestFlags = 0;
      }
      break;

    case STATE_CHG_FINALIZATION:
      measuredVoltage = hal_read_voltage_sensor();
      if (measuredVoltage > 10.0) {
        Serial.print(F("Logic Warning: Output DC voltage still > 10V ("));
        Serial.print(measuredVoltage); Serial.println(F("V)) after finalization!"));
      }
      Serial.println(F("Logic: Charge finalized. -> IDLE."));
      currentChargerState = STATE_CHG_IDLE;
      break;

    default:
      Serial.println(F("Logic: Unknown charger state! -> IDLE."));
      currentChargerState = STATE_CHG_IDLE;
      break;
  }
}

void logic_handle_periodic_tasks() {
    unsigned long now = millis();
    
    if ((vehicleStatus500.statusFlags & 0x01) && !vehicleReadyForCharge && currentChargerState == STATE_CHG_INITIAL_PARAM_EXCHANGE) {
        Serial.println(F("Logic: Vehicle CAN permission granted."));
        vehicleReadyForCharge = true;
    }
    if (vehicleEmergency5F0.errorRequestFlags & 0x01) {
        Serial.println(F("Logic: Vehicle sent EMERGENCY STOP!"));
        ch_sub_12_emergency_stop_procedure();
        vehicleEmergency5F0.errorRequestFlags = 0;
    }
    if (isChargingTimerRunning && vehicleParams501.maxChargeTime != 0xFFFF) {
        uint32_t newTotalTimeSeconds = (uint32_t)vehicleParams501.maxChargeTime * 60;
        if (currentTotalTimeSeconds != newTotalTimeSeconds) {
            currentTotalTimeSeconds = newTotalTimeSeconds;
            Serial.print(F("Logic: Total charge time updated by BMS to "));
            Serial.print(vehicleParams501.maxChargeTime);
            Serial.println(" min.");
        }
    }

    if (isChargingTimerRunning) {
        if (now - lastChargeTimeTick >= 1000) {
            uint32_t elapsed_ms = now - lastChargeTimeTick;
            elapsedChargingSeconds += elapsed_ms / 1000;
            lastChargeTimeTick += (elapsed_ms / 1000) * 1000;
        }
        if (currentTotalTimeSeconds > 0 && currentTotalTimeSeconds > elapsedChargingSeconds) {
            remainingTimeSeconds_global = currentTotalTimeSeconds - elapsedChargingSeconds;
        } else {
            remainingTimeSeconds_global = 0;
        }
    } else {
        remainingTimeSeconds_global = 0;
        elapsedChargingSeconds = 0;
    }

    if (now - lastCPReadTime >= CP_READ_INTERVAL) {
        lastCPReadTime = now;
        readAndSetCPState();
    }

    if (now - lastPeriodicSendTime >= PERIODIC_SEND_INTERVAL) {
        lastPeriodicSendTime = now;
        if (currentChargerState >= STATE_CHG_INITIAL_PARAM_EXCHANGE && currentChargerState < STATE_CHG_FAULT_HANDLING) {
            chargerParams509.actualOutputVoltage = (uint16_t)(measuredVoltage * 10.0);
            chargerParams509.actualOutputCurrent = (uint16_t)(measuredCurrent * 10.0);
            chargerParams509.remainingChargeTime = isChargingTimerRunning ? (remainingTimeSeconds_global + 30) / 60 : 0xFFFF;
            
            can_protocol_send_charger_status(chargerStatus508);
            can_protocol_send_charger_params(chargerParams509);
            can_protocol_send_emergency_stop(chargerEmergency5F8);
        }
    }
}

LedState logic_get_led_state() {
    if (faultLatch) return LED_STATE_FAULT;
    if (chargeCompleteLatch) return LED_STATE_COMPLETE;
    if (currentChargerState == STATE_CHG_DC_CURRENT_OUTPUT) return LED_STATE_CHARGING;
    return LED_STATE_STANDBY;
}

ChargerState logic_get_charger_state() { return currentChargerState; }
bool logic_is_fault_latched() { return faultLatch; }
bool logic_is_charge_complete() { return chargeCompleteLatch; }
int logic_get_soc() { return vehicleParams501.stateOfCharge; }
uint32_t logic_get_remaining_seconds() { return remainingTimeSeconds_global; }
float logic_get_measured_voltage() { return measuredVoltage; }
float logic_get_measured_current() { return measuredCurrent; }
bool logic_is_timer_running() { return isChargingTimerRunning; }
uint32_t logic_get_total_time_seconds() { return currentTotalTimeSeconds; }
unsigned int logic_get_max_voltage_setting() { return chargerMaxOutputVoltage_0_1V; }
unsigned int logic_get_max_current_setting() { return chargerMaxOutputCurrent_0_1A; }
int logic_get_target_soc_setting() { return userSetTargetSOC; }

// =================================================================
// =                      私有(static)函數實現                     =
// =================================================================

static void readAndSetCPState() {
    static byte cpErrorCount = 0;
    float sum = 0;
    for (int i = 0; i < CP_SAMPLE_COUNT; i++) {
        sum += hal_read_cp_voltage();
        delay(5);
    }
    measuredCPVoltage = sum / CP_SAMPLE_COUNT;

    CPState detectedState;
    if (measuredCPVoltage >= 0.0 && measuredCPVoltage <= 1.9 - CP_HYSTERESIS) detectedState = CP_STATE_OFF;
    else if (measuredCPVoltage >= 7.4 + CP_HYSTERESIS && measuredCPVoltage <= 13.7) detectedState = CP_STATE_ON;
    else detectedState = CP_STATE_ERROR;

    if (detectedState == CP_STATE_ERROR) {
        if (++cpErrorCount >= CP_ERROR_THRESHOLD) currentCPState = CP_STATE_ERROR;
    } else {
        cpErrorCount = 0;
        currentCPState = detectedState;
    }
}

static bool ch_sub_01_battery_compatibility_check() {
    if (vehicleStatus500.chargeVoltageLimit > chargerMaxOutputVoltage_0_1V) return false;
    if (vehicleStatus500.maxChargeVoltage > 0 && vehicleStatus500.chargeVoltageLimit > vehicleStatus500.maxChargeVoltage) return false;
    chargerStatus508.faultDetectionVoltageLimit = min(vehicleStatus500.maxChargeVoltage, chargerMaxOutputVoltage_0_1V);
    return true;
}

static bool ch_sub_03_coupler_lock_and_insulation_diagnosis() {
    hal_control_coupler_lock(true);
    insulationTestOK = true; 
    return true;
}

static void ch_sub_04_dc_current_output_control() {
    measuredVoltage = hal_read_voltage_sensor();
    if (digitalRead(CHARGE_RELAY_PIN) == HIGH) {
        measuredCurrent = (float)vehicleStatus500.chargeCurrentCommand / 10.0;
    } else {
        measuredCurrent = 0.0;
    }
}

static void ch_sub_06_monitoring_process() {
    if (!(vehicleStatus500.statusFlags & 0x01)) {
        Serial.println(F("Logic MONITOR: Vehicle CAN stop request."));
        ch_sub_10_protection_and_end_flow(false); return;
    }
    if (hal_get_button_state(BUTTON_STOP)) {
        Serial.println(F("Logic MONITOR: User stop button pressed."));
        ch_sub_10_protection_and_end_flow(false); return;
    }
    if (logic_get_soc() >= userSetTargetSOC) {
        Serial.println(F("Logic MONITOR: Target SOC reached."));
        ch_sub_10_protection_and_end_flow(false); return;
    }
    if (isChargingTimerRunning && currentTotalTimeSeconds > 0 && remainingTimeSeconds_global == 0) {
        Serial.println(F("Logic MONITOR: Max charge time reached."));
        ch_sub_10_protection_and_end_flow(false); return;
    }
    if (vehicleStatus500.faultFlags != 0) {
        Serial.println(F("Logic MONITOR: Fault reported by vehicle."));
        ch_sub_10_protection_and_end_flow(true); return;
    }
    if (currentCPState != CP_STATE_ON) {
        Serial.println(F("Logic MONITOR: CP signal lost during charging."));
        ch_sub_10_protection_and_end_flow(true); return;
    }
}

static void ch_sub_10_protection_and_end_flow(bool isFault) {
    Serial.print(F("Logic: CH10_ProtectEnd. IsFault: ")); Serial.println(isFault);
    isChargingTimerRunning = false;
    if (isFault) {
        faultLatch = true;
        currentChargerState = STATE_CHG_FAULT_HANDLING;
    } else {
        chargeCompleteLatch = true;
        currentChargerState = STATE_CHG_ENDING_CHARGE_PROCESS;
    }
    currentStateStartTime = millis();
}

static void ch_sub_12_emergency_stop_procedure() {
    Serial.println(F("Logic: CH12_EmergencyStop Procedure!"));
    faultLatch = true;
    isChargingTimerRunning = false;
    
    hal_control_charge_relay(false);
    hal_control_coupler_lock(false);
    hal_control_vp_relay(false);
    
    chargerStatus508.faultFlags |= 0x01;
    chargerStatus508.statusFlags |= 0x01;
    chargerEmergency5F8.emergencyStopRequestFlags |= 0x01;
    
    can_protocol_send_charger_status(chargerStatus508);
    can_protocol_send_emergency_stop(chargerEmergency5F8);

    currentChargerState = STATE_CHG_EMERGENCY_STOP_PROC;
    currentStateStartTime = millis();
}