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

**Partition table (`partitions_16MB.csv`):** SPIFFS removed (was QuickJS legacy). App partitions maximised for hardware variant growth:

| Partition | Size | Notes |
|-----------|------|-------|
| nvs | 20 KB | config + charge history blob |
| otadata | 8 KB | OTA slot selector |
| app0 | **7.94 MB** | active firmware (~15% used) |
| app1 | **7.94 MB** | OTA update slot |

Changing the partition table requires a full reflash (bootloader + partition-table + app); OTA-only update is not sufficient after a layout change.

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
| `task_scheduler` | 2 | 4 KB | 30 s | NTP sync (pool.ntp.org, UTC+8) + charging window edge detection → g_btn_event_queue (v3.4.0) |
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

### Current Status: v3.0.0 released (2026-05-02). 全部 49 項功能已實作，`idf.py build` 零錯誤（2026-05-09 確認）。v3.1.0 / v3.2.0 / v3.3.0 / v3.4.0 / Beta 自動充電功能均已實作，**尚未接上車輛進行完整充電流程測試**。

**2026-05-07 追加修改（未計入版本號）：**
- `partitions_16MB.csv`：移除 SPIFFS（QuickJS 時代遺留），app0/app1 各擴充至 7.94 MB（原 4 MB），韌體目前佔 15%
- `charge_session_t`：新增 `stop_voltage_v` 欄位（12→16 bytes）；`stop_reason_t` 新增 `STOP_REASON_VOLTAGE=6`，SOC/電壓/計時停止原因現可明確區分
- 充電紀錄 Web UI：電壓停止顯示「55.2 V」、計時停止顯示「計時完成」、SOC 停止顯示「SOC 達標」
- **Bug fix**：Timer 模式 Web UI 狀態列原本因 BMS 回報 `remaining_seconds=0` 而隱藏；改為從 `elapsed_seconds` / `charge_timer_min` 計算，顯示「已充時間：0h05m / 2h00m」格式
- **Bug fix**：`charge_timer_min` 最小值驗證從 10 分鐘降為 1 分鐘（`network_svc.c`、`config_svc.c`、`display_svc.c`、`web/index.html`）；原本設 < 10 分鐘會被靜默丟棄、維持舊值，導致計時停止無法在短時間內測試

**2026-05-09 追加修改（未計入版本號）：**
- **新功能**：v3.4.0 定時充電（`scheduler_svc`），NTP UTC+8 + 每日充電窗口邊緣偵測，自動 Start/Stop；`sched_enabled/start/stop_en/stop` 存 NVS；OLED ON/OFF 開關；Web UI 時間選擇器；`/status` 回傳 `ntp_synced`/`local_time`
- **新功能**：Beta 自動充電（`auto_start`，NVS key `auto_s`）；IDLE 時 VP 繼電器常通；偵測 CP OFF→ON 邊緣（CP 比 CAN 更早出現）或 CAN 0x500 bit0 上升邊緣（0→1）自動觸發 PARAM_EXCHANGE；充電中 CP 斷開：auto_start 模式 = 正常停止（`enter_ending`），手動模式 = 故障；OLED `[Beta]Auto: ON/OFF`；Web UI 含安全說明；`/config` GET/POST 支援
- **行為修改**：一般故障（`TES_STATE_FAULT`）10 秒後轉回 IDLE 時同步清除 `fault_latched`，實現自動復歸；硬體緊急停止鈕仍須手動 Reset Fault（不變）；車端 0x5F0 緊急仍 5 秒自動復歸（不變）
- **協定時序確認（TES-0D-02-01）：** `VP ON → CP ON → CAN bit0=1 → 充電 → CAN ends → CP OFF`；CP 先於 CAN 出現（可作首發觸發）、晚於 CAN 消失（CP 斷開才是真正結束訊號，充電中若先斷則為故障）；`charge_complete_latched` 清除時同步重置 `last_can_permit = false` 確保下次 CP/CAN 邊緣可正確偵測

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
| 50 | 定時充電：`scheduler_svc`，NTP 同步（UTC+8）+ 窗口進出偵測自動 Start/Stop；`sched_enabled/start/stop_en/stop` 存 NVS；OLED ON/OFF 開關；Web UI 設定卡（時間選擇器）；`/status` 回傳 `ntp_synced`/`local_time` | DONE ⚠️ 尚未測試 |
| 51 | Beta 自動充電：`auto_start` (NVS key `auto_s`)；IDLE 時 VP 繼電器常通；偵測 CP OFF→ON 邊緣（CP 先於 CAN 出現）或 CAN 0x500 bit0 上升邊緣（0→1）自動進入 PARAM_EXCHANGE；充電中 CP 斷開：auto_start = 正常停止（`enter_ending`）、手動 = 故障；一般故障 10 秒自動復歸；硬體緊急停止仍須手動 Reset；充電完成後 CP 連續 2 tick OFF + `last_can_permit` 重置，才允許再次觸發；OLED `[Beta]Auto: ON/OFF`；Web UI（含安全說明）；`/config` GET/POST | DONE ⚠️ 尚未測試 |

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
| Charge Timer | 1 min -- 600 min（Stop Mode=Timer 時顯示） | ±10 min | ±30 min (auto-repeat) |
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

