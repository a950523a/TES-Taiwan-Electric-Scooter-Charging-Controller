# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 firmware for a DC charging controller compatible with the **TES-0D-02-01** standard used by Taiwan electric scooters (e.g., eMoving iE125). The controller bridges an external PSU to a vehicle's BMS via CAN bus, with a web UI, OLED display, and OTA updates.

Firmware lives in `firmware/` (ESP-IDF, pure C99). V2 PlatformIO code has been removed.

License: CC BY-NC-SA 4.0 (non-commercial).

---

## Build (ESP-IDF)

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`sdkconfig.defaults` enables PSRAM (OPI 8MB), DIO 80 MHz flash, TWAI, USB CDC console, FreeRTOS 1 kHz tick.

**Build environment (PowerShell, Windows):**
```powershell
$env:IDF_PATH = "C:\Users\user\esp\v5.5.1\esp-idf"
$toolsDir = "C:\Users\user\.espressif\tools"
$env:PATH = "$toolsDir\cmake\3.30.2\bin;C:\Users\user\.espressif\python_env\idf5.5_py3.11_env\Scripts;$toolsDir\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;" + $env:PATH
python "$env:IDF_PATH\tools\idf.py" build
```
Note: ESP-IDF cmake (3.30.2) must come before STM32CubeCLT cmake in PATH.

**u8g2 dependency:** Tracked as a **git submodule** at `firmware/components/u8g2/` (points to olikraus/u8g2 on GitHub; source files are NOT committed to this repo). A thin ESP-IDF wrapper at `firmware/components/u8g2_idf/CMakeLists.txt` is committed and wires the submodule's `csrc/` into the build. On a fresh clone run:
```bash
git submodule update --init
```

---

## V3 Architecture

### Guiding Principle

`tes_protocol/` is **zero-dependency C99** -- no ESP-IDF, no FreeRTOS, no OS calls. It can be compiled on any platform (STM32, PC unit tests, etc.) by swapping `charger_hal/` and `platform/`. All time, GPIO, and CAN operations are injected by the caller.

### Component Layers

```
main/           <- FreeRTOS tasks + global IPC objects
services/       <- event_bus, config_svc, display_svc, network_svc, ota_svc
drivers/        <- can_driver, adc_driver, psu_driver, display_driver, led_driver
u8g2_idf/       <- ESP-IDF CMakeLists.txt wrapper only (committed); references u8g2/ submodule
u8g2/           <- git submodule → olikraus/u8g2 (source NOT committed to this repo)
charger_hal/    <- hal_gpio, hal_i2c, hal_uart, hal_nvs  (ESP-IDF wrappers)
platform/       <- platform_tick_ms() -- the only PAL function tes_protocol needs
tes_protocol/   <- tes_types.h, tes_codec.c, tes_sm.c  (portable, zero OS deps)
```

Note: component is named `charger_hal` (not `hal`) to avoid conflict with ESP-IDF's built-in `hal` component.

### State Machine Design (`tes_protocol/tes_sm.c`)

```c
void tes_sm_tick(tes_sm_t *sm, const tes_sm_inputs_t *in, tes_sm_outputs_t *out);
```

- **Inputs** are assembled by `task_tes_sm` each tick: CAN frames from queue, ADC values from globals, PSU status from driver, button events from queue, `tick_ms` from `platform_tick_ms()`.
- **Outputs** represent the **desired steady state** of all hardware each tick (`relay_on`, `coupler_lock`, `vp_relay`, PSU setpoints, CAN TX flags) -- executed by `task_tes_sm` after the tick, never inside the SM.
- The SM never reads time or touches hardware directly.

States: `IDLE -> PARAM_EXCHANGE -> PRE_CHARGE -> CHARGING -> ENDING -> FAULT / EMERGENCY / FINALIZE`

### IPC Between Tasks

```
task_can_rx   --[can_frame_t queue depth=16]--> task_tes_sm
task_hal_poll --[g_btn_event_queue depth=8]---> task_tes_sm       (START/STOP when menu closed)
task_hal_poll --[g_display_btn_queue depth=8]-> task_display      (SETTING always; START/STOP when menu open)
task_hal_poll --[g_emergency_stop atomic_bool]> task_tes_sm       (every tick, bypasses menu gate)
task_hal_poll --[g_adc_cp_voltage / g_adc_output_voltage volatile]-> task_tes_sm
task_tes_sm   --[g_snapshot + g_snapshot_mutex]--> task_display / task_network
task_tes_sm   --[event_bus]--------------------> task_ota / task_notify (v3.1.0) / task_log (v3.1.0) / task_mqtt (v3.2.0)
task_mqtt     --[g_btn_event_queue depth=8]----> task_tes_sm       (remote cmd: start/stop via MQTT)
display_svc   --[g_menu_open volatile bool]----> task_hal_poll    (gates button routing)
```

### Task Table

| Task | Priority | Stack | Period | Role |
|------|----------|-------|--------|------|
| `task_can_rx` | 15 | 2 KB | event | TWAI receive -> raw queue |
| `task_tes_sm` | 12 | 4 KB | 10 ms | SM tick + output execution + snapshot update |
| `task_hal_poll` | 10 | 2 KB | 10 ms | button debounce + ADC + PSU UART poll |
| `task_display` | 4 | 4 KB | 50 ms | OLED render + LED update |
| `task_network` | 3 | 12 KB | 100 ms | WiFi + HTTP server |
| `task_ota` | 2 | 16 KB | event | esp_https_ota |
| `task_notify` | 2 | 6 KB | event | push notification via webhook (v3.1.0) |
| `task_mqtt` | 2 | 8 KB | event + 10/30 s | MQTT publish status + subscribe cmd (v3.2.0) |
| `task_monitor` | 1 | 4 KB | 10 s | heap + stack watermark logging |
| `task_log` | 1 | 3 KB | event | charge session history to NVS (v3.1.0) |

### Button Routing

Long press threshold = 500 ms; auto-repeat every 100 ms while held (START/STOP only).

