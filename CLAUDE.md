# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 firmware for a DC charging controller compatible with the **TES-0D-02-01** standard used by Taiwan electric scooters (e.g., eMoving iE125). The controller bridges an external PSU to a vehicle's BMS via CAN bus, with a web UI, OLED display, and OTA updates.

Firmware lives in `firmware/` (ESP-IDF, pure C99). V2 PlatformIO code has been removed.

License: CC BY-NC-SA 4.0 (non-commercial).

### ⚠️ Deployment constraints — read before proposing anything structural

**Units have been sold and are deployed in the field, running a mix of V2 and V3
firmware on several hardware revisions.** Users cannot be forced to update, and the
hardware is expected to keep changing. Two consequences that are not obvious from the
code:

1. **OTA is the only update path for deployed units.** Anything that requires a full
   reflash (bootloader + partition table + app over USB) strands every device already in
   the field — they can never take another update. This is a hard constraint, not a
   preference.
2. **The oversized app partitions are deliberate.** 7.94 MB each for a 1.33 MB binary
   looks like waste, but the firmware is expected to grow while carrying support for
   *every* hardware revision simultaneously, since old units must keep receiving updates.
   The headroom is what buys that. **Do not propose shrinking them to free up space for
   a data partition** — that is a partition-table change, i.e. constraint 1.

**How to add flash-backed storage anyway, when it is eventually needed:** the partition
table belongs to the *device*, not the firmware. New hardware revisions can ship with a
data partition; existing units keep their current layout; **one firmware binary serves
both** by looking for the partition at runtime:

```c
const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "trace");
if (p) { /* persist */ } else { /* current RAM-only behaviour */ }
```

Feature present on new hardware, nothing broken on old, nobody forced to do anything.
This does **not** require a major version bump — the architecture boundary that would
justify V4 is the flash layout, which is a hardware-revision boundary, not a software one.

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
| app0 | **7.94 MB** | active firmware (~17% used) |
| app1 | **7.94 MB** | OTA update slot |

Only 64 KB of the 16 MB is unallocated (app1 ends at `0xFF0000`). That is not an
oversight — see **Deployment constraints** above for why the app partitions stay this
large and why repartitioning is off the table for deployed units.

Changing the partition table requires a full reflash (bootloader + partition-table + app); OTA-only is not sufficient.

**Build environment (PowerShell, Windows):**

Installed via the Espressif online installer to `C:\Espressif` (IDF_TOOLS_PATH),
framework at `C:\Espressif\frameworks\esp-idf-v5.5.5`. The installer bundles its own
Git and Python — no system-wide Git/Python is present on this machine.

CI (GitHub Actions) pins **v5.5.1**; the online installer only offers the latest patch
of each series, so local builds use **v5.5.5** (same 5.5.x API).

> ### ⚠️ The repo path contains non-ASCII characters — in-place builds FAIL
>
> The checkout lives at `D:\文件\GitHub\...`. Three separate tools in the ESP-IDF
> toolchain choke on that path under a cp950 (Traditional Chinese) Windows locale:
>
> | Stage | Failure | Workaround |
> |-------|---------|-----------|
> | `kconfgen` | `UnicodeDecodeError: 'cp950' codec can't decode` reading `build/config.env` | `PYTHONUTF8=1` |
> | `ccache` | `filesystem error: Cannot convert character sequence` | `idf.py --no-ccache` |
> | `objdump` (link step) | `xtensa-esp32s3-elf-objdump -h .../libxtensa.a` exits 1 | **no workaround** |
>
> The first two are fixable with env vars; the **objdump failure at the link stage is
> not** — GNU binutils resolves filenames through the ANSI codepage. A directory
> junction does not help either: CMake canonicalises it back to the physical path.
>
> **To build locally, the source must sit on an ASCII-only path.** Either move the
> checkout (e.g. `C:\dev\TES-...`), or copy `firmware/` to an ASCII path for a
> throwaway verification build. GitHub Actions is unaffected (Linux runner).

Verification-build recipe used from an ASCII path (bypasses `export.ps1`/`Initialize-Idf.ps1`,
both of which depend on `idf-env` config that points at the wrong `esp_idf.json` here):

```powershell
$env:IDF_PATH       = "C:\Espressif\frameworks\esp-idf-v5.5.5"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:PYTHONUTF8     = "1"
$py = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
# apply tool paths, then:
& $py "$env:IDF_PATH\tools\idf.py" --no-ccache set-target esp32s3
& $py "$env:IDF_PATH\tools\idf.py" --no-ccache build
```

(`idf_tools.py export --format key-value` supplies the PATH entries; `Initialize-Idf.ps1`
defines `idf.py` as a *PowerShell function*, so it must be dot-sourced and used in the
same scope — `& export.ps1` silently loses it.)

**Git submodules —— 有兩個，不是只有 u8g2:**

| Path | Upstream | Notes |
|------|----------|-------|
| `firmware/components/u8g2/` | `olikraus/u8g2` | third-party, never edited here |
| `firmware/components/tes_protocol/` | `a950523a/TES-Protocol` | **our own repo** — `tes_types.h`, `tes_codec.c/.h` live here |

On a fresh clone:
```bash
git submodule update --init
```

