# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 firmware for a DC charging controller compatible with the **TES-0D-02-01** standard used by Taiwan electric scooters (e.g., eMoving iE125). The controller bridges an external PSU to a vehicle's BMS via CAN bus, with a web UI, OLED display, and OTA updates.

- **V2** (`src/`): PlatformIO / Arduino framework — current production firmware (v2.5.0-Beta)
- **V3** (`v3/`): ESP-IDF native — active refactor; target is feature parity with V2 in pure C99

License: CC BY-NC-SA 4.0 (non-commercial).

---

## V2 Build (PlatformIO / Arduino)

```bash
pio run                          # build
pio run --target upload          # flash firmware
pio run --target uploadfs        # flash LittleFS (web UI data/)
pio device monitor               # serial monitor 115200 baud
```

No unit tests (`test/` is empty).

---

## V3 Build (ESP-IDF)

```bash
cd v3
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`sdkconfig.defaults` enables PSRAM (OPI 8MB), DIO 80 MHz flash, TWAI, USB CDC console, FreeRTOS 1 kHz tick.

---

## V3 Architecture

### Guiding Principle

`tes_protocol/` is **zero-dependency C99** — no ESP-IDF, no FreeRTOS, no OS calls. It can be compiled on any platform (STM32, PC unit tests, etc.) by swapping `hal/` and `platform/`. All time, GPIO, and CAN operations are injected by the caller.

### Component Layers

```
main/           ← FreeRTOS tasks + global IPC objects
services/       ← event_bus, config_svc, display_svc, network_svc, ota_svc
drivers/        ← can_driver, adc_driver, psu_driver, display_driver, led_driver
hal/            ← hal_gpio, hal_i2c, hal_uart, hal_nvs  (ESP-IDF wrappers)
platform/       ← platform_tick_ms() — the only PAL function tes_protocol needs
tes_protocol/   ← tes_types.h, tes_codec.c, tes_sm.c  (portable, zero OS deps)
```

### State Machine Design (`tes_protocol/tes_sm.c`)

```c
void tes_sm_tick(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
```

- **Inputs** are assembled by `task_tes_sm` each tick: CAN frames from queue, ADC values from globals, PSU status from driver, button events from queue, `tick_ms` from `platform_tick_ms()`.
- **Outputs** are hardware actions (`relay_on`, `coupler_lock`, `psu_set_voltage`, CAN TX flags) — executed by `task_tes_sm` after the tick, never inside the SM.
- The SM never reads time or touches hardware directly.

States: `IDLE → PARAM_EXCHANGE → PRE_CHARGE → CHARGING → ENDING → FAULT / EMERGENCY / FINALIZE`

### IPC Between Tasks

```
task_can_rx  ──[can_frame_t queue depth=16]──→  task_tes_sm
task_hal_poll ─[g_btn_event_queue depth=8]──→  task_tes_sm
task_hal_poll ─[g_emergency_stop atomic_bool]─→ task_tes_sm (every tick)
task_hal_poll ─[g_adc_cp_voltage / g_adc_output_voltage volatile]→ task_tes_sm
task_tes_sm  ──[g_snapshot + g_snapshot_mutex]─→ task_display / task_network
task_tes_sm  ──[event_bus]──────────────────────→ task_ota / future subscribers
```

### Task Table

| Task | Priority | Stack | Period | Role |
|------|----------|-------|--------|------|
| `task_can_rx` | 15 | 2 KB | event | TWAI receive → raw queue |
| `task_tes_sm` | 12 | 4 KB | 10 ms | SM tick + output execution + snapshot update |
| `task_hal_poll` | 10 | 2 KB | 10 ms | button debounce + ADC + PSU UART poll |
| `task_display` | 4 | 4 KB | 50 ms | OLED render + LED update |
| `task_network` | 3 | 12 KB | 100 ms | WiFi + HTTP server |
| `task_ota` | 2 | 16 KB | event | esp_https_ota |
| `task_monitor` | 1 | 2 KB | 10 s | heap + stack watermark logging |

### Key Design Decisions vs V2

| V2 Problem | V3 Solution |
|------------|-------------|
| `ChargerLogic.cpp` = god object, all tasks depend on it | `tes_sm.c` is pure-functional; tasks communicate only through IPC objects |
| `hal_update_leds()` opens NVS every 50 ms | `config_svc` loads NVS once → RAM cache; all callers use `config_svc_get()` |
| Dual mutex (`canDataMutex` + `displayDataMutex`) | Single `g_snapshot_mutex` (small struct copy) + event bus |
| `Arduino String` in PSU controller | `char[]` + `snprintf` in `psu_driver.c` |
| `LuxBeacon` directly coupled to ChargerLogic | `led_driver.c` is a self-contained state machine, driven by `task_display` |
| `Adafruit_ADS1115` library | Direct ADS1115 I2C register access in `adc_driver.c` |

### Known Accepted Trade-offs (V2 behaviour preserved in V3)

- **No current ramp limiting** — PSU hardware handles it
- **Insulation test is a stub** — always passes
- **CP reads DC voltage** via ADS1115, not standard PWM waveform decoding
- **`esChargeSequenceNumber` hardcoded to 18**

---

## V2 Architecture (for reference)

### Module Breakdown

- **`ChargerLogic/`** — Core TES-0D-02-01 state machine (god object, 740 lines). Reads NVS settings directly.
- **`CAN_Protocol/`** — Parses 0x500/0x501/0x5F0, sends 0x508/0x509/0x5F8. Uses ESP32 TWAI on GPIO 17/18.
- **`PowerSupplyController/`** — UART text protocol (`V=xx.x,I=xx.x`) using Arduino String.
- **`HAL/`** — GPIO, ADS1115 ADC (voltage divider + CP line), relay control.
- **`UI/`** — u8g2 OLED + four buttons: START (up), STOP (down), EMERGENCY (hard stop), SETTING (menu).
- **`NetworkServices/`** — AsyncWebServer + WiFiManager, REST API.
- **`OTAManager/`** — GitHub releases OTA with `RTC_DATA_ATTR` step tracking.
- **`LuxBeacon/`** — LED pulse pattern encoding SOC value.

### CAN Message IDs (both V2 and V3)

| ID | Direction | Content |
|----|-----------|---------|
| 0x500 | Vehicle → Charger | Status, faults, requested current/voltage |
| 0x501 | Vehicle → Charger | SOC, max charge time, ETA |
| 0x5F0 | Vehicle → Charger | Emergency flags |
| 0x508 | Charger → Vehicle | Charger status, available voltage/current |
| 0x509 | Charger → Vehicle | Actual output voltage/current, remaining time |
| 0x5F8 | Charger → Vehicle | Emergency stop |

### V2 Hardware Config (`src/config.h`)

GPIO: buttons (39–42), LEDs (5–7), relays (9–11), CAN (17/18), I2C SDA/SCL (16/15), PSU UART (43/44). ADS1115 at 0x48. Voltage divider: 348 kΩ / 12 kΩ (120 V range). CP divider: 150 Ω / 51 Ω.

---

## Safety Notes

- CAN frames sent to the vehicle **can damage the BMS** — validate all `tes_codec.c` encode functions carefully before testing on hardware.
- The emergency stop path must never block — `atomic_bool g_emergency_stop` is checked every 10 ms tick regardless of queue state.
- `g_snapshot_mutex` acquire timeout is 5 ms; callers must handle failure gracefully (skip render, don't block).

---

## V3 Implementation Plan & Progress

### Implementation Order

| # | 項目 | 狀態 |
|---|------|------|
| 1 | CMake 專案骨架 + `sdkconfig.defaults` | ✅ 完成 |
| 2 | `tes_protocol/tes_types.h` — 所有型別定義 | ✅ 完成 |
| 3 | `tes_protocol/tes_codec.c` — CAN encode/decode | ✅ 完成 |
| 4 | `tes_protocol/tes_sm.c` — 純函數式狀態機 | ✅ 完成 |
| 5 | `hal/` + `platform/esp32/` — GPIO、I2C、UART、NVS | ✅ 完成 |
| 6 | `drivers/can_driver` — TWAI | ✅ 完成 |
| 7 | `drivers/adc_driver` — ADS1115 I2C register 操作 | ✅ 完成 |
| 8 | `drivers/psu_driver` — UART 文字協議（無 Arduino String） | ✅ 完成 |
| 9 | `drivers/display_driver` — u8g2 ESP-IDF callback 模式 | ✅ 完成（骨架） |
| 10 | `drivers/led_driver` — LuxBeacon 狀態機整合 | ✅ 完成 |
| 11 | `services/event_bus` — FreeRTOS queue-based | ✅ 完成 |
| 12 | `services/config_svc` — NVS 集中管理 + RAM 快取 | ✅ 完成 |
| 13 | `services/display_svc` — OLED 狀態畫面 | ✅ 完成（骨架，menu 畫面待實作） |
| 14 | `services/network_svc` — WiFi + HTTP server | ✅ 完成（骨架，captive portal 待實作） |
| 15 | `services/ota_svc` — ESP-IDF OTA | ✅ 完成 |
| 16 | `main/` 任務骨架 — 全部 task_*.c | ✅ 完成 |
| 17 | `idf.py build` 確認零編譯錯誤 | ⬜ 待執行 |
| 18 | `display_svc` menu 畫面（電壓/電流/SOC 設定） | ⬜ 待實作 |
| 19 | `network_svc` captive portal + REST `/config` `/start` `/stop` | ⬜ 待實作 |
| 20 | 端對端整合測試（完整 TES 充電流程） | ⬜ 待測試 |

### 目前骨架結構（所有檔案已建立）

```
v3/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions_16MB.csv
├── components/
│   ├── tes_protocol/       ✅ tes_types.h, tes_codec.c/.h, tes_sm.c/.h
│   ├── platform/           ✅ platform.h, platform_esp32.c
│   ├── hal/                ✅ hal_gpio.c/.h, hal_i2c.c/.h, hal_uart.c/.h, hal_nvs.c/.h
│   ├── drivers/            ✅ can/adc/psu/display/led _driver.c/.h
│   └── services/           ✅ event_bus, config_svc, display_svc, network_svc, ota_svc
└── main/
    ├── globals.h            ← IPC 物件宣告（queues, snapshot, atomic flag）
    ├── main.c               ← HAL/driver/service init + task spawn
    ├── task_can_rx.c        ← TWAI 接收 → g_can_rx_queue
    ├── task_tes_sm.c        ← 組裝 inputs → tes_sm_tick → 執行 outputs → 更新 snapshot
    ├── task_hal_poll.c      ← 按鈕去抖 + ADC 輪詢（交錯讀取）+ PSU UART poll
    ├── task_display.c       ← 50ms 呼叫 display_svc_tick
    ├── task_network.c       ← 100ms 事件處理
    ├── task_ota.c           ← event_bus 訂閱，等待 OTA 事件
    └── task_monitor.c       ← 10s heap + stack watermark 報告
```

### 已知待處理項目

- `display_driver.c`：u8g2 ESP-IDF I2C callback 實作需在硬體上驗證（u8g2 component 需加入 CMake）
- `display_svc.c`：menu 畫面（DISP_SCREEN_MENU）尚未實作，目前 fallback 到狀態畫面
- `network_svc.c`：captive portal、REST `/config` POST handler 尚未實作
- `task_hal_poll.c`：ADC 以交錯方式讀取（每 50ms 讀 CP，每 100ms 讀電壓），避免單 tick 超出 10ms 預算
- `tes_sm.c` ENDING 狀態：`relay_open_delay_ms` 理想上應移入 `tes_sm_t` struct（目前為 local static，功能正確但非純函數式）
