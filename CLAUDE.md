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

**Partition table (`partitions_16MB.csv`):** SPIFFS removed. App partitions maximised:

| Partition | Size | Notes |
|-----------|------|-------|
| nvs | 20 KB | config + charge history blob |
| otadata | 8 KB | OTA slot selector |
| app0 | **7.94 MB** | active firmware (~15% used) |
| app1 | **7.94 MB** | OTA update slot |

Changing the partition table requires a full reflash (bootloader + partition-table + app); OTA-only is not sufficient.

**Build environment (PowerShell, Windows):**
```powershell
$env:IDF_PATH = "C:\Users\user\esp\v5.5.1\esp-idf"
$toolsDir = "C:\Users\user\.espressif\tools"
$env:PATH = "$toolsDir\cmake\3.30.2\bin;C:\Users\user\.espressif\python_env\idf5.5_py3.11_env\Scripts;$toolsDir\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;" + $env:PATH
python "$env:IDF_PATH\tools\idf.py" build
```
Note: ESP-IDF cmake (3.30.2) must come before STM32CubeCLT cmake in PATH.

**u8g2 dependency:** git submodule at `firmware/components/u8g2/`; source NOT committed. On a fresh clone:
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

## CAN Message IDs

| ID | Direction | Content |
|----|-----------|---------|
| 0x500 | Vehicle -> Charger | Status, faults, requested current/voltage |
| 0x501 | Vehicle -> Charger | SOC, max charge time, ETA |
| 0x5F0 | Vehicle -> Charger | Emergency flags |
| 0x508 | Charger -> Vehicle | Charger status, available voltage/current |
| 0x509 | Charger -> Vehicle | Actual output voltage/current, remaining time |
| 0x5F8 | Charger -> Vehicle | Emergency stop |

**V2 Hardware GPIO (for reference):** buttons (39-42), LEDs (5-7), relays (9-11), CAN (17/18), I2C SDA/SCL (16/15), PSU UART (43/44). ADS1115 at 0x48. Voltage divider: 348 kΩ / 12 kΩ (120 V range). CP divider: 150 Ω / 51 Ω.

---

## Safety Notes