⚠️ Editing `tes_types.h` or `tes_codec.c` changes the **submodule**, not this repo.
Those changes must be committed and pushed in `firmware/components/tes_protocol/`
first, then the updated pointer committed here — otherwise CI checks out the old
`tes_protocol` and the build breaks on missing symbols.

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
| `task_can_rx` | 15 | **4 KB** | event | TWAI receive -> raw queue + `can_driver_service()` |
| `task_tes_sm` | 12 | **8 KB** | 10 ms | SM tick + output execution + snapshot update |
| `task_hal_poll` | 10 | 4 KB | 10 ms | button debounce + ADC + PSU UART poll |
| `task_display` | 4 | 4 KB | 50 ms | OLED render + LED update |
| `task_network` | 3 | 12 KB | 100 ms | WiFi + HTTP server |
| `task_ota` | 2 | 16 KB | event | esp_https_ota |
| `task_notify` | 2 | 6 KB | event | push notification via webhook (v3.1.0) |
| `task_mqtt` | 2 | 8 KB | event + 10/30 s | MQTT publish status + subscribe cmd (v3.2.0) |
| `task_scheduler` | 2 | 4 KB | 30 s | NTP sync (pool.ntp.org, UTC+8) + charging window edge detection → g_btn_event_queue (v3.4.0) |
| `task_monitor` | 1 | 4 KB | 10 s | heap + stack watermark logging |
| `task_log` | 1 | 3 KB | event | charge session history to NVS (v3.1.0) |

Charge-curve sampling and the value-change log run inline in `task_tes_sm` (no extra
task) — see `trace_svc` below.

**⚠️ Stack sizing is not cosmetic here.** A single `ESP_LOGx` costs >1 KB of stack
(`esp_log` → `vprintf`), so any task that can log needs ≥4 KB. `task_can_rx` ran at
2 KB with 52 bytes to spare and overflowed the moment `can_driver_service()` first
logged (v3.5.0 fix). When adding a log call to a small task, raise its stack too.
`task_monitor` prints every task's high-water mark every 10 s — **read that line first
when debugging any reboot**; it names the culprit directly.

Self-deleting tasks (`mqtt`/`ota`/`log` when unconfigured) must call
`g_task_unregister_self()` before `vTaskDelete(NULL)`. `spawn()` in `main.c` handles
the race where the task exits before its handle is even registered — see the comment
there; do not "simplify" it back to a plain create-then-register.

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

### CAN diagnostics panel

The panel covers every field of all six frames, plus two things that are more important
than any individual value when CAN won't work:

**Per-frame liveness** — `rx_*_count` + `rx_*_age_ms` (counted in `drain_can_rx_queue()`).
Without these you cannot tell "the vehicle is sending nothing" from "the vehicle is sending
zeros", which is the first thing to establish. Shown as 未收到 / 0.4s 前 · 842 幀, coloured
green <1 s, amber <5 s, red beyond (the protocol period is 100 ms). TX frames show a sent
count instead — for our own frames the question is whether they left at all.

**TWAI controller health** — `can_driver_get_health()` wraps `twai_get_status_info()`:
state, TX/RX error counters, bus errors, arbitration losses, missed RX. A rising
`bus_err_count` with no frames received is the signature of bad wiring, a missing
termination resistor, or a baud-rate mismatch — invisible if you only look at frame
contents. TX/RX error counters turn red at ≥128 (error-passive threshold).

Fields that were missing before and are now present: `v501_soc`, `v5f0` bit1 熔接異常
(safety-relevant, never displayed), `v5f0_max_current`, `v5f0_maker`, `c509_seq`,
`c5f8_maker`.

`can_driver.h` includes `tes_protocol/tes_types.h` for `can_bus_state_t`.

### 0x500 bit definitions (TES-0D-02-01 表 16)

Named macros live in `tes_types.h` (`V500_FAULT_*`, `V500_ST_*`) — do not use bare hex.

**byte 0 — 故障旗標** (all 0=正常, 1=異常; bit 6-7 預備固定 0)

| bit | 意義 |
|-----|------|
| 0 | 供電系統異常 — ⚠ 車輛指控**充電樁**，不是它自己的電池 |
| 1 | 電池過電壓 |
| 2 | 電池不足電壓 |
| 3 | 電池電流差異異常 |
| 4 | 電池高溫異常 |
| 5 | 電池電壓差異常 |

`fault_flags = 0x01` in the beta auto-start bug is **bit 0 = 供電系統異常** — the vehicle is
reporting that *our* supply looks wrong, which matches the VLIM2=0 hypothesis. The web UI
says so explicitly and points at 0x508's `fault_detect_voltage`.

**byte 1 — 狀態表示旗標** (polarity differs per bit; bit 4-7 預備固定 0)

| bit | 0 | 1 |
|-----|---|---|
| 0 | 不可充電 | 可進行車輛充電 |
| 1 | 接觸器關閉／熔接診斷中 | 接觸器斷開／熔接診斷終了 |
| 2 | 車輛姿勢可充電 | 車輛姿勢不可充電 |
| 3 | 無正常停止要求 | 有正常停止要求 |

The SM logic was verified against this table and is correct. The CAN diagnostics panel had
bit 2 mislabelled as "停止請求"; it is 車輛充電姿勢, now fixed.

### 0x508 bit definitions (TES-0D-02-01 表 16)

Macros: `C508_FAULT_*`, `C508_ST_*`. This frame is what **we** assert to the BMS, so a wrong
bit is a false report about the charger — treat it as more serious than a display bug.

**byte 0 — 故障旗標** (bit 3-7 預備固定 0)

| bit | 意義 |
|-----|------|
| 0 | 供電系統異常 (0=正常, 1=發生) |
| 1 | 直流供電裝置異常 (0=正常, 1=異常) — 「直流供電裝置」＝充電樁本體含 PSU |
| 2 | 電池不適合 (0=適合, 1=不適合) |

`enter_fault()` picks the bit from `fault_source` via `c508_fault_bit()`:

