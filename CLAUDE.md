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
task_tes_sm   --[event_bus]--------------------> task_ota / future subscribers
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
| `task_monitor` | 1 | 4 KB | 10 s | heap + stack watermark logging |

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

### Current Status: OTA upload + UI improvements added (2026-05-02). V3 is feature-complete. Push tag `v3.x.x` to trigger automated build and release.

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
| mDNS | `tes-charger.local` registered after STA connects |

#### Known differences vs V2 for hardware testing

| Item | V2 | V3 current |
|------|-----|------------|
| OTA | GitHub release pull | POST /ota + Web UI + GitHub Actions auto-release |
| BLE | Partial implementation | Not implemented (out of scope for V3) |

### Settings Menu

11 items, accessible by **long-pressing SETTING** from the status screen.
**Short-pressing SETTING** on the status screen cycles the quick SOC preset: 80 % → 95 % → 100 % → 80 % (saves to NVS immediately).

| Item | Range | Short press step | Long press step |
|------|-------|-----------------|-----------------|
| Auto Volt | ON / OFF | toggle | toggle |
| Max Voltage | 40.0 V -- 120.0 V (顯示為 "V Cap" when Auto ON) | ±0.1 V | ±1 V (auto-repeat) |
| Max Current | 1.0 A -- 100.0 A | ±0.1 A | ±1 A (auto-repeat) |
| Stop Mode | SOC / Volt | toggle | toggle |
| Target SOC | 20 % -- 100 %（Stop Mode=SOC 時顯示） | ±1 % | ±5 % (auto-repeat) |
| Stop Voltage | 40.0 V -- 120.0 V（Stop Mode=Volt 時顯示） | ±0.1 V | ±1 V (auto-repeat) |
| LuxBeacon | ON / OFF | toggle | toggle |
| WiFi Info | read-only display | — | — |
| Reset Fault | 手動復歸緊急停止 | confirm | — |
| Save & Exit | writes to NVS | — | — |
| Cancel | discard changes | — | — |

WiFi Info shows: `AP: 192.168.4.1` (AP mode) / `IP: x.x.x.x` (STA connected) / `WiFi: ---` (disconnected).

OLED status screen:
- **SOC 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72/95%  1h23m`
- **Volt 模式**：第二行 `54.2V/100.0V`（即時/目標電壓），第三行 `SOC:72%  1h23m`

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

- Voltage source: PSU reported voltage; ADC fallback if PSU not reporting
- NVS keys: `stop_m` (uint32 0/1), `stop_v` (uint32, 0.1V/bit, default 1000 = 100.0V)
- SM check in `run_monitoring()` in `tes_sm.c`
- Configurable via OLED menu (Stop Mode + Stop Voltage items), REST API, web UI

### Known Technical Debt (correct behaviour, not clean design)

- `TES_STATE_ENDING`: `relay_open_delay_ms` is a local static inside the case block; should move to `tes_sm_t` struct to make SM fully re-entrant
- `check_battery_compatibility`: voltage limit logic needs validation against real vehicle CAN data

### Release 流程

1. 確認程式碼正確後 push tag：`git tag v3.0.0 && git push origin v3.0.0`
2. GitHub Actions (`.github/workflows/build-v3.yml`) 自動：
   - 用 ESP-IDF v5.5.1 編譯 V3
   - 生成合併 binary (`idf.py merge-bin`)
   - 建立 GitHub Release，附上：
     - `tes_charger.bin` → Web UI OTA 更新用
     - `tes_charger_flash.bin` → GitHub Pages 首次燒錄用

**新增硬體變體**：在 `build-v3.yml` 的 `matrix.include` 加一行，並在 `firmware/` 為新硬體建立對應 Kconfig/driver。

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
| GET | `/config` | JSON config: auto_voltage, max_voltage, max_current, target_soc, stop_mode, stop_voltage, wifi_ssid, beacon |
| POST | `/config` | Partial update JSON body (any subset of fields); WiFi changes require reboot |
| POST | `/start` | Sends `EVT_BUTTON_START` to `g_btn_event_queue` |
| POST | `/stop` | Sends `EVT_BUTTON_STOP` to `g_btn_event_queue` |
| POST | `/ota` | 從 URL 下載韌體（JSON body `{"url":"..."}` 可選）；省略則用 GitHub Releases 預設 URL |
| POST | `/ota/upload` | 手動上傳韌體 binary（`application/octet-stream`）；需 Content-Length；進度透過 `/status` 查詢 |

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
```
