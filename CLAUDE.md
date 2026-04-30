# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 firmware for a DC charging controller compatible with the **TES-0D-02-01** standard used by Taiwan electric scooters (e.g., eMoving iE125). The controller bridges an external PSU to a vehicle's BMS via CAN bus, with a web UI, OLED display, BLE, and OTA updates.

Current firmware: **v2.5.0-Beta** on branch `V3-dev`. License: CC BY-NC-SA 4.0 (non-commercial).

## Build & Flash Commands

This is a **PlatformIO** project (Arduino framework, ESP32-S3).

```bash
# Build firmware
pio run

# Upload firmware to device
pio run --target upload

# Upload LittleFS filesystem (data/ directory) — needed for web UI
pio run --target uploadfs

# Open serial monitor (115200 baud)
pio device monitor

# Build + upload + monitor in sequence
pio run --target upload && pio device monitor
```

No unit tests are implemented (`test/` is empty).

## Architecture

### FreeRTOS Task Structure

Six concurrent tasks are created in `src/main.cpp`, sharing data via two mutexes (`canDataMutex`, `displayDataMutex`):

| Task | Priority | Tick | Responsibility |
|------|----------|------|----------------|
| CAN | 5 | 10 ms | Receive & parse CAN bus messages |
| Logic | 4 | 20 ms | State machine + periodic PSU commands |
| UI | 3 | 50 ms | OLED rendering + button input |
| WiFi | 2 | 100 ms | Web server, REST API, WiFi management |
| OTA | 1 | 500 ms | Firmware update checks & installation |
| Monitor | 1 | 10 s | FreeRTOS stack watermark reporting |

### Module Breakdown

- **`ChargerLogic/`** — The core state machine implementing TES-0D-02-01. Eight states from `STATE_CHG_IDLE` through `STATE_CHG_FINALIZATION`. Reads user settings from NVS flash. This is the most critical module.
- **`CAN_Protocol/`** — Parses vehicle CAN messages (0x500, 0x501, 0x5F0) and sends charger status (0x508, 0x509, 0x5F8) using ESP32 TWAI driver on GPIO 17/18.
- **`PowerSupplyController/`** — Sends voltage/current commands to the external PSU over CAN.
- **`HAL/`** — GPIO (buttons, LEDs, relays, solenoid), ADC (ADS1115 for 120 V voltage divider and CP line).
- **`UI/`** — OLED menu system (u8g2 library) with four physical buttons: START (up/increase), STOP (down/decrease), EMERGENCY (hard stop), SETTING (menu enter).
- **`NetworkServices/`** — AsyncTCP + ESPAsyncWebServer serving `data/index.html` dashboard and REST endpoints (`/start_charge`, `/stop_charge`, settings).
- **`OTAManager/`** — Downloads and installs firmware (FW) and filesystem (FS) images; reports progress percentage.
- **`BLE_Comms/`** — BLE notification of charger state (optional feature).
- **`LuxBeacon/`** — LED beacon flashing patterns for status indication.

### Key Data Structures (`src/Charger_Defs.h`)

- `ChargerState` enum — state machine states
- `UIState` enum — display/menu states  
- `DisplayData` struct — shared data between Logic/UI tasks (protected by `displayDataMutex`)
- `CAN_*_5XX` structs — raw TES protocol CAN frame layouts

### CAN Message IDs

| ID | Direction | Content |
|----|-----------|---------|
| 0x500 | Vehicle → Charger | Status, faults, requested current/voltage |
| 0x501 | Vehicle → Charger | SOC, max charge time, ETA |
| 0x5F0 | Vehicle → Charger | Emergency flags |
| 0x508 | Charger → Vehicle | Charger status, available voltage/current |
| 0x509 | Charger → Vehicle | Actual output voltage/current, remaining time |
| 0x5F8 | Charger → Vehicle | Emergency stop request |

## Configuration

All user-tunable parameters are in **`src/config.h`**:
- GPIO pin assignments for buttons, LEDs, relays, CAN, I2C
- Voltage/current safety limits (max 120 V / 100 A)
- Voltage divider calibration ratios
- CAN message IDs and timing intervals
- WiFi AP SSID (`TES_Charger_ESP32`) and default password
- `OLED_ENABLED` and `DEV_MODE` feature flags

## Flash & Filesystem Layout

16 MB flash with a custom partition table (`partitions_16MB.csv`). The `data/` directory (web UI HTML + `fs_version.txt`) is flashed as a LittleFS partition separately from the firmware image. Custom ESP32-S3 board definitions live in `boards/`.

## Safety Notes (Relevant to Code Changes)

- CAN bus errors sent to the vehicle **can damage the BMS** — validate all CAN frame construction carefully.
- The emergency stop path (`STATE_CHG_EMERGENCY_STOP_PROC`) must remain reliable; do not introduce blocking calls in the CAN or Logic tasks.
- Mutex acquisition order is always `canDataMutex` before `displayDataMutex` to avoid deadlocks.