| fault_source | bit | 理由 |
|---|---|---|
| VOLTAGE_INCOMPAT | 2 電池不適合 | 車端要的電壓超出我們能給的範圍 |
| PSU_LOST, EMERGENCY_BTN | 1 直流供電裝置異常 | PSU 就是直流供電裝置；緊急停止是本機切斷 |
| 其餘 | 0 供電系統異常 | 語意最廣。**車端造成的故障絕不報 bit1** —— 那等於自認充電樁壞掉 |

Previously every fault except two hardcoded cases defaulted to `0x01`, so a PSU failure was
reported to the vehicle as a generic supply-system fault rather than a device fault.

**byte 1 — 狀態表示旗標** (bit 3-7 預備固定 0)

| bit | 0 | 1 |
|-----|---|---|
| 0 | 輸出追隨運轉中 | 停止控制中或停止狀態 |
| 1 | 待機中 | 充電中 |
| 2 | 電子鎖解除 | 電子鎖閉鎖中 |

**byte 1 was already correct** — `0x06` during charging = 充電中 + 電子鎖閉鎖, matching the
value CLAUDE.md already documented. Only the labels were wrong: bit0 was described as
"待機/準備" in both `tes_types.h` and the web CAN panel, but it is **停止控制** with the
opposite polarity (1 = stopped). Labels fixed; the transmitted values are unchanged.

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

**v3.5.0 released 2026-09-10.** Fixes the START-crash regression introduced on `dev` (b793cf3) and a batch of diagnostic/UX problems found alongside it. **Vehicle-tested: charging works end to end** (`IDLE → PARAM_EXCHANGE → PRE_CHARGE → CHARGING`). `idf.py build` zero errors on ESP-IDF v5.5.1.

**v3.5.0 fixes — the two crashes:**
1. **START → instant reboot.** `task_can_rx` stack overflow, *not* `task_tes_sm`. b793cf3 added `can_driver_service()` to that task's loop; pressing START starts 0x508/0x509 TX, no ACK on the bus → TWAI error-passive → `ESP_LOGW` inside a 2 KB task → overflow. Stack raised to 4 KB (headroom 52 → 2100 bytes idle, 1860 charging).
2. **Random reboot ~12 s after boot.** `spawn()` registered the task handle *after* `xTaskCreate`, but tasks outrank `app_main` and are unpinned, so a self-deleting task could vanish before registration and leave a dangling handle for `task_monitor` → `LoadProhibited` (EXCVADDR=0).

**v3.5.0 fixes — web UI was unusable ("offline, no data"):** three compounding causes, all fixed —
WiFi power save was never disabled (`WIFI_PS_MIN_MODEM`, radio woke every ~307 ms);
`/trace` + `/tracelog` sent one TCP segment *per record* while holding httpd's single
worker; and Nagle delayed every sub-MSS response by ~1.4 s (`/status` at 1528 B was
immune, which is why big responses looked fast and small ones looked broken).
Measured after: `/config` 1.4 s → 0.029 s, `/tracelog` 3.5 s → 0.12 s.

**v3.5.0 — fault reporting now persists.** OLED, LED and web all keyed off
`fault_latched`, which the SM clears on auto-recovery (10 s, or **1 s** for CP-loss in
manual mode), so the reason vanished before it could be read. All three now key off
`fault_source` instead. Charge history stores and displays the actual reason too.

**⚠️ Upgrading to v3.5.0 wipes existing charge history.** `charge_session_t` grew
20 → 24 bytes, so the NVS blob length no longer matches and `log_svc_init()` starts
fresh. Deliberate — reading old 20-byte records as 24-byte ones would misalign every
field. No migration was written.

**Confirmed TES-0D-02-01 protocol timing (commit c7fa3f8):** `VP ON → CP ON → CAN 0x500 bit0=1 → charging → CAN ends → CP OFF`. CP appears before CAN; CP OFF→ON edge is the primary auto-start trigger, CAN rising edge is backup.

**ESP-NOW PSU transport implemented (2026-05-16):** `psu_driver` now supports dual transport (UART + ESP-NOW). `POST /psu/pair` added to REST API. LianMing PSU Controller side not yet updated. **Not yet tested.**

**PSU disconnect fault fix (commit 89bc1c4, 2026-05-22):** `run_monitoring()` no longer faults on PSU disconnect unconditionally. `psu_session_connected` snapshot taken at `PRECHARGE_STEP_COMPLETE` — mid-charge disconnect only faults if PSU was present at session start; PSU-absent-at-start = ADC-only mode, charging continues uninterrupted. Fixes auto-start + PSU-less testing.

**In progress:** React Native mobile app (Expo + EAS Build, Android APK sideload). Will support multiple controllers, local HTTP + MQTT remote, guided onboarding. Not yet started.

**✅ Vehicle-verified as of v3.5.0:** manual START → full charge sequence; CP transition
0 V → 8.99 V; `trace_svc` session start; web UI latency (measured with curl, see above).

**⚠️ Still not vehicle-tested:** notify_svc, PWA offline caching, log_svc, WiFi scan,
mDNS AP mode, MQTT, Cloud PWA, power/energy tracking, CAN diagnostics panel, charge
timer stop, scheduler, beta auto-start, ESP-NOW PSU transport.

**⚠️ Shipped in v3.5.0 but never exercised on hardware** — verify these before trusting them:
- **Fault display persistence** (OLED / LED / web). Needs a real fault to confirm the
  screen holds instead of reverting to Standby. Easiest repro: start charging, reach
  CHARGING, unplug the connector (manual mode auto-recovers in 1 s, so the old build
  showed nothing).