| Button | Press type | Menu closed | Menu open (NAV) | Menu open (EDIT) |
|--------|------------|-------------|-----------------|------------------|
| START | short | `g_btn_event_queue` → TES SM (start) | scroll up | fine +0.1 (V/A/1%) |
| START | long/repeat | — | — | coarse +1 (V/A/5%) auto-repeat |
| STOP | short | `g_btn_event_queue` → TES SM (stop) | scroll down | fine −0.1 |
| STOP | long/repeat | — | — | coarse −1 auto-repeat |
| SETTING | short | cycle quick SOC preset (80→95→100→80) | confirm / enter edit | confirm edit, back to NAV |
| SETTING | long | open settings menu | — | — |
| EMERGENCY | short | `g_emergency_stop` atomic_bool (always, never gated) | same | same |

### Key Design Decisions vs V2

| V2 Problem | V3 Solution |
|------------|-------------|
| `ChargerLogic.cpp` = god object, all tasks depend on it | `tes_sm.c` is pure-functional; tasks communicate only through IPC objects |
| `hal_update_leds()` opens NVS every 50 ms | `config_svc` loads NVS once -> RAM cache; all callers use `config_svc_get()` |
| Dual mutex (`canDataMutex` + `displayDataMutex`) | Single `g_snapshot_mutex` (small struct copy) + event bus |
| `Arduino String` in PSU controller | `char[]` + `snprintf` in `psu_driver.c` |
| `LuxBeacon` directly coupled to ChargerLogic | `led_driver.c` is a self-contained state machine, driven by `task_display` |
| `Adafruit_ADS1115` library | Direct ADS1115 I2C register access in `adc_driver.c` |

### Known Accepted Trade-offs (V2 behaviour preserved in V3)

- **No current ramp limiting** -- PSU hardware handles it
- **Insulation test is a stub** -- always passes
- **CP reads DC voltage** via ADS1115, not standard PWM waveform decoding
- **`esChargeSequenceNumber` hardcoded to 18**

---

## V2 Architecture (for reference)

### Module Breakdown

- **`ChargerLogic/`** -- Core TES-0D-02-01 state machine (god object, 740 lines). Reads NVS settings directly.
- **`CAN_Protocol/`** -- Parses 0x500/0x501/0x5F0, sends 0x508/0x509/0x5F8. Uses ESP32 TWAI on GPIO 17/18.
- **`PowerSupplyController/`** -- UART text protocol (`V=xx.x,I=xx.x`) using Arduino String.
- **`HAL/`** -- GPIO, ADS1115 ADC (voltage divider + CP line), relay control.
- **`UI/`** -- u8g2 OLED + four buttons: START (up), STOP (down), EMERGENCY (hard stop), SETTING (menu).
- **`NetworkServices/`** -- AsyncWebServer + WiFiManager, REST API.
- **`OTAManager/`** -- GitHub releases OTA with `RTC_DATA_ATTR` step tracking.
- **`LuxBeacon/`** -- LED pulse pattern encoding SOC value.

### CAN Message IDs (both V2 and V3)

| ID | Direction | Content |
|----|-----------|---------|
| 0x500 | Vehicle -> Charger | Status, faults, requested current/voltage |
| 0x501 | Vehicle -> Charger | SOC, max charge time, ETA |
| 0x5F0 | Vehicle -> Charger | Emergency flags |
| 0x508 | Charger -> Vehicle | Charger status, available voltage/current |
| 0x509 | Charger -> Vehicle | Actual output voltage/current, remaining time |
| 0x5F8 | Charger -> Vehicle | Emergency stop |

### V2 Hardware Config (`src/config.h`)

GPIO: buttons (39-42), LEDs (5-7), relays (9-11), CAN (17/18), I2C SDA/SCL (16/15), PSU UART (43/44). ADS1115 at 0x48. Voltage divider: 348 kOhm / 12 kOhm (120 V range). CP divider: 150 Ohm / 51 Ohm.

---

## Safety Notes