- CAN frames sent to the vehicle **can damage the BMS** -- validate all `tes_codec.c` encode functions carefully before testing on hardware.
- The emergency stop path must never block -- `atomic_bool g_emergency_stop` is checked every 10 ms tick regardless of queue state.
- `g_snapshot_mutex` acquire timeout is 5 ms; callers must handle failure gracefully (skip render, don't block).

---

## Current Status

v3.0.0 released 2026-05-02. All 51 features implemented; `idf.py build` zero errors (confirmed 2026-05-09). v3.1.0–v3.4.0 and Beta auto-start implemented but **not yet tested with a vehicle**.

**Confirmed TES-0D-02-01 protocol timing (commit c7fa3f8):** `VP ON → CP ON → CAN 0x500 bit0=1 → charging → CAN ends → CP OFF`. CP appears before CAN; CP OFF→ON edge is the primary auto-start trigger, CAN rising edge is backup.

**⚠️ Not yet vehicle-tested:** notify_svc, PWA offline caching, log_svc, WiFi scan, mDNS AP mode, MQTT, Cloud PWA, power/energy tracking, CAN diagnostics panel, charge timer stop, scheduler, beta auto-start.

**Known issue:** iOS Safari `App-prefs:root=WIFI` URL scheme shows "Invalid URL" -- WiFi switch button does not work in Safari browser (may work in WKWebView/PWA mode).

---

## Settings Menu

Accessible by **long-pressing SETTING** from the status screen. **Short-pressing SETTING** cycles quick SOC preset: 80% → 95% → 100% → 80% (saves to NVS immediately).

| Item | Range | Short press step | Long press step |
|------|-------|-----------------|-----------------|
| Auto Volt | ON / OFF | toggle | toggle |
| Max Voltage | 40.0 V -- 120.0 V (顯示為 "V Cap" when Auto ON) | ±0.1 V | ±1 V (auto-repeat) |
| Max Current | 1.0 A -- 100.0 A | ±0.1 A | ±1 A (auto-repeat) |
| Stop Mode | SOC / Volt / Timer | toggle | toggle |
| Target SOC | 20% -- 100%（Stop Mode=SOC 時顯示） | ±1% | ±5% (auto-repeat) |
| Stop Voltage | 40.0 V -- 120.0 V（Stop Mode=Volt 時顯示） | ±0.1 V | ±1 V (auto-repeat) |
| Charge Timer | 1 min -- 600 min（Stop Mode=Timer 時顯示） | ±10 min | ±30 min (auto-repeat) |
| LuxBeacon | ON / OFF | toggle | toggle |
| WiFi Info | read-only display | — | — |
| Scheduler | ON / OFF | toggle | toggle |
| [Beta] Auto | ON / OFF | toggle | toggle |
| Reset Fault | 手動復歸緊急停止 | confirm | — |
| About | 韌體版本 + 作者（唯讀） | — | — |
| Save & Exit | writes to NVS | — | — |
| Cancel | discard changes | — | — |

OLED status screen:
- **SOC 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72/95%  1h23m`
- **Volt 模式**：第二行 `54.2V/100.0V`（即時/目標電壓），第三行 `SOC:72%  1h23m`
- **Timer 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72%  1h23m/2h00m`（已充時間/目標充電時長）

Web UI voltage/SOC display mirrors OLED: Volt mode shows `voltage / stop_voltage`; SOC mode shows `soc% / target_soc%`; Timer mode shows single voltage and reference SOC only.

---

## PSU UART Protocol

Two frame types from PSU:
- `V=xx.x,I=xx.x` -- actual output; only sent when PSU actively outputting
- `CMD_ACK:SET_V:xx.x` / `CMD_ACK:SET_I:xx.x` -- command ack; sent in standby

When `psu_voltage == 0` (PSU standby), SM falls back to ADC voltage for OLED display and 0x509 CAN output — prevents vehicle from seeing 0V/0A and aborting.

---

## NVS Key Reference

Config namespace `"tes_cfg"`. See `config_svc.c` for the full list; keys explicitly documented:

| NVS Key | Type | Default | Description |
|---------|------|---------|-------------|
| `auto_v` | bool | false | Auto-voltage: read ADC at boot, override max_voltage (RAM only) |
| `stop_m` | uint32 | 0 | Stop mode: 0=SOC, 1=Volt, 2=Timer |
| `stop_v` | uint32 | 1000 | Stop voltage × 10 (i.e. 100.0 V) |
| `timer_m` | uint32 | 120 | Charge timer (minutes, 1–600) |
| `notify_url` | str[128] | "" | ntfy/webhook URL; empty = disabled |
| `mqtt_url` | str[128] | "" | MQTT broker URL; empty = disabled |
| `mqtt_topic` | str[64] | "" | MQTT topic prefix |
| `sched_en` | uint8 | 0 | Scheduler master switch |
| `sched_start` | uint16 | 0 | Start time (minutes from midnight) |
| `sched_stop_en` | uint8 | 0 | Auto-stop enable |
| `sched_stop` | uint16 | 360 | Stop time (minutes from midnight, default 06:00) |
| `auto_s` | bool | false | Beta auto-start |

Charge history: namespace `"tes_hist"`, blob key `"log"` (324 bytes, 20 × `charge_session_t` circular buffer).

---

## Feature Summaries

### Auto-Voltage
NVS key `auto_v`. At boot: waits 1 s, reads ADC once, if 40–120 V overrides `max_voltage` in RAM (NVS not written). Boot logo shows "Auto Setting Voltage..." on OLED.

### Stop Mode
Three mutually exclusive termination conditions (NVS key `stop_m`). SM checks in `run_monitoring()`:
- `STOP_MODE_SOC=0`: BMS SOC ≥ target_soc
- `STOP_MODE_VOLTAGE=1`: output voltage ≥ stop_voltage_01v/10.0 (NVS key `stop_v`)
- `STOP_MODE_TIMER=2`: charge_elapsed_ms ≥ charge_timer_min × 60000 (NVS key `timer_m`)

Voltage source is PSU-reported; ADC fallback if PSU standby.

### 5F0 Post-Charge Emergency
eMoving iE125 sends 0x5F0 after every normal charge end. `emergency_hw_triggered` field in `tes_sm_t` distinguishes source:
- **Hardware button**: no auto-timeout; requires manual Reset Fault
- **Vehicle 0x5F0**: auto-recovers to IDLE after 5 s; `fault_latched` cleared → LED shows COMPLETE
- **Re-entry guard**: in IDLE with `charge_complete_latched=true`, further 0x5F0 is ignored

### Beta Auto-Start
NVS key `auto_s`. VP relay held ON in IDLE. Triggers `PARAM_EXCHANGE` on CP OFF→ON edge (primary) or CAN 0x500 bit0 rising edge (backup). Fields added to `tes_sm_t`: `cp_prev`, `last_can_permit`. CP updates every 50 ms; `cp_prev` saved before update for edge detection.

**CP disconnect during charging:**
- auto_start mode → `enter_ending()` (normal stop; vehicle initiated)
- manual mode → `enter_fault()` with 1 s auto-recovery timeout

**Fault auto-recovery (`fault_timeout_ms` in `tes_sm_t`):**

| Fault cause | Recovery |
|-------------|---------|
| CP disconnect (manual mode) | 1 s |
| Other faults (timeout, BMS flags, etc.) | 10 s |
| Hardware emergency button | No auto-recovery; requires Reset Fault |
| Vehicle 0x5F0 emergency | 5 s (separate path, unchanged) |

After charge complete: clears `charge_complete_latched` only when CP is OFF for 2 consecutive ticks **and** `last_can_permit` is reset. In FAULT: VP relay stays ON (`out->vp_relay = in->auto_start_enabled`) so vehicle can re-signal after recovery.

### Scheduled Charging
NVS keys: `sched_en`, `sched_start` (minutes from midnight 0-1439), `sched_stop_en`, `sched_stop`. NTP via `pool.ntp.org`, timezone CST-8. Task wakes every 30 s; uses edge-crossing detection on minute window to send `EVT_BUTTON_START`/`EVT_BUTTON_STOP` to `g_btn_event_queue`. Overnight windows (e.g. 23:00–06:00) supported. On first NTP sync: if already in window, fires START immediately. `/status` includes `ntp_synced` + `local_time` (populated by `scheduler_svc_get_time_info()`).

### Webhook / ntfy Push Notification
NVS key `notify_url` (empty = disabled). `notify_svc` subscribes to event bus, POSTs `{"title":"...", "message":"...", "priority":3}` on: CHARGING entered, IDLE with charge_complete, FAULT, EMERGENCY. Checks `network_svc_is_connected()` before every send. `POST /notify/test` sends a test notification.

### Charge Session History
`task_tes_sm` accumulates V×I during CHARGING (`energy_wh += V*A/360000.0f` per 10 ms tick). Publishes `EVT_SESSION_COMPLETE` with `charge_session_t` (16 bytes). `log_svc` stores last 20 sessions as NVS blob.

Key types in `tes_types.h`:
```c
typedef enum {
    STOP_REASON_NORMAL=0, STOP_REASON_USER=1, STOP_REASON_FAULT=2,
    STOP_REASON_EMERG=3, STOP_REASON_BMS=4, STOP_REASON_TIMER=5, STOP_REASON_VOLTAGE=6
} stop_reason_t;

typedef struct {
    uint32_t duration_s; float energy_wh; float stop_voltage_v;
    uint8_t soc_start, soc_end, stop_reason, energy_estimated;  // 16 bytes total
} charge_session_t;
```

`GET /history` returns newest-first JSON array (max 20 entries).

### MQTT Remote Monitoring
NVS keys: `mqtt_url` (empty = disabled), `mqtt_topic`. Publishes `{prefix}/status` every 10 s (CHARGING) or 30 s (other), immediately on state change. LWT: `{"state":"offline"}` retained. Subscribes `{prefix}/cmd` QoS 1 for `{"cmd":"start"/"stop"}` → `g_btn_event_queue`. Uses ESP-IDF `esp-mqtt` component. `GET /mqtt/link` returns Cloud PWA URL with broker/topic in fragment.

**Cloud PWA** (`docs/monitor.html`): MQTT.js via WebSocket, config from URL fragment (`#b=host&p=wsport&t=prefix`); stored in `localStorage` after first use. GitHub Pages (HTTPS) → Service Worker works → offline caching valid.

---

## Scheduler + Auto-Start Integration (Planned)

**Problem:** when both `sched_enabled` and `auto_start` are on, CP/CAN edges outside the charging window could trigger unintended charging.

**Planned solution:** add `bool in_charging_window` to `tes_sm_inputs_t`. Gate auto-start CP/CAN edge detection on this field. `scheduler_svc_is_in_window()` returns `true` when `sched_enabled=false`. Scheduler's `EVT_BUTTON_START` path (manual START route) clears `charge_complete_latched` directly — no re-plug needed.

**Implement after auto_start hardware testing.**

---

## Known Technical Debt

- `TES_STATE_ENDING`: `relay_open_delay_ms` is a local static inside the case block; should move to `tes_sm_t` for full re-entrancy
- `check_battery_compatibility`: voltage limit logic needs validation against real vehicle CAN data

---

## Network & Web UI

**WiFi modes:**
- No SSID in NVS → AP mode, SSID `TES-Charger` (open), IP `192.168.4.1`
- SSID configured → STA mode, auto-reconnect, mDNS `tes-charger.local` after got-IP

mDNS starts in both AP and STA mode; always use `tes-charger.local`.

**REST API (port 80, CORS *):**

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Embedded web UI |
| GET | `/status` | JSON snapshot: state, voltage, current, soc, target_soc, stop_mode, stop_voltage, timer, fault, wifi, ip, ntp_synced, local_time, power_w, energy_wh |
| GET | `/config` | JSON config: all `charger_config_t` fields |
| POST | `/config` | Partial update (any subset); WiFi changes require reboot |
| POST | `/start` | Sends `EVT_BUTTON_START` to `g_btn_event_queue` |
| POST | `/stop` | Sends `EVT_BUTTON_STOP` to `g_btn_event_queue` |
| POST | `/ota` | Pull firmware from URL (default: GitHub Releases latest) |
| POST | `/ota/upload` | Upload binary (`application/octet-stream`); progress via `/status` |
| GET | `/history` | Last 20 charge sessions (newest-first) |
| GET | `/manifest.json` | PWA manifest |
| GET | `/sw.js` | Service Worker |
| GET | `/icon.svg` | App icon |
| GET | `/wifi/scan` | Scan nearby APs (max 20: ssid, rssi, secured) |
| POST | `/notify/test` | Send test push notification |
| GET | `/mqtt/link` | Cloud PWA URL with broker/topic fragment |

**CMake notes for embedded web UI:**
- HTML embedded via `EMBED_TXTFILES "web/index.html"`; symbol `_binary_index_html_start` / `_binary_index_html_end`
- mDNS: managed component `espressif/mdns` in `idf_component.yml`; CMakeLists REQUIRES entry `espressif__mdns` (double underscore)
- `max_uri_handlers = 16`; currently 14 handlers registered

**PWA note:** Service Worker requires HTTPS. On `http://tes-charger.local` (plain HTTP), SW registration is silently blocked — offline caching does not work. "Add to Home Screen" shortcut works over HTTP.

---

## Release & OTA

**Release:** `git tag v3.x.x && git push origin v3.x.x` → GitHub Actions builds with ESP-IDF v5.5.1, creates Release with `tes_charger.bin` (OTA) and `tes_charger_flash.bin` (initial flash), deploys GitHub Pages. `docs/manifest.json` uses relative path `./tes_charger_flash.bin`; Pages source must be **GitHub Actions**.

**OTA:**
- First flash: GitHub Pages tool at `https://a950523a.github.io/TES-Taiwan-Electric-Scooter-Charging-Controller/`
- Web UI pull: "更新至最新韌體" → `POST /ota`
- Manual upload: `POST /ota/upload` or `curl --data-binary @tes_charger.bin http://tes-charger.local/ota/upload`

---

## File Structure

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
|   |   +-- mqtt_svc.c/.h       MQTT remote monitoring (v3.2.0)
|   |   +-- scheduler_svc.c/.h  scheduled charging (v3.4.0)
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
    +-- idf_component.yml       also declares espressif/mdns
docs/
+-- index.html              GitHub Pages 首次燒錄工具 (ESP Web Tools)
+-- manifest.json           燒錄工具 manifest（相對路徑 ./tes_charger_flash.bin）
+-- monitor.html            Cloud PWA 遠端監控（MQTT.js WebSocket，v3.2.0）
```