- **Fault reason in charge history.** `/history` was emptied by the struct change, so
  no record with `fault_source` has ever been rendered. Expect "充電槍鬆脫" rather than
  a bare "故障".
- **`Reset Fault` in IDLE.** Newly wired up; previously the menu item did nothing in
  that state.

**⚠️ Beta auto-start bug (still unconfirmed as of v3.5.0):** the v3.5.0 vehicle test used
**manual START only**, so this remains untested. Vehicle sends `fault_flags=0x01` in 0x500
shortly after charging starts, causing false FAULT. Two fixes applied but not yet vehicle-tested:
1. `check_battery_compatibility()`: prevented `fault_detect_voltage` (VLIM2 in 0x508) from being set to 0 when `max_charge_voltage=0` — vehicle interprets VLIM2=0 as "fault when output voltage ≥ 0V".
2. `run_monitoring()`: added 2-second grace period before acting on `fault_flags` — vehicle may send residual `fault_flags=0x01` frames during early CHARGING while its state machine stabilises on `status_flags=0x06`.
Also: max current > 15A now blocks auto-start (SM guard + web UI warning). Stale `vehicle_status` cleared on IDLE→PARAM_EXCHANGE transition.

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

## PSU Protocol

Text protocol shared by both transports:
- RX: `V=xx.x,I=xx.x\n` -- actual output (PSU actively outputting, ~100ms interval)
- RX: `HB\n` -- heartbeat (PSU standby, ~1s interval; **ESP-NOW only**)
- RX: `CMD_ACK:SET_V:xx.x\n` / `CMD_ACK:SET_I:xx.x\n` -- command ack (standby)
- TX: `SET:V=xx.x\n` / `SET:I=xx.x\n` -- setpoint commands

`HB\n` is parsed in `parse_frame()`; it updates `s_last_valid_ticks` and sets `connected=true` without changing voltage/current. This is the primary liveness signal for ESP-NOW idle state.

When `psu_voltage == 0` (PSU standby), SM falls back to ADC voltage for OLED display and 0x509 CAN output — prevents vehicle from seeing 0V/0A and aborting.

**ESP-NOW safety constraints (`psu_driver.c`):**

| Parameter | Value | Notes |
|-----------|-------|-------|
| Receive timeout | 2s | vs 3s for UART；MAC-ACK 連敗 3 次仍 30ms 快斷 |
| MAC-ACK fail limit | 3 consecutive | → immediate disconnect |
| SET command throttle | Δ > 0.05 **or** 500ms elapsed | reduces 100Hz → ~2Hz in steady state |
| RSSI warn threshold | −80 dBm | logs LOGW on threshold crossing |

`psu_status_t` new fields: `rssi` (last received dBm, 0 for UART), `fail_streak` (consecutive send failures, 0 for UART).

**Transport selection** (`psu_transport` NVS key, default 0=UART):
- `PSU_TRANSPORT_UART=0`: UART pins 43/44, always available
- `PSU_TRANSPORT_ESPNOW=1`: wireless, PSU also uses ESP32; init after `network_svc_init()`

**ESP-NOW pairing flow:**
1. PSU enters pairing mode → broadcasts `"PSU_HELLO\n"` every 500 ms
2. TES enters pairing mode (`POST /psu/pair` or button combo)
3. TES receives `PSU_HELLO` → saves sender MAC to NVS (`psu_mac` blob) → adds as peer
4. Both sides use stored MAC for all subsequent communication
5. Pairing window = 10 s; timeout logs warning and clears pairing flag