### Charge Timer Stop Feature (v3.3.0, DONE ⚠️ 尚未測試)

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

### Web UI Volt Mode Display Fix (v3.3.0, DONE ⚠️ 尚未測試)

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

### Scheduled Charging Feature (v3.4.0, DONE ⚠️ 尚未測試)

定時充電：設定每日充電時間窗口，到達開始時間自動 Start、到達結束時間自動 Stop（可選）。適合夜間離峰電價或定時補電場景。

**Config 新增欄位（`charger_config_t`）：**
```c
bool     sched_enabled;    // NVS key "sched_en"      master switch (default false)
uint16_t sched_start_min;  // NVS key "sched_start"   minutes from midnight 0-1439 (default 0 = 00:00)
bool     sched_stop_en;    // NVS key "sched_stop_en" auto-stop switch (default false)
uint16_t sched_stop_min;   // NVS key "sched_stop"    minutes from midnight 0-1439 (default 360 = 06:00)
```
API: `config_svc_set_scheduler(bool enabled, uint16_t start_min, bool stop_en, uint16_t stop_min)`

**scheduler_svc 架構：**

- Init: 呼叫 `esp_netif_sntp_init`；設定時區 `setenv("TZ", "CST-8", 1); tzset()`；若 WiFi 已連線則立即啟動 SNTP，否則訂閱 event_bus 等 `EVT_WIFI_CHANGED` 再啟動
- SNTP server: `pool.ntp.org`（ESP-IDF 內建 `esp_netif` SNTP 支援，無需額外元件）
- Task 每 30 s 喚醒一次；NTP 未同步時 skip 所有觸發邏輯
- **邊緣偵測（edge crossing）機制：** 追蹤 `last_minute`（上次執行時的分鐘數 0-1439）
  - 每次喚醒：取 `current_minute = (hour*60 + minute)`，若與 `last_minute` 不同則掃描是否有 start_min / stop_min 落在 `(last_minute, current_minute]` 區間
  - 若偵測到 start_min 在區間內 → 送 `EVT_BUTTON_START` 至 `g_btn_event_queue`
  - 若 `sched_stop_en` 且 stop_min 在區間內 → 送 `EVT_BUTTON_STOP` 至 `g_btn_event_queue`
  - 更新 `last_minute = current_minute`
- **隔夜窗口支援**（start_min > stop_min，例如 23:00–06:00）：crossing check 分兩段：`[last_minute+1, 1439]` ∪ `[0, current_minute]`（當 last_minute > current_minute 時代表跨過午夜）
- **NTP 首次同步後（`first_sync`）：** 若當前時刻已在窗口內（`is_in_window(current_minute, start_min, stop_min)`）→ 立即送 START，避免重開機後漏觸發；`first_sync = false` 之後轉為純邊緣偵測
- SM 已處理重複 START 指令（IDLE 以外的狀態忽略），不需要在此檢查當前狀態

**`/status` JSON 新增欄位：**
```json
"ntp_synced": true,
"local_time": "2026-05-09 14:30:00"
```

**`/config` GET/POST 新增欄位：**
```json
"sched_enabled": false,
"sched_start_min": 1380,
"sched_stop_en": true,
"sched_stop_min": 360
```