- CAN frames sent to the vehicle **can damage the BMS** -- validate all `tes_codec.c` encode functions carefully before testing on hardware.
- The emergency stop path must never block -- `atomic_bool g_emergency_stop` is checked every 10 ms tick regardless of queue state.
- `g_snapshot_mutex` acquire timeout is 5 ms; callers must handle failure gracefully (skip render, don't block).

---

## V3 Implementation Plan & Progress

### Current Status: v3.0.0 released (2026-05-02). v3.1.0 in progress：所有功能已實作，**尚未接上車輛進行充電流程測試**。v3.2.0 in progress：MQTT 遠端監控 + Cloud PWA + 診斷功能已實作，尚未測試。

### Implementation Progress

| # | Item | Status |
|---|------|--------|
| 1 | CMake skeleton + `sdkconfig.defaults` | DONE |
| 2 | `tes_protocol/tes_types.h` | DONE |
| 3 | `tes_protocol/tes_codec.c` -- CAN encode/decode | DONE |
| 4 | `tes_protocol/tes_sm.c` -- pure-functional state machine | DONE |
| 5 | `charger_hal/` + `platform/esp32/` -- GPIO, I2C, UART, NVS | DONE |
| 6 | `drivers/can_driver` -- TWAI | DONE |
| 7 | `drivers/adc_driver` -- ADS1115 direct I2C register access | DONE |
| 8 | `drivers/psu_driver` -- UART text protocol, 3s disconnect timeout | DONE |
| 9 | `drivers/display_driver` -- u8g2 SSD1306 128x64, I2C callback via hal_i2c | DONE |
| 10 | `drivers/led_driver` -- LuxBeacon state machine | DONE |
| 11 | `services/event_bus` -- FreeRTOS queue-based | DONE |
| 12 | `services/config_svc` -- NVS with RAM cache, namespace "tes_cfg" | DONE |
| 13 | `services/display_svc` -- status screen + settings menu | DONE |
| 14 | `services/network_svc` -- WiFi STA/AP + REST API + mDNS + embedded web UI | DONE |
| 15 | `services/ota_svc` -- esp_https_ota | DONE |
| 16 | `main/` tasks -- all 7 tasks, full button routing | DONE |
| 17 | `idf.py build` zero errors | DONE |
| 18 | Hardware flash + TES charging flow test | DONE |
| 19 | u8g2 integration + full OLED display | DONE |
| 20 | SETTING button + display menu | DONE |
| 21 | REST API -- GET /status, GET /config, POST /config, POST /start, POST /stop | DONE |
| 22 | AP mode (no SSID → "TES-Charger" open AP) + mDNS `tes-charger.local` | DONE |
| 23 | Embedded web UI -- single-page app, 1s live polling, dark theme | DONE |
| 24 | OTA: `POST /ota` REST API + Web UI 進度顯示 + GitHub Releases 預設 URL | DONE |
| 25 | 5F0 post-charge emergency auto-recovery (5s timeout + `emergency_hw_triggered` tracking) | DONE |
| 26 | GitHub Actions matrix build + Release (`tes_charger.bin` + `tes_charger_flash.bin`) | DONE |
| 27 | GitHub Pages 首次燒錄工具 (`docs/index.html` + `docs/manifest.json`, ESP Web Tools) | DONE |
| 28 | Auto-voltage: ADC 開機一次讀取 + 1s 延遲 + 龜 logo 畫面 + OLED 選單 + Web UI 開關 | DONE |
| 29 | Stop mode: SOC / 電壓二選一停止 + OLED 選單 + REST API + Web UI radio + NVS 保存 | DONE |
| 30 | Stop mode UI: OLED 選單依模式只顯示 Target SOC 或 Stop Voltage；Web UI 同步隱藏另一欄 | DONE |
| 31 | OTA 手動上傳：`POST /ota/upload` + Web UI 檔案選擇器；進度透過 `/status` polling 顯示 | DONE |
| 32 | OLED 狀態畫面：Volt 模式顯示即時/目標電壓，SOC 模式顯示 SOC 現在/目標 | DONE |
| 33 | Web UI 停止條件選單排序：充電停止條件 radio 移至 Target SOC / Stop Voltage 之前（與 OLED 一致） | DONE |
| 34 | GitHub Pages 燒錄工具 CORS 修正：manifest 改相對路徑 `./tes_charger_flash.bin`；Actions 部署 binary，Pages source 設為 GitHub Actions | DONE |
| 35 | Webhook / ntfy 推播通知：`notify_svc`，充電開始/完成/故障時 HTTP POST JSON；URL 存 NVS，Web UI 設定 | DONE ⚠️ 尚未接車測試 |
| 36 | PWA 升級：`manifest.json` + `sw.js` + `icon.svg`；Apple homescreen meta tags；Web UI 可安裝至手機桌面 | DONE ⚠️ 尚未接車測試 |
| 37 | 充電紀錄 + Wh 統計：`log_svc` 最近 20 次記錄存 NVS；`GET /history`；Web UI 紀錄表格 | DONE ⚠️ 尚未接車測試 |
| 38 | WiFi 掃描：`GET /wifi/scan` 回傳附近 AP；Web UI 掃描按鈕 + dropdown 點選填入 SSID | DONE ⚠️ 尚未接車測試 |
| 39 | mDNS AP 模式修正：`WIFI_EVENT_AP_START` 時也啟動 mDNS，`tes-charger.local` 在 AP/STA 兩種模式皆有效 | DONE ⚠️ 尚未接車測試 |
| 40 | Web UI 手機 WiFi 切換按鈕：iOS `App-prefs:root=WIFI` / Android `intent:#Intent;action=...WIFI_SETTINGS` | DONE ⚠️ iOS Safari 已知問題：`App-prefs:root=WIFI` 在 Safari 顯示「網址無效」，無法跳轉 WiFi 設定 |
| 41 | PWA 離線修正：`Cache-Control: max-age=86400, stale-if-error=604800`；`sw.js` 改 network-first + 離線 fallback；`poll()` 離線顯示「離線」狀態 | DONE ⚠️ 尚未接車測試 |
| 42 | OLED 設定選單 + Web UI：新增韌體版本 / 作者欄（`MENU_ITEM_ABOUT`，唯讀；Web UI OTA 卡片底部） | DONE ⚠️ 尚未接車測試 |
| 43 | MQTT 遠端監控：`mqtt_svc`，publish 狀態、subscribe 指令；空 URL = 停用；Web UI 設定卡 + 連結；`GET /mqtt/link` | DONE ⚠️ 尚未測試 |
| 44 | Cloud PWA 遠端監控：GitHub Pages `docs/monitor.html`，MQTT.js WebSocket，URL fragment 自動帶入設定，可安裝至手機桌面 | DONE ⚠️ 尚未測試 |
| 45 | 即時功率 + 累積電量：`power_w` / `energy_wh` 加入 snapshot 及 `/status`；PSU 未接時 Web UI 顯示 `(預估)` | DONE ⚠️ 尚未測試 |
| 46 | CAN 診斷面板：Web UI 摺疊卡顯示全部 6 條 CAN 訊息解碼值（0x500/501/5F0 RX、0x508/509/5F8 TX），附方向標籤與 bit 指示燈 | DONE ⚠️ 尚未測試 |
| 47 | 本機 build 版本號：`CMakeLists.txt` 以 `git describe --always --tags --dirty` 設定 `PROJECT_VER`；本機 dev build 顯示完整 commit hash，tag release 顯示 tag 名稱 | DONE |
| 48 | 充電倒數計時停止：`STOP_MODE_TIMER`；`charge_timer_min` 設定充電時長上限（NVS key `timer_m`）；OLED 選單 + Web UI；SM `run_monitoring()` 檢查 elapsed ≥ timer；OLED 狀態第三行顯示已充時間/目標時長 | DONE ⚠️ 尚未測試 |
| 49 | Web UI Volt 模式顯示修正：充電停止條件為「依電壓」時，Web UI 電壓欄改顯示「即時電壓 / 目標停止電壓」（與 OLED 一致），隱藏目標 SOC 欄 | DONE ⚠️ 尚未測試 |

### V3 vs V2 Feature Parity for Hardware Testing

#### Features that work and match V2

| Feature | Notes |
|---------|-------|
| TES state machine | All 8 states, correct transitions, CP debounce, timer -- verified with eMoving iE125 |
| CAN codec | 0x500/501/5F0 decode, 0x508/509/5F8 encode, same byte order as V2 |
| PSU UART control | `V=xx.x,I=xx.x` + `CMD_ACK:` frames handled; ADC fallback when PSU in standby |
| Relay / coupler lock / VP relay | GPIO direct control, each tick outputs desired steady state |
| ADS1115 ADC | Differential AIN0-1 (voltage), AIN2-3 (CP), same divider ratios |
| LuxBeacon LED pattern | Same SOC pulse-count encoding as V2 LuxBeacon.cpp |
| Button debounce | START / STOP / EMERGENCY / SETTING, 3-tick stable (30ms) |
| Emergency stop | atomic_bool, checked every 10ms tick, bypasses queue and menu gate |
| Config NVS | Load-once RAM cache; fixes V2's NVS-open-every-50ms bug |
| OLED status screen | State name, voltage/current, SOC, elapsed time, target config |
| OLED settings menu | Auto Volt / Max Voltage / Max Current / Target SOC / Stop Mode / Stop Voltage / LuxBeacon / WiFi Info / Reset Fault / Save / Cancel |
| SETTING button | Long press opens menu; short press cycles SOC (80→95→100→80) |
| WiFi / REST API | AP mode when unconfigured; STA mode when SSID set; full REST API |
| Web UI | Embedded SPA at `GET /`, live 1s polling, start/stop/config controls |
| mDNS | `tes-charger.local` registered in both AP and STA mode (AP: maps to 192.168.4.1) ⚠️ 尚未測試 |

#### Known differences vs V2 for hardware testing

| Item | V2 | V3 current |
|------|-----|------------|
| OTA | GitHub release pull | POST /ota + Web UI + GitHub Actions auto-release |
| BLE | Partial implementation | Not implemented (out of scope for V3) |

### Settings Menu

12 items, accessible by **long-pressing SETTING** from the status screen.
**Short-pressing SETTING** on the status screen cycles the quick SOC preset: 80 % → 95 % → 100 % → 80 % (saves to NVS immediately).

| Item | Range | Short press step | Long press step |
|------|-------|-----------------|-----------------|
| Auto Volt | ON / OFF | toggle | toggle |
| Max Voltage | 40.0 V -- 120.0 V (顯示為 "V Cap" when Auto ON) | ±0.1 V | ±1 V (auto-repeat) |
| Max Current | 1.0 A -- 100.0 A | ±0.1 A | ±1 A (auto-repeat) |
| Stop Mode | SOC / Volt / Timer | toggle | toggle |
| Target SOC | 20 % -- 100 %（Stop Mode=SOC 時顯示） | ±1 % | ±5 % (auto-repeat) |
| Stop Voltage | 40.0 V -- 120.0 V（Stop Mode=Volt 時顯示） | ±0.1 V | ±1 V (auto-repeat) |
| Charge Timer | 10 min -- 600 min（Stop Mode=Timer 時顯示） | ±10 min | ±30 min (auto-repeat) |
| LuxBeacon | ON / OFF | toggle | toggle |
| WiFi Info | read-only display | — | — |
| Reset Fault | 手動復歸緊急停止 | confirm | — |
| About | 韌體版本 + 作者（唯讀） | — | — |
| Save & Exit | writes to NVS | — | — |
| Cancel | discard changes | — | — |

WiFi Info shows: `AP: 192.168.4.1` (AP mode) / `IP: x.x.x.x` (STA connected) / `WiFi: ---` (disconnected).

OLED status screen:
- **SOC 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72/95%  1h23m`
- **Volt 模式**：第二行 `54.2V/100.0V`（即時/目標電壓），第三行 `SOC:72%  1h23m`
- **Timer 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72%  1h23m/2h00m`（已充時間/目標充電時長）

### PSU UART Protocol Notes (discovered during hardware testing 2026-05-01)

The PSU sends two types of frames on UART:
- **`V=xx.x,I=xx.x`** -- actual measured output voltage/current; only sent when PSU is actively outputting power
- **`CMD_ACK:SET_V:xx.x` / `CMD_ACK:SET_I:xx.x`** -- acknowledgement of SET commands; sent in standby/idle and when a SET command is received

`psu_driver.c` handles both: `V=I` frames update `psu_voltage`/`psu_current`; `CMD_ACK:` frames only mark PSU as connected (no V/I data). When `psu_voltage == 0` (PSU standby), the SM falls back to ADC measured voltage for both OLED display and 0x509 CAN output. This prevents the vehicle from seeing 0V/0A and aborting charging.

### Auto-Voltage Feature (ADDED 2026-05-02)

When `auto_voltage = true` in config:
1. `app_main` draws the boot logo (turtle mark + "Auto Setting Voltage...") on the OLED
2. Waits `AUTO_VOLT_SETTLE_MS = 1000 ms` for the ADC to stabilise
3. Reads `adc_driver_read_voltage()` once; if 40–120 V, calls `config_svc_override_voltage()` (RAM only — NVS not written)
4. Continues normal startup; the overridden value flows into the SM via `inputs.max_voltage_01v` every tick

NVS key `auto_v` (bool). Controllable via OLED menu item "Auto Volt" and web UI checkbox.

### Stop Mode Feature (ADDED 2026-05-02)

Two mutually-exclusive charging termination conditions (selected by user):

| Mode | `stop_mode_t` value | Condition |
|------|---------------------|-----------|
| SOC (default) | `STOP_MODE_SOC = 0` | `BMS SOC >= target_soc` |
| Voltage | `STOP_MODE_VOLTAGE = 1` | `output_voltage >= stop_voltage_01v / 10.0f` |
| Timer | `STOP_MODE_TIMER = 2` | `charge_elapsed_ms >= charge_timer_min * 60000` |

- Voltage source: PSU reported voltage; ADC fallback if PSU not reporting
- NVS keys: `stop_m` (uint32 0/1/2), `stop_v` (uint32, 0.1V/bit, default 1000 = 100.0V), `timer_m` (uint32, minutes, default 120)
- SM check in `run_monitoring()` in `tes_sm.c`
- Configurable via OLED menu (Stop Mode + Stop Voltage / Charge Timer items), REST API, web UI

### Charge Timer Stop Feature (TODO v3.3.0)

充電倒數計時停止：使用者設定最長充電時間，到時自動停止，適合無法確認 SOC 或不想等到滿電的場景。

**Config 新增欄位（`charger_config_t`）：**
```c
uint32_t charge_timer_min;  // NVS key "timer_m"; range 10–600 min; default 120
```

**`stop_mode_t` 新增：**
```c
STOP_MODE_TIMER = 2,  // stop when charge_elapsed_ms >= charge_timer_min * 60000
```

**SM 修改（`tes_sm.c` `run_monitoring()`）：**
```c
case STOP_MODE_TIMER:
    if (sm->charge_elapsed_ms >= (uint32_t)inputs->charge_timer_min * 60000u)
        transition(sm, TES_STATE_ENDING);
    break;
```
`charge_elapsed_ms` 已存在於 `tes_sm_t`（charging 計時器），無需新增欄位。

**OLED 狀態畫面（Timer 模式）：**
- 第二行：`54.2V  12.3A`（同 SOC 模式）
- 第三行：`SOC:72%  1h23m/2h00m`（已充時間 / 目標時長，格式與 SOC 模式目標 SOC 對齊）

**OLED 設定選單：**
- Stop Mode 值循環改為 `SOC → Volt → Timer → SOC`
- Stop Mode = Timer 時顯示「Charge Timer」項目（隱藏 Target SOC 和 Stop Voltage）；±10 min 短按，±30 min 長按 auto-repeat

**Web UI：**
- Stop Mode radio 新增「依時間」選項
- 選「依時間」時隱藏 Target SOC / Stop Voltage 欄，顯示 Charge Timer 輸入框（分鐘，10–600）
- `/status` JSON 加入 `charge_timer_min`；`/config` GET/POST 支援 `charge_timer_min`

**`/status` JSON 新增欄位：**
```json
"charge_timer_min": 120
```

**NVS key：** `timer_m`（uint32，minutes）

---

### Web UI Volt Mode Display Fix (TODO v3.3.0)

**問題（Bug）：** 充電停止條件選「依電壓」時，Web UI 狀態區塊仍顯示「目標 SOC」，與 OLED 行為不一致，會誤導用戶以為 SOC 是停止條件。

**OLED 正確行為（已實作）：**
- Volt 模式：第二行 `54.2V/100.0V`（即時電壓 / 目標停止電壓），第三行 `SOC:72%  1h23m`（SOC 純參考，無目標）

**Web UI 修正目標（需實作）：**

| Stop Mode | 電壓顯示 | SOC 顯示 |
|-----------|----------|----------|
| SOC | `54.2 V`（單值） | `72% / 95%`（即時/目標） |
| Volt | `54.2 V / 100.0 V`（即時/目標） | `72%`（純參考，無目標） |
| Timer | `54.2 V`（單值） | `72%`（純參考，無目標） |

**修改範圍（`firmware/components/services/web/index.html`）：**
- `poll()` 回呼中，依 `data.stop_mode` 切換電壓欄標籤與值格式：
  - `stop_mode == 1`（Volt）：電壓欄顯示 `${voltage} V / ${stop_voltage} V`，標籤改為「充電電壓 / 目標電壓」
  - 其他模式：電壓欄顯示 `${voltage} V`，標籤維持「充電電壓」
- SOC 欄：
  - `stop_mode == 0`（SOC）：顯示 `${soc}% / ${target_soc}%`，標籤「SOC / 目標」
  - 其他模式：顯示 `${soc}%`，標籤「SOC」
- `/status` JSON 須包含 `stop_mode`（整數 0/1/2）和 `stop_voltage`（float，V）；若目前 `/status` 未回傳這兩個欄位，需在 `network_svc.c` snapshot JSON 中補上

---

### Known Technical Debt (correct behaviour, not clean design)

- `TES_STATE_ENDING`: `relay_open_delay_ms` is a local static inside the case block; should move to `tes_sm_t` struct to make SM fully re-entrant
- `check_battery_compatibility`: voltage limit logic needs validation against real vehicle CAN data

### Release 流程

1. 確認程式碼正確後 push tag：`git tag v3.x.x && git push origin v3.x.x`
2. GitHub Actions (`.github/workflows/build-v3.yml`) 自動：
   - 用 ESP-IDF v5.5.1 編譯 V3
   - 生成合併 binary (`idf.py merge-bin`)
   - 建立 GitHub Release（stable tag），附上：
     - `tes_charger.bin` → Web UI OTA 更新用
     - `tes_charger_flash.bin` → GitHub Pages 首次燒錄用
   - 部署 GitHub Pages（含最新 `tes_charger_flash.bin`）

**GitHub Pages 燒錄工具注意事項：**
- `docs/manifest.json` 使用相對路徑 `./tes_charger_flash.bin`；binary 由 Actions 打包進 Pages，不存入 git
- Pages source 必須設為 **GitHub Actions**（repo Settings → Pages → Source）
- Push main 或 stable tag 時 Actions 自動重新部署 Pages（含最新 binary）
- `docs/index.html` 載入 `esp-web-tools@10` 使用 jsDelivr CDN（`cdn.jsdelivr.net`）

**新增硬體變體**：在 `build-v3.yml` 的 `matrix.include` 加一行，並在 `firmware/` 為新硬體建立對應 Kconfig/driver。只有 `variant == 'esp32s3-ssd1306'` 負責上傳 Pages artifact（避免 matrix 多次上傳衝突）。

### OTA 更新流程

- **首次燒錄（V2→V3 或全新）**：前往 `https://a950523a.github.io/TES-Taiwan-Electric-Scooter-Charging-Controller/`，USB 連接後一鍵燒錄
- **後續更新（V3→V3）**：
  - **自動**：Web UI 點「更新至最新韌體」，設備從 GitHub Releases 拉取最新 `tes_charger.bin`，完成後自動重啟
  - **手動**：Web UI 點「上傳韌體」，選擇本地 `.bin` 檔案；或 `curl --data-binary @tes_charger.bin http://tes-charger.local/ota/upload`

### 5F0 Post-Charge Emergency Behaviour (FIXED 2026-05-02)

eMoving iE125 sends 0x5F0 after every normal charge end. The fix in `tes_sm.c`:
- `emergency_hw_triggered` (field in `tes_sm_t`) tracks source: `true` = hardware button, `false` = vehicle CAN
- **Hardware button**: EMERGENCY requires manual fault-clear (Reset Fault menu), no auto-timeout
- **Vehicle 0x5F0**: EMERGENCY auto-recovers to IDLE after 5 s; `fault_latched` cleared → LED shows COMPLETE (not FAULT)
- **Re-entry guard**: in IDLE with `charge_complete_latched=true`, further vehicle 0x5F0 is ignored

### Network & Web UI

**WiFi modes:**
- No SSID in NVS → AP mode, SSID `TES-Charger` (open), IP `192.168.4.1`
- SSID configured → STA mode, auto-reconnect, mDNS `tes-charger.local` after got-IP

**REST API (port 80, CORS *):**

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Embedded web UI (HTML/CSS/JS, ~9 KB) |
| GET | `/status` | JSON snapshot: state, voltage, current, soc, target_soc, timer, fault, wifi, ip |
| GET | `/config` | JSON config: auto_voltage, max_voltage, max_current, target_soc, stop_mode, stop_voltage, wifi_ssid, beacon, notify_url |
| POST | `/config` | Partial update JSON body (any subset of fields); WiFi changes require reboot |
| POST | `/start` | Sends `EVT_BUTTON_START` to `g_btn_event_queue` |
| POST | `/stop` | Sends `EVT_BUTTON_STOP` to `g_btn_event_queue` |
| POST | `/ota` | 從 URL 下載韌體（JSON body `{"url":"..."}` 可選）；省略則用 GitHub Releases 預設 URL |
| POST | `/ota/upload` | 手動上傳韌體 binary（`application/octet-stream`）；需 Content-Length；進度透過 `/status` 查詢 |
| GET | `/history` | JSON 陣列，最近 20 次充電紀錄（由新到舊）：duration_s, energy_wh, soc_start, soc_end, stop_reason |
| GET | `/manifest.json` | PWA Web App Manifest |
| GET | `/sw.js` | Service Worker（離線快取 index.html） |
| GET | `/icon.svg` | App 圖示（SVG，用於 homescreen） |
| GET | `/wifi/scan` | 掃描附近 WiFi AP，回傳最多 20 筆（ssid, rssi, secured）⚠️ 尚未測試 |
| POST | `/notify/test` | 立即發送測試推播到已設定的 notify_url；AP 模式或未連線時回傳錯誤 ⚠️ 尚未測試 |
| GET | `/mqtt/link` | 回傳 Cloud PWA URL（含 broker+topic fragment）供 Web UI 產生 QR code |

**Initial WiFi setup** (AP mode): connect to `TES-Charger`, open `http://192.168.4.1`, use web UI Settings or:
```bash
curl -X POST http://192.168.4.1/config \
  -H "Content-Type: application/json" \
  -d '{"wifi_ssid":"MySSID","wifi_pass":"password"}'
```
Reboot to connect to the configured AP. After connecting, use `http://tes-charger.local`.

**CMake notes for embedded web UI:**
- HTML is embedded via `EMBED_TXTFILES "web/index.html"` in `services/CMakeLists.txt`
- Symbol name uses **filename only** (not path): `_binary_index_html_start` / `_binary_index_html_end`
- mDNS uses managed component `espressif/mdns` declared in `services/idf_component.yml`; CMakeLists REQUIRES entry is `espressif__mdns` (double underscore)

### Webhook / ntfy Push Notification Feature (DONE ⚠️ 尚未接車測試)

`notify_svc` subscribes to event bus, POSTs ntfy-compatible JSON when charging state changes.

**Trigger events:**
- `TES_STATE_CHARGING` entered → "充電開始"
- `TES_STATE_IDLE` with `charge_complete=true` → "充電完成" (includes SOC, elapsed time from snapshot)
- `TES_STATE_FAULT` → "充電故障"
- `TES_STATE_EMERGENCY` → "緊急停止"

**AP mode behaviour:** `network_svc_is_connected()` checked before every send; no HTTP attempt in AP mode or when STA disconnected (avoids 5s timeout).

**Config:** `notify_url[128]` in `charger_config_t`; NVS key `"notify_url"` in `"tes_cfg"`.
- `notify_url` = `"https://ntfy.sh/{topic}"` → ntfy mode
- `notify_url` = any other URL → generic webhook
- `notify_url` = `""` → disabled

**JSON payload** (ntfy.sh compatible):
```json
{"title": "充電完成", "message": "SOC 95%  1h 23m", "priority": 3}
```

**Task:** priority 2, stack 6 KB. `esp_http_client`, 5 s timeout, no retry.

**Public API:** `notify_svc_send(url, title, message, priority)` — callable from REST handler.

**Web UI guided setup:**
- ntfy.sh 模式：自動產生 `tes-xxxxxxxxxx` 隨機 topic；提供「複製」「重新產生」「📲 訂閱」（`ntfy://` 深層連結）「📨 測試」按鈕；附 Android/iOS app 下載連結
- 自定義 Webhook 模式：直接輸入 URL
- 停用模式：儲存空字串

**Files:** `notify_svc.h`, `notify_svc.c` in `firmware/components/services/`.
**CMake:** `notify_svc.c` in SRCS; `esp_http_client` in REQUIRES.
**main.c:** `notify_svc_init()` after `config_svc_init()`; `xTaskCreate(task_notify, "notify", 6144, NULL, 2)`.

---

### PWA Upgrade (DONE ⚠️ 尚未接車測試)

Embedded web UI converted to installable PWA.

**Embedded files** (EMBED_TXTFILES in `services/CMakeLists.txt`):

| File | Endpoint | MIME | Cache-Control |
|------|----------|------|---------------|
| `web/manifest.json` | `GET /manifest.json` | `application/manifest+json` | max-age=3600 |
| `web/sw.js` | `GET /sw.js` | `application/javascript` | no-cache |
| `web/icon.svg` | `GET /icon.svg` | `image/svg+xml` | max-age=86400 |
| `web/index.html` | `GET /` | `text/html` | max-age=300 |

**sw.js strategy:** cache `/` on install; network-first for `/status`, `/config`, `/history`, `/wifi/scan`; cache-first for `/`.

**Note on HTTP limitation:** Service Worker requires HTTPS or localhost. On `http://tes-charger.local` (plain HTTP), SW registration is silently blocked by browsers — offline caching does not work. "Add to Home Screen" shortcut works on all platforms over HTTP.

**Consistent URL across modes:** mDNS now starts in both AP and STA mode → always use `http://tes-charger.local`, never install PWA from `http://192.168.4.1` (IP changes when device switches to STA).

**Additional Web UI features added in this pass:**
- `GET /wifi/scan`: active scan, returns up to 20 APs; Web UI scan button + dropdown
- 「切換手機 WiFi」button: iOS → `App-prefs:root=WIFI`; Android → `intent:#Intent;action=android.settings.WIFI_SETTINGS`
  - **已知問題：** iOS Safari 對 `App-prefs:root=WIFI` URL scheme 顯示「網址無效」錯誤，無法跳轉至 WiFi 設定。此 URL scheme 在 WKWebView（PWA 模式）可能有效，但在 Safari 瀏覽器內無效。待修正。
- Post-save WiFi guidance: when SSID + password saved, shows instruction to switch phone WiFi and link to `tes-charger.local`

**CMake:** `max_uri_handlers = 16`; 13 handlers registered (14 after log_svc adds `GET /history`).

---

### Charge Session History + Wh Tracking (v3.1.0)

`task_tes_sm` accumulates V×I energy per 10 ms tick during CHARGING, then publishes a `EVT_SESSION_COMPLETE` event when the session ends. `log_svc` stores the last 20 sessions as an NVS blob.

**New types** (added to `tes_types.h`):
```c
typedef enum {
    STOP_REASON_NORMAL = 0,  // SOC / voltage target reached
    STOP_REASON_USER   = 1,  // user stop button / remote stop
    STOP_REASON_FAULT  = 2,
    STOP_REASON_EMERG  = 3,
    STOP_REASON_BMS    = 4,  // BMS revoked permission
    STOP_REASON_TIMER  = 5,
} stop_reason_t;

typedef struct {
    uint32_t duration_s;   // session length in seconds
    float    energy_wh;    // Wh delivered (V×I × 10ms / 3600000)
    uint8_t  soc_start;
    uint8_t  soc_end;
    uint8_t  stop_reason;  // stop_reason_t
    uint8_t  _pad;
} charge_session_t;        // 12 bytes — fits in 24-byte event payload
```

**Energy formula (per tick):** `energy_wh += V * A / 360000.0f` (10 ms tick)
Only accumulated when `state == TES_STATE_CHARGING && timer_running && V > 0 && A > 0`.

**`EVT_SESSION_COMPLETE`** added to `event_type_t`; payload carries `charge_session_t` (12 bytes).

**log_svc storage:** NVS namespace `"tes_hist"`, blob key `"log"`, 248-byte circular buffer (head + count + 20 × `charge_session_t`).

**New files:** `log_svc.h`, `log_svc.c` in `firmware/components/services/`.
**Task:** priority 1, stack 3 KB; subscribes to event bus.
**main.c:** call `log_svc_init()` after `config_svc_init()`.

**`GET /history` response:**
```json
[{"duration_s":5400,"energy_wh":1.23,"soc_start":45,"soc_end":95,"stop_reason":0}, ...]
```
Returned newest-first; max 20 entries.

**Web UI:** "充電紀錄" section at bottom of page; columns: 充電時間(分鐘), 電量(Wh), SOC起→終, 停止原因.

**hal_nvs blob:** if `hal_nvs_get_blob` / `hal_nvs_set_blob` not present in `charger_hal/hal_nvs.h`, add them wrapping `nvs_get_blob` / `nvs_set_blob`.

---

### MQTT Remote Monitoring (v3.2.0)

選配功能，`mqtt_broker_url == ""` 時完全不啟動連線。用戶設定後可在任何網路環境監控充電狀態並遠端 Start/Stop。

**Config 新增欄位（`charger_config_t`）：**
```c
char mqtt_broker_url[128];   // NVS key "mqtt_url"   e.g. "mqtt://broker.hivemq.com:1883"
char mqtt_topic_prefix[64];  // NVS key "mqtt_topic"  e.g. "tes/charger1"
```

**Publish（ESP32 → broker）：**

| Topic | 頻率 | 內容 |
|-------|------|------|
| `{prefix}/status` | CHARGING 時 10 s，其他 30 s；狀態改變時立即發 | 同 `GET /status` JSON |
| `{prefix}/status` (LWT) | 連線中斷時由 broker 自動發 | `{"state":"offline"}` retained |

**Subscribe（broker → ESP32）：**

| Topic | Payload | 動作 |
|-------|---------|------|
| `{prefix}/cmd` | `{"cmd":"start"}` | 送 `EVT_BUTTON_START` 至 `g_btn_event_queue` |
| `{prefix}/cmd` | `{"cmd":"stop"}` | 送 `EVT_BUTTON_STOP` 至 `g_btn_event_queue` |

- QoS: publish QoS 0，cmd subscribe QoS 1
- ESP-IDF 元件: `esp-mqtt`（內建，CMakeLists REQUIRES 加 `"mqtt"`）
- Task: priority 2，stack 8 KB；訂閱 event_bus 取得狀態改變事件

**公共 broker 選項（供 Web UI 引導用戶選擇）：**
- `mqtt://broker.hivemq.com:1883`（免帳號，穩定，WS port 8000）
- `mqtt://test.mosquitto.org:1883`（免帳號，測試用，WS port 8080）
- 自定義（用戶填入，需自行確認 WS port）

**Web UI 設定卡（「遠端監控」）：**
- Broker URL 輸入框 + Topic Prefix 輸入框
- 儲存後顯示「取得遠端監控連結」按鈕 → 呼叫 `GET /mqtt/link` → 顯示 QR code + 複製連結

**`GET /mqtt/link` 回傳：**
```json
{"url": "https://a950523a.github.io/TES-Taiwan-Electric-Scooter-Charging-Controller/monitor.html#b=broker.hivemq.com&p=8000&t=tes%2Fcharger1"}
```

**Files:** `mqtt_svc.h`, `mqtt_svc.c` in `firmware/components/services/`.
**main.c:** `mqtt_svc_init()` after `config_svc_init()`；`xTaskCreate(task_mqtt, "mqtt", 8192, NULL, 2)`.

---

### Cloud PWA Remote Monitor (v3.2.0)

部署在 GitHub Pages 的靜態 HTML，完全 client-side，不需要任何後端。

**檔案：** `docs/monitor.html`

**技術：**
- MQTT.js via CDN (`unpkg.com/mqtt/dist/mqtt.min.js`)
- 透過 WebSocket 連線到 MQTT broker（瀏覽器不支援原生 TCP MQTT）
- Config 從 URL fragment 讀取：`#b=broker.hivemq.com&p=8000&t=tes/charger1`
  - `b` = broker hostname
  - `p` = WebSocket port
  - `t` = topic prefix
- 首次設定後存入 `localStorage`，之後直接開 `monitor.html` 即可（不需帶 fragment）

**功能：**
- 訂閱 `{prefix}/status` → 即時顯示狀態面板（state, voltage, current, SOC, timer）
- 發布 `{prefix}/cmd` → Start / Stop 按鈕（遠端控制）
- 連線狀態指示（connecting / connected / broker offline / device offline）
- 可安裝為 PWA（獨立 manifest）加到手機桌面
- GitHub Pages 為 HTTPS → Service Worker 可正常運作 → 離線快取有效

**用戶流程：**
1. 在 Device PWA（`http://tes-charger.local`）設定 MQTT broker + topic
2. 點「取得遠端監控連結」→ 掃描 QR code 或複製連結
3. 在任何網路環境打開連結 → 即時監控 + 遠端控制
4. 加到手機桌面 → 下次直接從桌面圖示開啟

**兩個 PWA 表面的分工：**

| | Device PWA (`tes-charger.local`) | Cloud PWA (GitHub Pages) |
|--|----------------------------------|--------------------------|
| 適用場景 | 同一個 WiFi | 任何地點 |
| 連線方式 | HTTP polling | MQTT WebSocket |
| 功能 | 完整設定 + 控制 + OTA | 即時狀態 + Start/Stop |
| SW 離線快取 | ❌ HTTP 限制 | ✅ HTTPS |
| 需要 MQTT 設定 | 否 | 是 |

**CMake:** `docs/monitor.html` 為靜態檔案，不需 embed；由 GitHub Actions 部署至 Pages 時一併包含。

---

### File Structure

```
firmware/
+-- CMakeLists.txt
+-- sdkconfig.defaults          (ASCII only -- Windows cp950 restriction)
+-- partitions_16MB.csv
+-- components/
|   +-- tes_protocol/           tes_types.h, tes_codec.c/.h, tes_sm.c/.h
|   +-- platform/               platform.h, platform_esp32.c
|   +-- charger_hal/            hal_gpio/i2c/uart/nvs .c/.h
|   +-- u8g2/                   git submodule (olikraus/u8g2); run `git submodule update --init`
|   +-- u8g2_idf/               CMakeLists.txt only -- wires u8g2/csrc into ESP-IDF build
|   +-- drivers/                can/adc/psu/display/led -- all complete
|   +-- services/               all services complete
|   |   +-- web/index.html      embedded web UI (EMBED_TXTFILES in services/CMakeLists.txt)
|   |   +-- web/manifest.json   PWA manifest
|   |   +-- web/sw.js           service worker
|   |   +-- web/icon.svg        app icon
|   |   +-- notify_svc.c/.h     push notification service (v3.1.0)
|   |   +-- log_svc.c/.h        charge session history (v3.1.0)
|   |   +-- mqtt_svc.c/.h       MQTT remote monitoring service (v3.2.0)
|   |   +-- idf_component.yml   declares espressif/mdns managed component
+-- main/
    +-- globals.h               IPC objects (queues, snapshot mutex, atomic, volatile ADC, menu flag)
    +-- main.c                  HAL/driver/service init + task spawn
    +-- task_can_rx.c           TWAI receive -> g_can_rx_queue
    +-- task_tes_sm.c           tes_sm_tick + execute_outputs + snapshot update
    +-- task_hal_poll.c         button debounce + interleaved ADC + PSU poll + menu routing
    +-- task_display.c          50ms: drain g_display_btn_queue + display_svc_tick
    +-- task_network.c          100ms WiFi status poller
    +-- task_ota.c              event_bus subscriber
    +-- task_monitor.c          10s heap report
    +-- idf_component.yml       also declares espressif/mdns (added by idf.py add-dependency)
    +-- web/index.html          copy of services/web/index.html (kept for reference)
docs/
+-- index.html              GitHub Pages 首次燒錄工具 (ESP Web Tools)
+-- manifest.json           燒錄工具 manifest（相對路徑 ./tes_charger_flash.bin）
+-- monitor.html            Cloud PWA 遠端監控（MQTT.js WebSocket，v3.2.0）
```