**ESP-NOW init constraint:** `psu_driver_set_transport()` must be called after `network_svc_init()` (ESP-NOW needs WiFi driver started). In `main.c`, called immediately after `network_svc_init()`.

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
| `psu_trans` | uint32 | 0 | PSU transport: 0=UART, 1=ESP-NOW |
| `psu_mac` | blob[6] | — | ESP-NOW peer MAC (PSU 的 MAC 地址，配對後寫入）|
| `sta_en` | bool | **true** | false = 固定 AP 模式但保留 SSID／密碼。預設必須為 true，否則 OTA 上來的舊機器會全部掉進 AP 模式 |
| `dev_name` | str[24] | "" | 裝置顯示名稱；空 = 顯示 `TES Charger <id>`。不影響主機名 |
| `sess_seq` | uint32 | 0 | (namespace `tes_hist`) 充電 session 流水號，供 trace_svc 使用 |

Charge history: namespace `"tes_hist"`, blob key `"log"` (**484 bytes**, 20 × `charge_session_t` circular buffer). Blob length is the version check — changing `charge_session_t` discards existing history by design (see Charge Session History).

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
NVS key `auto_s`. VP relay held ON in IDLE. Triggers `PARAM_EXCHANGE` on CP OFF→ON edge (primary) or CAN 0x500 bit0 rising edge (backup). Fields added to `tes_sm_t`: `cp_prev`, `last_can_permit`, `psu_session_connected`.

**CP sampling cadence:** `task_hal_poll` interleaves the two ADS1115 channels, so CP is
sampled every **100 ms** (not 50 ms). The SM ticks at 10 ms, so it must not re-process the
same sample — `tes_sm_inputs_t.cp_sample_seq` (incremented by `task_hal_poll`, tracked as
`sm->last_cp_seq`) gates the CP update. Without that gate the same bad sample is counted
repeatedly and `CP_ERROR_THRESHOLD` is reached from a single glitch. `cp_prev` is saved
before each update for edge detection.

**PSU-less (ADC-only) mode:** `psu_session_connected` is snapshotted at `PRECHARGE_STEP_COMPLETE`. If PSU was absent at session start, `run_monitoring()` skips the PSU disconnect fault — charging continues using ADC voltage for display and 0x509 output. If PSU was present at session start and later disconnects mid-charge, FAULT is triggered as normal (safety preserved).

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
`task_tes_sm` accumulates V×I during CHARGING (`energy_wh += V*A/360000.0f` per 10 ms tick). Publishes `EVT_SESSION_COMPLETE` with `charge_session_t` (**24 bytes**). `log_svc` stores last 20 sessions as NVS blob (`session_log_t` = 4 + 20×24 = **484 bytes**).

Key types in `tes_types.h`:
```c
typedef enum {
    STOP_REASON_NORMAL=0, STOP_REASON_USER=1, STOP_REASON_FAULT=2,
    STOP_REASON_EMERG=3, STOP_REASON_BMS=4, STOP_REASON_TIMER=5, STOP_REASON_VOLTAGE=6
} stop_reason_t;

typedef struct {
    uint32_t duration_s; float energy_wh; float stop_voltage_v;
    uint32_t session_id;
    uint8_t  soc_start, soc_end, stop_reason, energy_estimated;
    uint8_t  fault_source;   // fault_source_t — FAULT_SRC_NONE when not a fault stop
    uint8_t  _pad;
    uint16_t fault_ctx_a;    // meaning depends on fault_source
} charge_session_t;          // 24 bytes — exactly charger_event_t.payload's limit
```

**⚠️ This struct is at the payload ceiling.** `charger_event_t.payload` is 24 bytes and
`task_tes_sm.c` `memcpy`s the whole struct into it; a `_Static_assert` at that call site
blocks any further growth at compile time. To add a field, widen the payload first.

**⚠️ Changing this struct's size discards existing history.** `log_svc_init()` compares
the stored blob length against `sizeof(session_log_t)` and starts fresh on mismatch —
deliberate, since reinterpreting old records at the new stride misaligns every field.
There is no migration path; write one if history ever needs to survive.

`GET /history` returns newest-first JSON array (max 20 entries), each with a
`session_id` linking to the trace buffers below, plus `fault_source` / `fault_ctx_a`.
The web UI renders those through the same `FAULTS[]` table as the live fault panel, so
a fault stop reads "充電槍鬆脫" rather than a bare "故障".

### Charge Curve + Detailed Log (`trace_svc`)

Two ring buffers in **PSRAM**, written only by `task_tes_sm`, read by the HTTP task:

| Buffer | Capacity | Size | Contents |
|--------|----------|------|----------|
| samples | 8192 | 128 KB | V / I / BMS-requested-I / SOC / state, every **5 s** during CHARGING plus one on every state transition |
| events | 6144 | 528 KB | one line per **value change**, each timestamped |

> ⚠️ **Volatile — and this is a decision, not a limitation.** Both buffers are lost on
> power-off. Hardware resources are not the constraint (6.4 MB PSRAM free, 64 KB flash
> unallocated); the constraint is that persisting time-series needs a writable flash
> partition, and adding one is a partition-table change — which strands every deployed
> unit, per **Deployment constraints**. A 2-hour session is roughly 22 KB of curve plus
> ~25 KB of events, so NVS (20 KB total, already holding config) is genuinely too small.
> The NVS `charge_session_t` summary (log_svc) is unaffected and survives reboots.
>
> When this does become worth doing, use the runtime `esp_partition_find_first()` pattern
> in **Deployment constraints** rather than repartitioning existing devices.

**The web UI must say so.** The history panel carries a note explaining that curves and
logs live in RAM and vanish on power-off, and rows whose trace is gone show a dimmed `·`
with a tooltip instead of an expand caret. A blank cell reads as a broken UI; users
reported it as a bug before the explanation existed.

A **trace session starts at `IDLE → PARAM_EXCHANGE`**, not at CHARGING — the handshake
is where faults actually happen, so the log must cover it. It ends on return to IDLE.
`session_id` comes from NVS key `sess_seq` (namespace `tes_hist`) so ids stay unique
across reboots. The index keeps the last `TRACE_MAX_SESSIONS` (8) sessions.

### Fault reporting

A fault must tell the user **what happened, why, and what to do** — not a hex code to look
up afterwards. Three pieces make that work:

1. **`fault_source`** (`fault_source_t`, 9 values) — the internal cause. Never sent on CAN;
   `status_508.fault_flags` bits are protocol-defined and go to the BMS, so they can't carry
   custom meanings.
2. **`fault_ctx_a` / `fault_ctx_b`** — two numbers captured *at the instant the fault is
   raised*, meaning defined per `fault_source` (see the enum comments). The state machine
   only records numbers; `tes_protocol` does no string formatting.
3. **Display layers format them.** This turns "Code:0x01" into "車輛要求 72.0 V，但最大電壓
   設定只有 60.0 V → 把最大電壓調到 72.0 V 以上".

`enter_fault(sm, out, tick_ms, src, ctx_a, ctx_b)` — every call site must state its context.

Web UI (`FAULTS` table in index.html): title / why / action per source, with `ctx` rendered
into the text. `FAULT_SRC_EMERGENCY_VEHICLE` is styled **informational (amber), not red** —
the iE125 sends 0x5F0 after every normal charge end, so a red alarm there would be wrong.
The panel shows whenever `fault_source > 0`; the old condition also required `fault_flags`,
which hid every fault that doesn't set one (PSU lost, CP lost, voltage limit).

OLED keeps English — u8g2 `*_tr` fonts are ASCII-only and adding a CJK font is a large
change — but now shows a two-line description plus the context values, with the raw code
demoted to a footnote.

**Visibility is keyed to `fault_source`, never `fault_latched` (v3.5.0).** The SM clears
`fault_latched` when FAULT auto-recovers — after `fault_timeout_ms`, which is 10 s for most
faults but only **1 s** for CP-loss in manual mode — so anything gated on it disappeared
before the user could read it. `fault_source` survives auto-recovery and is cleared only by:

| Cleared by | Where |
|---|---|
| Manual START (button or remote) | `TES_STATE_IDLE`, `start_requested` branch |
| auto_start triggering a new round | `TES_STATE_IDLE`, CP/CAN edge branch |
| `Reset Fault` menu item | `TES_STATE_IDLE` **and** `TES_STATE_EMERGENCY` |

All three surfaces use the same condition — OLED and LED via
`fault_latched || fault_source != FAULT_SRC_NONE`, web via
`src>0 || fault || state==='fault' || state==='emergency'`. Keep them in sync; a mismatch
produces the exact contradiction v3.5.0 fixed (screen says FAULT STOP, LED says standby).

Note `TES_STATE_IDLE` handles `fault_clear_requested` as of v3.5.0. Before that only
EMERGENCY consumed the flag, so `Reset Fault` silently did nothing in IDLE — which
matters now that the fault text persists there and needs a way to be dismissed.

**Change detection lives in `task_tes_sm.c`, not `tes_sm.c`** — `tes_protocol`/`tes_sm`
must stay zero-dependency (no PSRAM, FreeRTOS or string formatting). Tracked fields:
state, CP state, relay/coupler/VP, PSU connected, 0x500 status+fault, 0x508 status+fault,
BMS voltage limit, BMS current request (Δ≥0.5 A), SOC, output V (Δ≥0.5 V), output I
(Δ≥0.5 A), PSU setpoints, fault source. Threshold-gated fields only update their
"previous" value when a record was actually emitted — otherwise slow drift never trips
the threshold and would never be logged.

Locking uses a **mutex, not `portMUX`**: the critical section touches PSRAM, and
`taskENTER_CRITICAL` disables interrupts. Writers take the lock with a 5 ms timeout and
drop the record on failure — tracing must never delay the 10 ms SM tick.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/trace` | `?session=<id>` (default: newest), `?limit=N` (default 2000, evenly decimated). Returns `sessions[]` list + compact `samples[]` of `[t_ms, V×10, I×10, BMSreq×10, soc, state]`. Chunked. |
| GET | `/tracelog` | `?session=<id>&limit=N` (default 1000, newest-first window). Returns `events[]` of `[t_ms, epoch_s, text]`. Chunked. |

`t_ms` is uptime ms for events and session-relative ms for samples; both use the same
`esp_timer` base as `platform_tick_ms()`, so `event.t_ms − session.start_uptime_ms`
gives session-relative time. `epoch_s` is 0 until NTP syncs, in which case the web UI
falls back to `+MM:SS.mmm` relative timestamps.

**Web UI** — the chart and log live *inside each charge-history row*: clicking a row
expands an inline detail panel holding that session's curve + log, fetched lazily on
first expand and cached. Multiple rows can be open at once, so all chart DOM lookups are
scoped to the row's own container (`box.querySelector('.hit')`), never by global id.

The list merges two sources, sorted by `session_id` descending:
- `/history` — NVS summaries; survive reboot, but their trace may be gone
- `/trace` → `sessions[]` — RAM traces. **An attempt that faults before reaching
  CHARGING produces no NVS record**, yet that is exactly the case worth debugging, so
  those appear as extra rows labelled 未完成. Without this merge they'd be unreachable.

Rows whose `session_id` is not in the RAM trace index get no expand caret. Legacy rows
written before `session_id` existed have `session_id == 0` and sort last.

Chart: three stacked panels sharing a time axis (voltage / current / SOC). Voltage is
auto-ranged rather than 0-based — a 54→67 V swing on a 0–100 V axis renders as a flat
line. Current is 0-based (so "is it actually outputting?" is visible) with the BMS
request overlaid as a dashed line; SOC is fixed 0–100 %. Hand-written SVG:
**no CDN libraries**, since AP mode has no internet access.

### MQTT Remote Monitoring
NVS keys: `mqtt_url` (empty = disabled), `mqtt_topic`. Publishes `{prefix}/status` every 10 s (CHARGING) or 30 s (other), immediately on state change. LWT: `{"state":"offline"}` retained. Subscribes `{prefix}/cmd` QoS 1 for `{"cmd":"start"/"stop"}` → `g_btn_event_queue`. Uses ESP-IDF `esp-mqtt` component. `GET /mqtt/link` returns Cloud PWA URL with broker/topic in fragment.

**Cloud PWA** (`docs/monitor.html`): MQTT.js via WebSocket, config from URL fragment (`#b=host&p=wsport&t=prefix`); stored in `localStorage` after first use. GitHub Pages (HTTPS) → Service Worker works → offline caching valid.

---

## Mobile App (In Progress)

**技術：** React Native + Expo（Managed Workflow）+ EAS Build → Android APK（側載）

**目標：** 非技術使用者也能輕鬆使用，支援多台控制器管理。

**連線方式：**
- 本地（同 WiFi）：HTTP REST API，`http://<ip>/`
- 遠端：MQTT WebSocket（透過 broker）

**控制器配對（首次新增）：**
- 掃描區域網路子網路，對每個 IP 嘗試 `GET /status`，回應含充電器特徵欄位者視為 TES 控制器
- 使用者點選後命名，MAC/IP 儲存至 AsyncStorage

**主要畫面：**
1. 引導流程（首次開啟）：歡迎 → 連線說明 → 自動掃描 → 命名 → 完成
2. 控制器列表：每台狀態卡片（在線/充電中/離線）
3. 儀表板：大字電壓/電流/SOC + 開始/停止
4. 設定：對應現有 REST `/config` API
5. 充電歷史：對應 `GET /history`

**開發狀態：** 尚未開始，ESP-NOW 韌體完成後進行。

---

## Scheduler + Auto-Start Integration (Planned)

**Problem:** when both `sched_enabled` and `auto_start` are on, CP/CAN edges outside the charging window could trigger unintended charging.

**Planned solution:** add `bool in_charging_window` to `tes_sm_inputs_t`. Gate auto-start CP/CAN edge detection on this field. `scheduler_svc_is_in_window()` returns `true` when `sched_enabled=false`. Scheduler's `EVT_BUTTON_START` path (manual START route) clears `charge_complete_latched` directly — no re-plug needed.

**Implement after auto_start hardware testing.**

---

## Known Technical Debt

- `check_battery_compatibility`: voltage limit logic needs validation against real vehicle CAN data
- `sw.js` `CACHE_NAME` is a fixed `'tes-v3'` and never changes across firmware versions, so
  its `activate` handler never purges anything. Harmless today (the shell handler is
  network-first, and the SW does not even register over plain HTTP on a LAN IP), but it
  will bite if the strategy ever changes to cache-first. Derive it from the build version.
- **Comments have drifted from code more than once.** `display_svc.c`, `index.html` and
  `sw.js` each carried a comment describing the *correct* behaviour while the code below
  still had the old logic (all three were the same v3.5.0 fault-visibility bug; `sw.js`
  says "cache-first" over a network-first implementation). When a comment here explains
  a fix, verify the code actually does it.

---

## Network & Web UI

### Device identity — two units on one LAN

Every unit derives a **`device_id`** from the last 3 bytes of its WiFi STA MAC
(`config_svc_init()`, e.g. `a1b2c3`). Everything network-visible is namespaced by it,
because the previous hardcoded names made two units on the same LAN indistinguishable:

| | Before | Now |
|---|---|---|
| mDNS hostname | `tes-charger` (collided) | **`tes-<id>.local`** — unique, and deliberately *not* derived from the user's name so bookmarks survive a rename |
| AP SSID | `TES-Charger` (collided) | **`TES-Charger-<id>`** |
| MQTT topic prefix default | `tes/charger` (collided) | **`tes/<id>`** (only for units that never configured MQTT) |
| mDNS instance name / TXT | fixed "TES Charger" | user's `device_name`, else `TES Charger <id>` |

`tes-charger.local` still resolves **in both AP and STA mode** — it is registered as an
mDNS **delegated hostname** pointing at the current IP (re-applied on every got-IP event,
so DHCP changes are followed), plus an `_http._tcp` service registered *for that host*
via `mdns_service_add_for_host()`. The service registration matters: a delegated hostname
with no service attached is not reliably answered for plain A queries. With two units both
delegating it, whichever answers first wins; that is harmless because each unit's own
`tes-<id>.local` is always unambiguous.

**`device_name`** (NVS key `dev_name`, ≤24 chars, default empty) is a display label only.
Changing it updates the mDNS instance name and TXT record live — no reboot — but never
the hostname. It appears in the web UI `<h1>`, the browser tab title, and the `_http._tcp`
TXT records (`id`, `name`, `ver`) so a LAN scan can identify units without opening each one.
The OLED settings menu has a read-only `tes-<id>` row for cross-referencing.

**WiFi modes** — AP when *either* condition holds, STA otherwise:
- No SSID in NVS → AP mode, SSID `TES-Charger-<id>` (open), IP `192.168.4.1`
- `sta_enabled == false` (NVS `sta_en`) → AP mode **with SSID/password kept intact**
- Otherwise → STA mode, auto-reconnect, mDNS `tes-<id>.local` after got-IP

`sta_enabled` **defaults to true** and must stay that way: units upgrading by OTA have no
such NVS key, and defaulting to false would drop every deployed device into AP mode after
an update. The web UI applies the same rule to `/config` responses that lack the field
(`d.sta_enabled !== false`), so an old firmware's JSON doesn't render as "off".

Before this switch existed, the only way back to AP mode was erasing the SSID, which threw
the password away too. `POST /config` never calls `esp_restart()`, so toggling it does not
drop the current connection — it takes effect on the next boot.

mDNS starts in both AP and STA mode. `mdns_publish()` is called on every got-IP event, so
a DHCP address change refreshes the delegated hostname's address too.

**HTTP performance (v3.5.0) — three settings that must stay as they are.** Together they
took the UI from "looks offline" to sub-50 ms; each was independently sufficient to make
it feel broken:

| Setting | Where | Why |
|---|---|---|
| `esp_wifi_set_ps(WIFI_PS_NONE)` | `network_svc_start()`, after `esp_wifi_start()` | The STA default `WIFI_PS_MIN_MODEM` wakes the radio only every ~307 ms (`li: 3`), so every TCP round trip stalls. Mains-powered device — the saving buys nothing. |
| `cfg.open_fn = http_sock_open` → `TCP_NODELAY` | `start_http_server()` | httpd sends headers and body as separate `send()`s; Nagle holds the second small segment until the first is ACKed, and lwIP only flushes it on the slow timer (~1.4 s). Sub-MSS responses were the slow ones — `/status` at 1528 B was immune, which made "big fast, small slow" look impossible. |
| `chunk_out_t` 1 KB buffering | `/trace`, `/tracelog` | One `httpd_resp_sendstr_chunk()` per record = one TCP segment per record. Hundreds of records = hundreds of round trips, all while holding httpd's **single** worker thread, which queues `/status` behind them. |

HTML is served `Cache-Control: no-cache` (`UI_CACHE_CONTROL`). It was `max-age=86400`,
which left users on a stale UI for a day after each OTA with no way to notice. The page is
tens of KB over LAN; re-fetching costs far less than showing an outdated interface. This
does not affect offline support — the Service Worker's Cache API is independent of the
HTTP cache.

**REST API (port 80, CORS *):**

### Settings UI convention (v3.5.x) — don't undo this by accident

**Sliding switches save immediately. Typed values need the save button.**

That split is deliberate and learnable (iOS Settings works the same way). What must be
avoided is *some* switches saving instantly and others not — mixed behaviour within one
control type forces the user to guess every time.

| Piece | Where | Notes |
|---|---|---|
| `TOGGLE_KEY` | index.html | checkbox id → config key. **Adding a switch means adding it here**, otherwise it silently falls back to manual save and breaks the rule. |
| `autoSaveToggle()` | index.html | POSTs that one key; on failure or cancel it flips the switch back. The UI must never show a state the device doesn't hold. |
| `toggleConfirmMsg()` | index.html | Returns a confirm string for switches whose consequence is non-obvious. Criterion is **not** "is it important" but "will something happen the user didn't expect": currently STA-off (next boot is AP-only) and auto-start-on (VP always live, charging starts unattended). |
| `#cfg-bar` | index.html | Floating save bar for the typed fields. Shows the pending count, marks changed fields, and carries save feedback — `#msg` sits at the bottom of a ~140-line card and is off-screen when saving from mid-page. |
| `CFG_BASE` / `cfgChangedKeys()` | index.html | Dirty state is a comparison against the device's actual values, not a "touched" flag, so reverting a field by hand clears it. |

`POST /config` accepts partial updates and the UI relies on that: `save()` sends only
changed keys, and each switch sends just its own. Sending the whole object (the old
behaviour) made "adjust the current" walk into the WiFi-change branch and rewrite NVS
needlessly.

Two failure modes that were fixed once and are easy to reintroduce: hiding the bar's
buttons on error (the user sees the failure but has no way to retry), and leaving
`cfgBarLock` set after a failure (the bar then freezes on the old message and the pending
count stops updating — `cfgSaving` distinguishes "mid-save" from "showing a result").

### Device list page

`/` serves **`web/devices.html`** — a list of every TES controller on the LAN; the control
UI moved to **`/control`**. `GET /devices` does the discovery **on the device** via
`mdns_query_ptr("_http","_tcp", 2000ms, 20)`, because browsers have no mDNS-browse API and
subnet-scanning from JS is slow and often blocked.

Results are filtered on the TXT record `dev=tes-charger` so other `_http._tcp` services
(NAS, printers) are excluded. Most mDNS stacks do not answer their own queries, so the
handler appends itself if it wasn't in the results — otherwise the list would be short one
unit. Each card then fetches that unit's `/status` **cross-origin** (CORS is `*`) to show
live state, so the list doubles as a multi-charger dashboard; an unreachable unit just
shows 離線 without affecting the others.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Device list page (mDNS discovery) |
| GET | `/control` | Embedded control web UI |
| GET | `/devices` | JSON list of TES controllers found on the LAN |
| GET | `/status` | JSON snapshot: state, voltage, current, soc, target_soc, stop_mode, stop_voltage, timer, fault, fault_source, stop_reason, wifi, ip, ntp_synced, local_time, power_w, energy_wh, device_id, device_name, display_name, hostname, ap_ssid |
| GET | `/config` | JSON config: all `charger_config_t` fields |
| POST | `/config` | Partial update (any subset); WiFi changes require reboot |
| POST | `/start` | Sends `EVT_BUTTON_START` to `g_btn_event_queue` |
| POST | `/stop` | Sends `EVT_BUTTON_STOP` to `g_btn_event_queue` |
| POST | `/ota` | Pull firmware from URL (default: GitHub Releases latest) |
| POST | `/ota/upload` | Upload binary (`application/octet-stream`); progress via `/status` |
| GET | `/history` | Last 20 charge sessions (newest-first), incl. `session_id`, `fault_source`, `fault_ctx_a` |
| GET | `/trace` | Charge-curve samples for a session + session list (chunked) |
| GET | `/tracelog` | Timestamped value-change log for a session (chunked) |
| GET | `/manifest.json` | PWA manifest |
| GET | `/sw.js` | Service Worker |
| GET | `/icon.svg` | App icon |
| GET | `/wifi/scan` | Scan nearby APs (max 20: ssid, rssi, secured) |
| POST | `/notify/test` | Send test push notification |
| POST | `/psu/pair` | Start 10 s ESP-NOW pairing window (requires psu_transport=1) |
| GET | `/mqtt/link` | Cloud PWA URL with broker/topic fragment |

**CMake notes for embedded web UI:**
- HTML embedded via `EMBED_TXTFILES "web/index.html"`; symbol `_binary_index_html_start` / `_binary_index_html_end`
- mDNS: managed component `espressif/mdns` in `idf_component.yml`; CMakeLists REQUIRES entry `espressif__mdns` (double underscore)
- `max_uri_handlers = 24`; currently 20 handlers registered
- `web/devices.html` is a second `EMBED_TXTFILES` entry → `_binary_devices_html_start/_end`
- `sw.js` cache bumped to `tes-v3`; app shell is now `/` **and** `/control`
- `drivers` component REQUIRES `esp_wifi` (for ESP-NOW in `psu_driver.c`)

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
|   |   +-- web/index.html      embedded control UI, served at /control
|   |   +-- web/devices.html    embedded device-list page, served at /
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