**OLED 設定選單：**
- 新增 `MENU_ITEM_SCHEDULER`（ON/OFF toggle），插入 WiFi Info 之後、Reset Fault 之前
- 選單總項目數：13 可設定項（+Save & Cancel）

**Web UI 設定卡（「定時充電」）：**
- Master ON/OFF checkbox（`sched_enabled`）
- 開始充電：`<input type="time">` → 存為 `sched_start_min`（分鐘數）
- 自動結束 checkbox（`sched_stop_en`）
- 結束時間：`<input type="time">`（`sched_stop_en` ON 時才顯示）
- 狀態列：「NTP：已同步（2026-05-09 14:30）」/ 「NTP：等待同步…」（從 `/status` 的 `ntp_synced` + `local_time` 取得）
- 提示文字：「隔夜窗口（如 23:00–06:00）自動支援」

**Public API（`scheduler_svc.h`）：**
```c
esp_err_t scheduler_svc_init(void);
void      task_scheduler(void *arg);
void      scheduler_svc_get_time_info(bool *synced_out, char *buf, size_t buflen);
          // buf → "2026-05-09 14:30:00" 或 "---" (未同步)
```
`network_svc.c` 的 `/status` handler 直接呼叫 `scheduler_svc_get_time_info()`，不需擴充 `tes_snapshot_t`。

**Files:**
- `firmware/components/services/include/services/scheduler_svc.h`
- `firmware/components/services/scheduler_svc.c`

**CMake（`services/CMakeLists.txt`）：**
- SRCS 加入 `scheduler_svc.c`
- REQUIRES 加入 `"lwip"`（SNTP 屬於 lwip stack，`esp_netif` 已在 REQUIRES 中）

**main.c 修改：**
- `#include "services/scheduler_svc.h"`
- `scheduler_svc_init()` 在 `network_svc_init()` 之後呼叫（WiFi stack 需先就緒）
- `xTaskCreate(task_scheduler, "sched", 4096, NULL, 2, NULL)`

**Task table 新增：**

| Task | Priority | Stack | Period | Role |
|------|----------|-------|--------|------|
| `task_scheduler` | 2 | 4 KB | 30 s | NTP sync + charging window edge detection → g_btn_event_queue |

**NVS key 一覽：**

| Key | Type | Default | 說明 |
|-----|------|---------|------|
| `sched_en` | uint8 (bool) | 0 | master switch |
| `sched_start` | uint16 | 0 | start time (minutes from midnight) |
| `sched_stop_en` | uint8 (bool) | 0 | auto-stop switch |
| `sched_stop` | uint16 | 360 | stop time (minutes from midnight) |

---

### Beta Auto-Start Feature (DONE ⚠️ 尚未測試)

VP 繼電器常通 + CAN 邊緣自動觸發充電流程，適合固定充電位置不需手動按鍵的場景。

**TES-0D-02-01 協定時序（已確認）：**
```
VP ON → CP ON → CAN 0x500 bit0 = 1 → 充電進行 → CAN 結束 → CP OFF
```
- VP 不通電時 CP 完全沒有訊號
- CP 比 CAN 更早出現，可作為最早的觸發條件（插槍後 CP 第一個有訊號）
- 充電結束時 CAN 先停，CP 最後才消失；充電中若 CP 意外斷開則為異常故障

**Config 新增欄位（`charger_config_t`）：**
```c
bool auto_start;   // NVS key "auto_s"; default false
```

**`tes_sm_inputs_t` 新增：**
```c
bool auto_start_enabled;  // Beta: VP 常通 + CAN 邊緣自動觸發充電
```

**`tes_sm_t` 新增邊緣偵測欄位：**
```c
cp_state_t cp_prev;        // 上次 CP 更新前的狀態（用於 CP 邊緣偵測及 charge_complete_latched 清除）
bool       last_can_permit; // 上一 tick 的 CAN 許可位元（偵測 0→1 邊緣）
```

**SM 行為（IDLE 狀態）：**
- `auto_start_enabled=true` → `out->vp_relay = true`（VP 常通）
- **觸發條件：CP OFF→ON 邊緣**（`cp_state==ON && cp_prev!=ON`）**或 CAN 0x500 bit0 上升邊緣**（`status_flags&0x01 && !last_can_permit`）→ 自動進入 PARAM_EXCHANGE；CP 比 CAN 更早，通常先觸發
- 一般故障（`TES_STATE_FAULT`）10 秒後自動復歸並清除 `fault_latched`，auto_start 可再次觸發
- 充電完成後（`charge_complete_latched=true`）：CP 連續兩次讀取為 OFF **且** `last_can_permit` 重置 → 清除 latch，允許下次觸發

**充電中 CP 斷開處理（`run_monitoring()`）：**
```c
if (sm->cp_state != CP_STATE_ON) {
    if (in->auto_start_enabled)
        enter_ending(sm, out);  // auto_start 模式：正常停止（車端主動中斷）
    else
        enter_fault(sm, out);   // 手動模式：CP 斷開屬非預期異常
    return;
}
```

**CP 更新與邊緣偵測：**
- CP 每 50ms 更新一次；更新前先存 `sm->cp_prev = sm->cp_state`（用於觸發邊緣偵測及雙 tick 穩定判斷）
- `last_can_permit` 在 `tes_sm_tick()` 末尾每 tick 更新

**安全特性：**
- 一般故障：10 秒後自動清除 `fault_latched` 並回到 IDLE，auto_start 可再觸發
- 硬體緊急停止鈕：`fault_latched=true` 且 `emergency_hw_triggered=true`，必須手動 Reset Fault 才能解除
- 充電完成後須 CP 穩定消失（2 tick OFF）+ `last_can_permit` 重置才允許下一次觸發
- 緊急停止後 VP 仍關閉（`enter_emergency` 強制 `out->vp_relay = false`）
- FAULT 狀態下 VP 仍保持通電（`out->vp_relay = in->auto_start_enabled`），讓車端在故障復歸後可重新發出訊號

**NVS key：** `auto_s`（bool）

**OLED 選單：** 在 Scheduler 之後顯示 `[Beta]Auto: ON/OFF`

**Web UI：** 在定時充電設定區塊之後，「儲存設定」之前，含 Beta 標籤與中文安全說明。安全說明：「一般故障 10 秒後自動復歸；按下緊急停止鈕後仍需手動 Reset Fault；充電完成後 CP 穩定斷開才允許再次觸發。」

**`/config` GET/POST：** 欄位 `auto_start`（bool）

---

### Scheduler + Auto-Start Integration (Planned, pending hardware testing)

當定時充電（`sched_enabled`）與 Beta 自動充電（`auto_start`）同時開啟時，兩功能需要協調，否則在充電窗口外 CP/CAN 邊緣仍可能意外觸發充電。

**問題：** VP 常通且槍一直插著時，充電結束後若 CP/CAN 未完全消失，scheduler 的 START 如何在下個窗口再次觸發？

**解法：** Scheduler 透過 `g_btn_event_queue` 送 `EVT_BUTTON_START`，此路徑走 SM 的**手動 START 路徑**，會主動清除 `charge_complete_latched`（不依賴 CP 斷開或 CAN 邊緣），因此不需要重插槍。

**額外安全閘：** 窗口外的 CP/CAN 邊緣不應觸發充電（避免車端自行發出訊號導致非預期充電）。

**實作計畫（實作前需先完成 auto_start 硬體測試）：**

1. **`tes_sm_inputs_t` 新增欄位：**
   ```c
   bool in_charging_window;  // true = 目前在定時充電窗口內（或未啟用定時）
   ```

2. **SM IDLE auto_start 觸發加閘：**
   ```c
   if (in->auto_start_enabled && !sm->fault_latched && !sm->charge_complete_latched
       && in->in_charging_window) {                  // ← 新增閘
       bool cp_edge  = (sm->cp_state == CP_STATE_ON && sm->cp_prev != CP_STATE_ON);
       bool can_edge = (in->vehicle_status.status_flags & 0x01) && !sm->last_can_permit;
       if (cp_edge || can_edge) { ... 觸發 PARAM_EXCHANGE ... }
   }
   ```

3. **`run_monitoring()` 窗口結束中止充電（可選）：**
   ```c
   if (in->auto_start_enabled && in->sched_enabled && !in->in_charging_window) {
       enter_ending(sm, out);  // 窗口結束時自動停止
       return;
   }
   ```
   （此邏輯與 `sched_stop_en` + scheduler 送 STOP 互補；若兩者同時啟用，scheduler STOP 先到則已停止，這裡是最後保護）

4. **`scheduler_svc` 新增 API：**
   ```c
   bool scheduler_svc_is_in_window(void);
   // 回傳目前是否在充電窗口內（sched_enabled=false 時恆回傳 true）
   ```

5. **`task_tes_sm.c` 填入輸入：**
   ```c
   inputs.in_charging_window = (!cfg->sched_enabled || !cfg->auto_start)
                                ? true
                                : scheduler_svc_is_in_window();
   ```
   - 兩功能都未啟用 → `true`（不限制）
   - 只有 auto_start 啟用（無排程）→ `true`
   - 兩者都啟用 → 由 scheduler 判斷

**Scheduler START/STOP 事件繼續保留**（不依賴 SM 自動偵測，仍由 scheduler 在時間邊緣主動送 START/STOP），`in_charging_window` 只是 CP/CAN 邊緣觸發的額外防護。

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
| GET | `/history` | JSON 陣列，最近 20 次充電紀錄（由新到舊）：duration_s, energy_wh, energy_estimated, stop_voltage_v, soc_start, soc_end, stop_reason |
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
    STOP_REASON_NORMAL  = 0,  // SOC 目標達成
    STOP_REASON_USER    = 1,  // user stop button / remote stop
    STOP_REASON_FAULT   = 2,
    STOP_REASON_EMERG   = 3,
    STOP_REASON_BMS     = 4,  // BMS revoked permission
    STOP_REASON_TIMER   = 5,  // 計時到達
    STOP_REASON_VOLTAGE = 6,  // 電壓目標達成
} stop_reason_t;

typedef struct {
    uint32_t duration_s;       // session length in seconds
    float    energy_wh;        // Wh delivered (V×I × 10ms / 3600000)
    float    stop_voltage_v;   // output voltage at stop (STOP_REASON_VOLTAGE only, else 0)
    uint8_t  soc_start;
    uint8_t  soc_end;
    uint8_t  stop_reason;      // stop_reason_t
    uint8_t  energy_estimated; // 1 = PSU not connected; energy calculated from ADC
} charge_session_t;            // 16 bytes — fits in 24-byte event payload
```

`task_tes_sm` sets `stop_reason` based on `inputs.stop_mode` when `charge_complete`:
- `STOP_MODE_SOC` → `STOP_REASON_NORMAL`
- `STOP_MODE_VOLTAGE` → `STOP_REASON_VOLTAGE` + captures `snap.output_voltage` into `stop_voltage_v`
- `STOP_MODE_TIMER` → `STOP_REASON_TIMER`

`energy_estimated = 1` when PSU UART was never seen during the session (ADC-based energy estimate).

**Energy formula (per tick):** `energy_wh += V * A / 360000.0f` (10 ms tick)
Only accumulated when `state == TES_STATE_CHARGING && timer_running && V > 0 && A > 0`.

**`EVT_SESSION_COMPLETE`** added to `event_type_t`; payload carries `charge_session_t` (16 bytes).

**log_svc storage:** NVS namespace `"tes_hist"`, blob key `"log"`, 324-byte circular buffer (head + count + pad + 20 × `charge_session_t`). Blob size change (was 248) clears old history on first boot after upgrade.

**New files:** `log_svc.h`, `log_svc.c` in `firmware/components/services/`.
**Task:** priority 1, stack 3 KB; subscribes to event bus.
**main.c:** call `log_svc_init()` after `config_svc_init()`.

**`GET /history` response:**
```json
[{"duration_s":5400,"energy_wh":1.23,"energy_estimated":false,"stop_voltage_v":0,"soc_start":45,"soc_end":95,"stop_reason":0}, ...]
```
Returned newest-first; max 20 entries.

**Web UI:** "充電紀錄" section at bottom of page; columns: 充電時間, 電量(Wh, 附 `(預估)` 標記), SOC起→終, 停止條件。停止條件欄依 stop_reason 顯示：SOC 達標 / `55.2 V`（電壓模式） / 計時完成 / 手動停止 / 故障 / 緊急停止 / BMS 停止。

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
