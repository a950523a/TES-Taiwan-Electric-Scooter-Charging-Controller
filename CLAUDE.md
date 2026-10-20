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
| tes_factory | 64 KB | signed factory record (2026-10-05) — **only on boards flashed fresh**, see below |

The app partitions end at `0xFF0000`; the last 64 KB, unallocated until 2026-10-05, now
holds `tes_factory` (data, subtype `0x40`). That is the **Deployment constraints**
pattern in practice: a board flashed over USB with this table gets the partition, a
deployed unit updated by OTA keeps its old table, and `factory_svc` treats a missing
partition as "no factory record" — nothing else depends on it. The app partitions did
not shrink and must not.

Changing the partition table requires a full reflash (bootloader + partition-table + app); OTA-only is not sufficient.

**ESP-IDF version: v5.5.5.** CI and the development machine are pinned to the same
version on purpose — a mismatch means backtrace addresses decoded locally do not match
the firmware actually running, which cost real debugging time once already. **Bump both
or neither.**

Staying on 5.5.x rather than 6.x is a deliberate call, not inertia — see
**Why not ESP-IDF 6.x** below.

**Build environment (PowerShell, Windows):** ESP-IDF is a git checkout at
`C:\Users\user\esp\v5.5.5\esp-idf` (upgrade = `git checkout <tag>` + `git submodule
update --init --recursive` + `idf_tools.py install`), tools in `C:\Users\user\.espressif`.

```powershell
$env:IDF_PATH = "C:\Users\user\esp\v5.5.5\esp-idf"
$py = "C:\Users\user\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
# 動態取得工具鏈 PATH —— 不要寫死版本號，換 IDF 版本時工具鏈目錄也會變
# （v5.5.1 用 xtensa-esp-elf/esp-14.2.0_20241119，v5.5.5 用 esp-14.2.0_20260121）
foreach ($line in (& $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value)) {
  if ($line -match '^([A-Z_]+)=(.*)$') {
    Set-Item -Path "env:$($matches[1])" -Value ($matches[2] -replace '%PATH%', $env:PATH)
  }
}
Set-Location "<repo>\firmware"
& $py "$env:IDF_PATH\tools\idf.py" build
```

Changing `IDF_PATH` invalidates the CMake cache — delete `firmware/build` and re-run
`set-target esp32s3` after any IDF version change, otherwise the stale cache points at
the previous compiler.

`export.ps1` / `Initialize-Idf.ps1` are best avoided: they define `idf.py` as a
*PowerShell function*, so they must be dot-sourced and used in the same scope —
`& export.ps1` silently loses it.

> ### ⚠️ Non-ASCII checkout paths break the build (not an issue on the current machine)
>
> The current checkout is on an ASCII path, so this does not apply — but a checkout under
> e.g. `D:\文件\GitHub\...` on a cp950 (Traditional Chinese) Windows locale fails in three
> separate tools:
>
> | Stage | Failure | Workaround |
> |-------|---------|-----------|
> | `kconfgen` | `UnicodeDecodeError: 'cp950' codec can't decode` reading `build/config.env` | `PYTHONUTF8=1` |
> | `ccache` | `filesystem error: Cannot convert character sequence` | `idf.py --no-ccache` |
> | `objdump` (link step) | `xtensa-esp32s3-elf-objdump -h .../libxtensa.a` exits 1 | **no workaround** |
>
> The first two are fixable with env vars; the **objdump failure at the link stage is
> not** — GNU binutils resolves filenames through the ANSI codepage, and a directory
> junction does not help (CMake canonicalises it back to the physical path). The source
> must sit on an ASCII-only path. GitHub Actions is unaffected (Linux runner).

### Why not ESP-IDF 6.x

Checked against this codebase, not assumed. v6.0 would be a **port, not an upgrade**, and
it buys this project almost nothing:

| v6.0 change | Impact here |
|---|---|
| Legacy TWAI API deprecated | `can_driver.c` uses 13 legacy symbols (`twai_driver_install`, `twai_read_alerts`, `twai_initiate_recovery`, …) — full rewrite onto the node-based driver, then a complete re-validation of the CAN protocol against a real vehicle |
| cJSON and esp-mqtt moved out of IDF | `network_svc.c` and `mqtt_svc.c` — become managed dependencies to track |
| mbedTLS v4 / PSA Crypto | `esp_https_ota` + ntfy push; HTTPS costs ~800 bytes more stack, and `task_notify` only has 6 KB |
| Newlib → Picolibc | `trace_svc` formats floats via `vsnprintf("%f")`; needs re-verification |
| Warnings become errors, C → gnu23 | Existing warnings become build failures |
| Picolibc size savings | Irrelevant — the app partition is 17% used |

**Revisit when one of these happens**, not when a firmware major version is declared (the
IDF version is invisible to users, so it is not a reason for V4 on its own):
1. ESP-IDF 5.5 reaches end of support (~30-month window from its release)
2. Something needed lands only in 6.x
3. **The next hardware revision** — that already requires full re-validation, so folding
   the migration in costs one validation cycle instead of two

**Git submodules —— 有三個，不是只有 u8g2:**

| Path | Upstream | Notes |
|------|----------|-------|
| `firmware/components/u8g2/` | `olikraus/u8g2` | third-party, never edited here |
| `firmware/components/tes_protocol/` | `a950523a/TES-Protocol` | **our own repo** — `tes_wire.h` (CAN frames + bit macros), `tes_types.h` (application types; includes `tes_wire.h`), `tes_codec.c/.h` live here |
| `firmware/components/psu_link/` | `a950523a/PSU-Link` | **our own repo** — the controller ↔ power-node link (`psu_link.h/.c`), shared with the LianMing PSU Controller |

On a fresh clone:
```bash
git submodule update --init
```

⚠️ Editing `tes_wire.h`, `tes_types.h` or `tes_codec.c` changes the **submodule**, not this repo.
Those changes must be committed and pushed in `firmware/components/tes_protocol/`
first, then the updated pointer committed here — otherwise CI checks out the old
`tes_protocol` and the build breaks on missing symbols. The same applies to
`psu_link/`, which has **two** consumers: bump the pointer here *and* in the LianMing
PSU Controller.

**Why two protocol repos, not one:** TES-Protocol holds only the TES-0D-02-01
vehicle ↔ charger CAN protocol. The power-node link never touches the vehicle, so it
lives in PSU-Link (decided 2026-09-24) — do not move it into TES-Protocol.

---

## V3 Architecture

### Guiding Principle

`tes_protocol/` is **zero-dependency C99** -- no ESP-IDF, no FreeRTOS, no OS calls. It can be compiled on any platform (STM32, PC unit tests, etc.) by swapping `charger_hal/` and `platform/`. All time, GPIO, and CAN operations are injected by the caller.

**Data in, data out — on purpose.** The state machine never calls hardware; it gets an
`inputs` snapshot and returns the `outputs` the hardware should hold. The LianMing PSU
Controller solves the same portability problem the other way — C++ code calling hardware
through a virtual `IHardwareHAL` — and both are right for what they do (compared
2026-09-24):

| | Data in / data out (this SM) | Virtual interface (PSU controller) |
|---|---|---|
| Testing | Build `inputs`, check `outputs`; time is injected, so runs are exactly repeatable | Needs a fake HAL recording calls; tests assert call sequences, not state |
| What will the hardware do? | All of it is in `outputs`, one place | Scattered through the logic |
| Cost | Boilerplate: a new interaction means new struct fields plus glue in `task_tes_sm` | Call it where you need it |
| Fits | Decisions — relays, contactor, what to tell the BMS | I/O — streams, polling, display |
| Latency | One tick (10 ms) between snapshot and action | Immediate |

This code decides whether a relay closes onto a vehicle's HV battery, so testability and
being able to see every hardware action in one struct outweigh the boilerplate. **Do not
"simplify" the SM into calling drivers directly.** A third pattern lives in PSU-Link:
pure encode/decode functions with no I/O and no decisions, which is why the same C file
serves this C firmware and the C++ PSU controller.

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
| `task_hal_poll` | 10 | **6 KB** | 10 ms | button debounce + ADC + PSU poll (incl. ESP-NOW pairing crypto) |
| `task_display` | 4 | 4 KB | 50 ms | OLED render + LED update |
| `task_network` | 3 | 12 KB | 100 ms | WiFi + HTTP server |
| `task_ota` | 2 | 16 KB | event | esp_https_ota |
| `task_notify` | 2 | 6 KB | event | push notification via webhook (v3.1.0) and to the mobile app (Expo) |
| `task_mqtt` | 2 | 8 KB | event + 10/30 s | MQTT publish status + subscribe cmd (v3.2.0) |
| `task_scheduler` | 2 | 4 KB | 30 s | NTP sync (pool.ntp.org, UTC+8) + charging window edge detection → g_btn_event_queue (v3.4.0) |
| `task_monitor` | 1 | 4 KB | 10 s | heap + stack watermark logging |
| `task_log` | 1 | 4 KB | event | charge session history to NVS (v3.1.0); was 3 KB until 2026-10-08 (896 B left mid-charge on hardware) |

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

**STOP during the handshake (2026-10-17).** `stop_requested` — button, `/stop`, app, MQTT all
arrive as it — used to be read only in CHARGING, so a started handshake could only time out
(15/20 s, leaving a fault on screen) or be E-stopped. Now PARAM_EXCHANGE → straight to IDLE
(nothing is closed yet; VP off, PSU current 0) and PRE_CHARGE → ENDING (the vehicle contactor,
and after RELAY_DELAY our relay, may be closed). Both are `STOP_REASON_USER`; the IDLE path
sets `charge_complete_latched` so beta auto-start does not re-fire while CP is still on.
No history record and no push — the session never reached CHARGING.

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

Named macros live in `tes_wire.h` (`V500_FAULT_*`, `V500_ST_*`; reached through `tes_types.h`) — do not use bare hex.

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

**V2 Hardware GPIO (for reference):** buttons (39-42), LEDs (5-7), relays (9-11), CAN (17/18), I2C SDA/SCL (16/15), PSU UART (43/44). ADS1115 at 0x48. CP divider: 150 Ω / 51 Ω.

---

## Hardware — design is private; what the firmware needs is here

**The board design lives in a private repository** —
[`a950523a/TES-Controller-Hardware`](https://github.com/a950523a/TES-Controller-Hardware),
checked out next to this one as `Documents/GitHub/TES-Controller-Hardware` (decided
2026-10-04: hardware is 海龜電能's product, firmware stays public). It holds the KiCad and
EasyEDA projects, Gerbers, JLCPCB BOM/CPL, the enclosure CAD, the logo source and every
`tools/` script, with their history and their own CLAUDE.md (everything that used to be
in this chapter). **This repo publishes the schematic only:**
`docs/schematic/TES_Controller_V1.3_schematic.pdf`, exported from the KiCad project
(`kicad-cli sch export pdf`). Do not add layout, Gerbers, BOM/CPL, enclosure files or
brand assets here again.

Files committed before the split stay in this repo's history — deliberately not
rewritten. They remain CC BY-NC-SA for anyone who obtained them.

Two things here are still generated from the private repo: the board drawing in
`firmware/components/services/web/hw.html` (`tools/make_hw_board_svg.py` writes into
this checkout) and the schematic PDF.

### Board identification on ADS1115 AIN3 (V1.3 onwards)

V1.3 changes the voltage-divider coefficient (30.000 → 38.954), so one firmware binary
tells the boards apart by the voltage on AIN3 — grounded on V1.1/V1.2, the midpoint of
R36/R37 from V1.3 on. The level a future revision uses is chosen in the private repo
(`tools/changes_v13.py`, R36).

| AIN3 reading | Meaning | Divider coefficient |
|---|---|---|
| < 0.9625 V (pin grounded — levels 0–3) | V1.1 / V1.2 — **every unit in the field** | 30.000 |
| level k = round(V / 0.275 V), k = 4…11 | a board revision; **V1.3 = k 6 (R36 = R37 = 10 k)** | per revision |
| > 3.162 V (AIN3 tied straight to VDD33) | extension code: "this board has an EEPROM at 0x50, read it" | from EEPROM |
| between levels | unknown revision | 30.000 + warning |

**Levels 1–3 are reserved — a new revision must not use them.** A V1.2 in the field (2026-10-08)
read 0.431 V on its "grounded" AIN3 (level 2) when booted with the gun in a vehicle, and was shown as
an unknown board with a "voltage readings untrustworthy" warning. CP on the same ADS1115 read 0.00 V,
so it is that pin's ground joint, not the whole ground. Anything under 3.5 levels is now V1.1/V1.2
(with a log warning). That is not a guess: V1.3 sits at 1.65 V, and the unknown-board fallback used
the V1.1/V1.2 coefficient anyway — only the false warning and the missing `/hw` drawing go away.

**A valid factory record overrides AIN3 (2026-10-12).** AIN3 can be misread (a bad joint, as above),
and the dangerous direction is a V1.3 read as an old board: ×30 instead of ×38.954, 77 % of the real
voltage, Stop Mode = Volt overcharges. The factory record is signed and bound to the chip's MAC, so
when `factory_svc` says `FACTORY_VALID`, its `hw_level` decides the board and the coefficient
(`adc_driver_apply_factory_level()`). `main.c` verifies it right after `adc_driver_init()`, **before**
Auto Volt and the SM read any voltage, in a short-lived 8 KB task (ECDSA does not fit `app_main`'s
3.5 KB; on a 5 s timeout it stays with AIN3 and never verifies on the main stack). No record (every
unit in the field), or an invalid one → AIN3 as before. If the record and AIN3 disagree,
`id_mismatch` is set: `/status` `hw_id_mismatch`, `/hw.json` `adc.hw_id_mismatch`, the web UI and the
app say "check R36/R37", `/hw` marks them red, the OLED Board row shows `ID!`. `/status` also carries
`hw_source` (`factory`/`ain3`) and `hw_ain3_level`; `hw_id_level` is the level actually used.

AIN3 itself is now read as the **median of 16 samples**; if the interquartile spread exceeds 50 mV it
retries (3 tries, 50 ms apart; the last try's median is used, with a log warning per unsteady try). First V1.3 (TP-0001, 2026-10-12, USB power
only): AIN3 1.638 V → level 6, factory record valid, no mismatch.

### Firmware side of V1.3

The divider coefficient goes 30.000 → 38.954 and the constant cannot simply be
edited, because deployed units run the old divider and one binary serves both.
The board identifies itself through the AIN3 divider (see **Board
identification** above) and the firmware now reads it — see the next section.

### What V1.3 needs from the firmware — board ID written 2026-09-29, not yet tested on a board

Firmware older than the board-ID change applies a coefficient of 30.000 to a
divider that is 38.954, so **a V1.3 board would report 92 V for a pack sitting at
120 V** (×30.000/38.954 = 77 %).

| | shipped hardware (V1.1 / V1.2) | V1.3 |
|---|---|---|
| ADS1115 supply | 3.3 V | 3.3 V (unchanged) |
| upper arm | 348 kΩ, one 0603 | **115k × 3 = 345 kΩ** |
| lower arm | 12 kΩ | **9.09 kΩ** |
| coefficient | 30.000 | **38.954** |
| reading at 120 V | 4.000 V — **over the input limit** | 3.081 V |
| measurement ceiling | ~108 V, clipped (see 3 below) | 140.2 V |

**1–2. Coefficient lookup and board ID — done in firmware (2026-09-29).**
`ADC_VOLT_R1_KOHM` / `ADC_VOLT_R2_KOHM` are gone. `adc_driver.c` now:

- reads CP as **AIN2 single-ended** (`ADS_CFG_MUX_2G`, MUX 110) instead of
  AIN2−AIN3 — identical on old boards, where AIN3 is ground;
- in `adc_driver_init()`, reads **AIN3 single-ended** (`ADS_CFG_MUX_3G`, MUX 111)
  8 times, averages, classifies with `classify_hw_id()`, and looks the level up in
  `BOARD_REVS[]` (level 0 = V1.1/V1.2 348k/12k, level 6 = V1.3 345k/9.09k).
  **A new board revision = one new row in `BOARD_REVS[]`**, at the level chosen in
  `changes_v13.py`;
- falls back to **30.000** and marks the board unknown for a read failure, a level
  between bands, a level not in the table, or the EEPROM extension code (not
  supported yet). Missing ADS1115 keeps the default too.

Exposed as `adc_driver_board()`: `/status` carries `hw_rev`, `hw_known`,
`hw_id_level`, `hw_id_v`; the web UI prints the board next to the firmware
version (and says the voltage cannot be trusted when `hw_known` is false); the
OLED settings menu has a read-only `Board: V1.3` row under `tes-<id>` (unknown
shows `Board: ? 1.23V`). The classifier was host-tested with every level's
worst-case voltages (two 1 % resistors, 3.3 V ±2 %) — all 13 codes classify
correctly, and a voltage between bands is rejected.

**Not tested on hardware yet.** On a V1.1/V1.2 board: the boot log must say
`level 0 = V1.1/V1.2`, and CP must read exactly as before. On the first V1.3:
`level 6 = V1.3`, `AIN3 ≈ 1.65 V`, and the output voltage must match a meter.

⚠ **Release order matters.** v3.5.0 and older never read AIN3, so a V1.3 board
running them applies 30.000 and **reports 77 % of the real voltage** — and in
Stop Mode = Volt it keeps charging past the target, leaving only the vehicle's BMS
to stop it. The ID-aware firmware must be the **latest GitHub Release before the
first V1.3 ships**, so OTA and the first-flash tool can only give a V1.3 board
firmware that knows it. Only a deliberate manual upload of an old `.bin` can still
downgrade one; ESP-IDF anti-rollback would prevent even that, but it burns eFuses
irreversibly and is not worth it here.

**3. Field units cannot read near 120 V today, and firmware cannot fix it.**
The ADS1115's analog input is limited to VDD + 0.3 V = 3.6 V. At a coefficient
of 30.000, 120 V produces 4.000 V at the pin — past the limit, so the reading
clips. The datasheet limit works out to ~108 V; a V1.2 board measured 114 V.
The discrepancy has never been explained, and it does not need to be: either
way, **readings near full charge on shipped hardware are not trustworthy**, and
raising the coefficient on V1.3 is what fixes it (120 V → 3.081 V, ceiling
140.2 V). Worth remembering when a field unit reports a voltage that stops
rising.

**4. CP's ceiling is 14.2 V and V1.3 did not change it.** R9 = 150 Ω / R8 =
51 Ω and the 3.3 V supply are all untouched, so the limit is 3.6 V × 3.941 =
14.2 V on both revisions. (An earlier note here said it "drops from 20.9 V" —
that was comparing against a 5 V draft of V1.3 that was abandoned, not against
anything that was ever built.) Charging measured 8.99 V, about 1.6× margin;
confirm against a real vehicle before relying on it.

**5. The PGA does not need touching.** ±4.096 V covers 3.081 V with room to
spare, and on both revisions the pin limit binds before the PGA does.

#### Why the hardware changed at all

Two defects, both explained with their reasoning in `tools/changes_v13.py` (private hardware repo):

1. **A single 0603 cannot hold off 120 V.** Its rated working voltage is
   75 V and the upper arm sees 116 V. Three in series drop 39 V each.
   All three are the same value, which is one fewer part number and one
   fewer per-part setup fee at JLCPCB.
2. **The ADS1115 could not read the I2C bus.** V_IH is 0.7 × VDD, so a 5 V
   part needs 3.50 V while the pull-ups only reach 3.3 V. Keeping it at
   3.3 V is what forces the lower arm to 9.09 kΩ, and *that* is what moves
   the coefficient.


---

## Safety Notes

- CAN frames sent to the vehicle **can damage the BMS** -- validate all `tes_codec.c` encode functions carefully before testing on hardware.
- The emergency stop path must never block -- `atomic_bool g_emergency_stop` is checked every 10 ms tick regardless of queue state.
- `g_snapshot_mutex` acquire timeout is 5 ms; callers must handle failure gracefully (skip render, don't block).

---

## Current Status

**v3.5.0 released 2026-09-10.** Fixes the START-crash regression introduced on `dev` (78f88d1) and a batch of diagnostic/UX problems found alongside it. **Vehicle-tested: charging works end to end** (`IDLE → PARAM_EXCHANGE → PRE_CHARGE → CHARGING`). `idf.py build` zero errors. **該版由 ESP-IDF v5.5.1 建置**；v5.5.5 是之後才升的，要解 v3.5.0 韌體的 backtrace 需 `git checkout v5.5.1`。

**v3.5.0 fixes — the two crashes:**
1. **START → instant reboot.** `task_can_rx` stack overflow, *not* `task_tes_sm`. 78f88d1 added `can_driver_service()` to that task's loop; pressing START starts 0x508/0x509 TX, no ACK on the bus → TWAI error-passive → `ESP_LOGW` inside a 2 KB task → overflow. Stack raised to 4 KB (headroom 52 → 2100 bytes idle, 1860 charging).
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

**Confirmed TES-0D-02-01 protocol timing (commit 5bb8887):** `VP ON → CP ON → CAN 0x500 bit0=1 → charging → CAN ends → CP OFF`. CP appears before CAN; CP OFF→ON edge is the primary auto-start trigger, CAN rising edge is backup.

**ESP-NOW PSU transport (implemented 2026-05-16, ✅ hardware-tested 2026-09-15):** `psu_driver` supports dual transport (UART + ESP-NOW). `POST /psu/pair` added to REST API. The LianMing PSU Controller side has been updated to match, and the pair → publish → command path has been exercised on real hardware.

**PSU disconnect fault fix (commit bb1735e, 2026-05-22):** `run_monitoring()` no longer faults on PSU disconnect unconditionally. `psu_session_connected` snapshot taken at `PRECHARGE_STEP_COMPLETE` — mid-charge disconnect only faults if PSU was present at session start; PSU-absent-at-start = ADC-only mode, charging continues uninterrupted. Fixes auto-start + PSU-less testing.

**Hardware V1.3 (2026-09-15):** the EasyEDA → KiCad migration is finished and the
board passes every check it has — netlist zero-difference, ERC clean, DRC 0 errors
and 0 unconnected. Gerbers are the only step left, and nothing has been ordered.
Details, and what is deliberately left undone, in the private hardware repo's CLAUDE.md.
Note that V1.3 is **not compatible with the shipped firmware constant**: its divider
reads 38.954, deployed units read 30.000.

**⚠️ On `main`, in no release yet.** Eight commits from 2026-09-11 to 09-13 sit on
`main` above the v3.5.0 tag. Three of them change how the device behaves, so anything
built from `main` is **not** what a deployed unit is running:

| | Consequence |
|---|---|
| CSRF protection on the seven state-changing endpoints | `curl` and scripts must send `X-TES-Request: 1` or get 403 — including `POST /ota/upload` |
| MQTT TLS + `mqtt_cmd` opt-out | new NVS key, defaults to true (see **Security**) |
| ESP-IDF v5.5.5 | CI and the dev machine; v3.5.0 itself was built with v5.5.1 |

Pushing `main` deploys GitHub Pages, which republishes `tes_charger_flash.bin` on the
public first-flash tool — so a push is what actually puts these in front of users, tag
or no tag. (It did, on 2026-09-15: main now builds green and the published binary
carries the CSRF header requirement. The Releases the OTA button pulls from are still
v3.5.0, which does not — only re-flashed units have it.)

**`main` carries firmware only; the V1.3 hardware work stayed on `dev`** (decided
2026-09-15) — and since 2026-10-04 the hardware is not in this repo at all (see
**Hardware** above). Do **not** fast-forward `main` to `dev` — pick the firmware commits across
deliberately, as was done for the eight above and for the CI fix (`ff3a831`, a
cherry-pick of `0794ad3`). That cherry-pick means the branches have diverged, so bringing
`dev` over later needs a merge commit rather than a fast-forward; the workflow file is
already identical on both sides, so it will not conflict.

**Git history was rewritten on 2026-09-29 — every commit hash before that date
changed.** The twelve prototype photos in `docs/images/` (added 2025-08-04) carried
the phone's GPS position of the author's home. `git filter-repo` replaced each
original in history with its resized, EXIF-free version, then `main`, `dev` and all
32 tags were force-pushed. Trees at the branch tips are byte-identical to before;
the repo went from 67 MB to 29 MB. The original commits were GPG-signed and
filter-repo drops signatures, so **all 355 commits got new hashes**, not only those
after the photos. Hash references in this file and in commit messages were
translated through filter-repo's commit map; a hash quoted anywhere else points at a
commit that no longer exists on `main`/`dev`. A clone made before that date must be
re-cloned, not pulled. One fork (2025-08-14) predates the rewrite and still holds the
originals — the author has decided not to pursue it. Any new photo goes in with EXIF
stripped.

**Development resumed on 2026-09-23** — firmware and the V1.3 hardware both.
It had been paused since 2026-09-15 over a patent question (below). The pause
notice is gone from README.md; the top of README now points to the ready-made
products instead.

**Publication status — decided 2026-09-23: no patent; the repository stays
public** (it has been public since 2025-06-19). The TES market is too small for a
patent to be worth pursuing. README.md's licence section was cut down to the
licence itself: CC BY-NC-SA 4.0 licenses copyright only — its own §2(b)(2) says
patent and trademark rights are not licensed — so publishing under it grants
nobody a patent licence. The "reserving the right to file" wording and the
not-yet-filed disclaimer were removed along with the plan. Do not reintroduce
"Patent Pending" or similar wording; if the question ever reopens, it is one
for a patent attorney, not for this file.

**PSU link protocol v2 (2026-09-24) — on `dev` only, not hardware-tested.** The
power-node link was rewritten end to end: codec in the new
[PSU-Link](https://github.com/a950523a/PSU-Link) repo (70 host tests), `psu_driver` here
(`ed99d3b`), `SerialCmd` in the LianMing PSU Controller (`9c2e6f8`). Both firmwares
build under IDF 5.5.5 and CI is green, but **no frame has crossed a real wire yet**.
Deliberately **not** cherry-picked to `main`: that would republish the public flash
binary. Pick `ed99d3b` across after this checklist passes on real boards:

1. UART: TES receives `$CAP` → `psu_status_t.caps_known` true, `node_type` 1
2. `$ST` every 1 s idle / 100 ms outputting; V/I match the PSU's own display
3. `$SET` from TES → `$ACK,<seq>,0` → the output actually changes
4. Repeat 1–3 over ESP-NOW (re-pair first — see the PSU repo's partition note)
5. `rx_crc_errors` stays 0; a rising count means wiring or baud rate

**Next: a measurement-only node for knob power supplies** (the ones with no digital
interface — the iE125 retrofit and the 15 A charger). Decided 2026-09-24:

- It talks over the **existing UART / ESP-NOW link**, not I2C — the board design is
  fixed, and ESP-NOW gives galvanic isolation for free. It declares
  `PSU_CAP_REPORT_V | PSU_CAP_REPORT_I` and no set capability, so `SET` never reaches it.
- It measures voltage too, with its own divider — which also sidesteps the V1.1/V1.2
  ADS1115 ceiling (~108–114 V, see **What V1.3 needs from the firmware**).

Why it matters — **today, with no PSU link, the controller does not know the output
current at all.** `tes_sm.c` fills 0x509 `actual_current`, the live display and the
energy estimate with the **`max_current` setting** (`tes_sm.c` ADC-only branches, and
0x508 `available_current` from the same setting). That is why users have to set Max
Current to match the knob by hand. The SM changes, in priority order (none written yet):

1. **Over-current stop** — actual current above the BMS request by more than a tolerance
   for a sustained time → stop. A knob PSU cannot be commanded, so when the BMS tapers
   its request the PSU keeps pushing; today nothing notices. This is the reason to do it.
2. **0x509 reports the measured current** instead of the setting.
3. **0x508 available current follows the knob** (BMS asks for more than is measured,
   sustained → the PSU is current-limited; available = measured). Needs a vehicle test:
   it is unknown whether the vehicle re-reads 0x508 during CHARGING, or faults when the
   delivered current stays below its request.

Open, not decided: sensor type (Hall recommended — isolated, retrofit by passing the wire
through; whatever it is, its output must stay under the ADC pin limit, the same trap as
V1.2); where the node gets power; whether it reuses the LianMing ESP32 board; and what
happens if the node drops mid-charge (recommended: stop, matching the
`psu_session_connected` rule). **Hard constraint:** with no node present, behaviour must
stay exactly as today — including reporting the setting as current — because that is
every unit in the field.

**In progress:** React Native mobile app (Expo + EAS Build, Android APK sideload). Will support multiple controllers, local HTTP + MQTT remote, guided onboarding. Not yet started.

**✅ Vehicle-verified as of v3.5.0:** manual START → full charge sequence; CP transition
0 V → 8.99 V; `trace_svc` session start; web UI latency (measured with curl, see above).

**✅ Confirmed on hardware 2026-09-15** — everything v3.5.0 shipped without having been
exercised has since been checked, and the ESP-NOW PSU transport with it:
- **Fault display persistence** (OLED / LED / web) — the reason now holds instead of the
  screen reverting to Standby when FAULT auto-recovers.
- **Fault reason in charge history** — `/history` renders through the same `FAULTS[]`
  table as the live panel, so a fault stop reads "充電槍鬆脫" rather than a bare "故障".
- **`Reset Fault` in IDLE** — the menu item does something in that state now.
- **ESP-NOW PSU transport**, against the updated LianMing PSU Controller.

The pass/fail was recorded, the procedure was not. If any of these is ever suspected
again, the repro steps have to be rebuilt from scratch — worth writing down next time.

**⚠️ Still not vehicle-tested:** notify_svc, push_svc (mobile-app push), PWA offline caching, log_svc, WiFi scan,
mDNS AP mode, MQTT, Cloud PWA, power/energy tracking, CAN diagnostics panel, charge
timer stop, scheduler, beta auto-start.

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
| Max Voltage | 40.0 V -- 120.0 V，**V1.1/V1.2 只到 110.0 V**（顯示為 "V Cap" when Auto ON） | ±0.1 V | ±1 V (auto-repeat) |
| Max Current | 1.0 A -- 100.0 A，**接上會回報額定電流的電源時最多到那裡**（LianMing 66 A；`psu_driver_rated_current_01a()`，網頁／App 用 `/config` `current_limit`，`POST /config` 超過就夾住） | ±0.1 A | ±1 A (auto-repeat) |
| Stop Mode | SOC / Volt / Timer / kWh | toggle | toggle |
| Target SOC | 20% -- 100%（Stop Mode=SOC 時顯示） | ±1% | ±5% (auto-repeat) |
| Stop Voltage | 40.0 V -- 120.0 V，**V1.1/V1.2 只到 110.0 V**（Stop Mode=Volt 時顯示） | ±0.1 V | ±1 V (auto-repeat) |
| Charge Timer | 1 min -- 600 min（Stop Mode=Timer 時顯示） | ±10 min | ±30 min (auto-repeat) |
| Stop E | 0.1 -- 20.0 kWh（Stop Mode=kWh 時顯示） | ±0.1 kWh | ±1 kWh (auto-repeat) |
| LuxBeacon | ON / OFF | toggle | toggle |
| WiFi Info | read-only display（連著 WiFi 顯示 `IP:`，只有熱點時 `AP:`） | — | — |
| AP pass / 熱點密碼 | 自動熱點的 WPA2 密碼（唯讀） | — | — |
| Scheduler | ON / OFF | toggle | toggle |
| [Beta] Auto | ON / OFF | toggle | toggle |
| Language | 中文 / English（兩種都用本身的語言寫，選錯也認得回去） | toggle | toggle |
| Charge anim | All / Battery / Arrows / OFF | next / previous | same |
| Screensaver | 5 / 15 / 30 min / OFF | next / previous | same |
| Saver style | Turtle / Dark（Screensaver=OFF 時不顯示） | toggle | toggle |
| Reset Fault | 手動復歸緊急停止 | confirm | — |
| Restart | 重新啟動：**按兩次**（第一次顯示 `Restart? press again`，3 秒內再按才執行，移動游標就取消）；充電流程中顯示 `Restart (stop first)`、不可用。未儲存的設定變更會丟掉 | confirm ×2 | — |
| About | 韌體版本 + 作者（唯讀） | — | — |
| Board（位於 `tes-<id>` 下一行） | 硬體版本（唯讀，開機時由 AIN3 辨識；不認得顯示 `? 1.23V`） | — | — |
| Save & Exit | writes to NVS | — | — |
| Cancel | discard changes | — | — |

OLED status screen:
- **SOC 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72/95%  1h23m`
- **Volt 模式**：第二行 `54.2V/100.0V`（即時/目標電壓），第三行 `SOC:72%  1h23m`
- **Timer 模式**：第二行 `54.2V  12.3A`，第三行 `SOC:72%  1h23m/2h00m`（已充時間/目標充電時長）
- **kWh 模式**：第二行 `54.2V  12.3A`，第三行 `0.85/1.5kWh  1h23m`（已充入/目標＋剩餘時間；沒接 PSU 時 `~0.85/1.5kWh`，≥10 kWh 改一位小數）。SOC 放不下，網頁與 App 有

### OLED: Chinese, animations, screensaver (2026-10-17)

**Language** — NVS `oled_lang` (0 中文, default; 1 English). Every Chinese string is written
`ZH("...")` (a no-op macro in `display_driver.h`); `firmware/tools/gen_oled_font.py` collects
them and runs u8g2's `bdfconv` (compiled from the submodule on first use) to write
`components/drivers/oled_font_zh12.c`: printable ASCII + only the ~180 characters used,
~17 KB. **Re-run it after adding or changing a `ZH()` string** — a missing character draws as
nothing, with no build error. The source font is **efont b12** (12 px, Unicode incl.
Traditional Chinese, `u8g2/tools/font/bdf/b12.bdf`, BSD-style licence). Layout: Chinese is
12 px, 10 characters per line; numbers (V, A, kWh, SOC) keep the English fonts in both
languages. `L(en, zh)` and `text()`/`header()` in `display_svc.c` pick per language; the
English screens are unchanged. The bench-test screen stays English.

**Animations** (display task, every 50 ms, 100 ms per frame):

| Where | What | Setting |
|---|---|---|
| Boot (Auto Volt on) | lightning blinks, dots after 偵測電壓 / Voltage cycle — `main.c` redraws every 100 ms while `auto_volt_boot_step()` runs | always |
| CHARGING | battery icon top right fills from the SOC segment upward; a row of `>` under the voltage line flows right (lines 3 and 4 move down 8 px) | NVS `oled_anim`: 0 both (default), 1 battery, 2 arrows, 3 off |
| Charge complete | IDLE + `led_state == COMPLETE`: check mark drawn in 1.2 s, then SOC / duration / kWh of that session (latched while CHARGING — the snapshot zeroes them in IDLE). Shown for **1 minute, or until any key is pressed** (the user found "press SETTING to go back" awkward); a SETTING press used for that does not also cycle the SOC preset. It returns to the normal standby screen, which shows the charge target (`SOC:0/95%`); replacing that line with a last-session summary was tried and dropped — the user wants to see the settings | always |
| IDLE | whole screen offset by 1 px, four positions, one per minute (`display_driver_set_offset()`) — burn-in | always |
| Fault | never animated | — |

**Screensaver** — NVS `ss_min` (0 off, 5 default, 15, 30) and `ss_style` (0 turtle, 1 dark).
Entered in IDLE (standby, completion or a fault left on screen) after `ss_min` minutes with no
activity: button state, SM state, CP state, menu, pairing, bench test. Shows a two-line block
— state + clock (once NTP has synced), PSU voltage + "按鍵喚醒 / any key" — that moves corner
every minute; "turtle" adds a small turtle swimming across with bubbles. **The key that wakes
it does nothing else:** `task_hal_poll` swallows START/STOP/SETTING pressed while
`g_screen_saver` is set until released and raises `g_screen_wake`; E-STOP is never swallowed.
Any SM change (plug-in, remote start, fault) wakes it by itself.

All four settings: OLED menu (Language / Charge anim / Screensaver / Saver style, after
`[Beta]Auto`), `GET/POST /config` (`oled_lang`, `oled_anim`, `ss_min`, `ss_style`), the web
UI's 🖥 螢幕 card and the app's 螢幕 page. Web and app hide them on firmware that lacks the
fields.

**Checking the screens without hardware:** compile `display_svc.c` + `display_driver.c` +
u8g2 + the font on the PC with stub headers and dump `u8g2_GetBufferPtr()` (remember the
panel is mounted with `U8G2_R2`: rotate 180° to get what the user sees). That is how the
2026-10-17 layouts were checked; the harness lives outside the repo.

Web UI voltage/SOC display mirrors OLED: Volt mode shows `voltage / stop_voltage`; SOC mode shows `soc% / target_soc%`; Timer mode shows single voltage and reference SOC only.

---

## PSU Protocol

**Link protocol v2 (2026-09-24), defined in the `psu_link` submodule**
([PSU-Link](https://github.com/a950523a/PSU-Link)) — `include/psu_link/psu_link.h` +
`psu_link.c`, with host tests in `test/`. The LianMing PSU Controller pulls in the
**same submodule**, so the wire format has exactly one definition: change it in PSU-Link,
then bump the submodule pointer in **both** repos. Same bytes over UART and ESP-NOW:

```
$<TYPE>,<field>,...*<CRC16>\n     CRC-16/CCITT over the bytes between '$' and '*'
```

| Direction | Message | Notes |
|---|---|---|
| TES → node | `$HELO,<ver>` | sent every 1 s while connected but `caps_known == false` |
| node → TES | `$CAP,<ver>,<type>,<caps>,<vmax>,<imax>,<fw>` | at node boot and on `HELO` |
| node → TES | `$ST,<seq>,<v>,<i>,<mode>,<flags>` | 100 ms outputting / 1 s idle — **also the heartbeat** (no `HB` any more) |
| TES → node | `$SET,<seq>,<v>,<i>` | either field may be empty = unchanged; throttled as before |
| node → TES | `$ACK,<seq>,<result>` | `PSU_ACK_RANGE` / `UNSUPPORTED` are logged |

Integers only, 0.01 V / 0.01 A. Lines not starting with `$` (the node's boot banner,
replies to human text commands) are ignored; CRC failures and unusable frames are
counted in `psu_status_t.rx_crc_errors` / `rx_bad_frames` — a rising CRC count is a
wiring or baud-rate problem.

**Node types.** The protocol describes a *power node* by what it declares in `CAP`, not
by brand: a controllable rectifier (LianMing: report V/I + set V/I) or a
**measurement-only node** for power supplies with no digital interface (report V/I, no
set). `psu_driver_can_set_voltage/current()` stop `SET` going to a node that cannot take
it; before `CAP` arrives they return true and an unsupported node answers `ACK 2`.
**The state machine does not use node type or capabilities yet** — that is the next
step (measured current in 0x509, following a manual current knob, over-current stop).

**Measured current (2026-10-17).** `tes_sm_inputs_t` gained `current_measured` / `measured_current` /
`psu_commandable`. `task_tes_sm` sets `current_measured` when the node declares `PSU_CAP_REPORT_I` and the
latest `ST` has `PSU_ST_I_VALID` — LianMing, or a current-only **meter node** (`PSU_NODE_METER`, planned as
an optional UART add-on for knob power supplies: reports current only, ≤ 15 A, rated current 15 A in `CAP`,
so Max Current is capped at 15 A by the mechanism below). With it:
- 0x509, the live display and the session energy use the measured current (no PSU / not measured: as before —
  PSU current when the PSU reports a voltage, else the Max Current setting).
- **Hard over-current**: measured > Max Current + 1.5 A for 1 s → `FAULT_SRC_OVER_CURRENT` (0x508 bit1, device
  abnormal). Only when the PSU cannot be commanded (no `PSU_CAP_SET_I`) — a commanded PSU's current is ours.
  Measured above the vehicle's request is **not** stopped; the vehicle decides.
- `isense_check` (`services/isense_check.c`): vehicle asks ≥ 2 A but the measured current stays < 0.3 A for
  10 s (a shunt wired backwards reads 0) → the reading is distrusted until reboot (`/status` `isense_suspect`).
- Output voltage everywhere goes through `output_voltage()`: PSU voltage only when it is > 0, else the ADC. The
  over-voltage end check used to take the PSU voltage whenever the PSU was connected, so a current-only node
  (voltage 0) would have disabled it.
Nothing about this is in the settings yet (decided 2026-10-17); `/status` has `current_measured`. The OLED,
web `FAULTS` and the app's `faults.ts` describe fault 10. Host-tested in the session that wrote it.

**Except the current ceiling (2026-10-17).** `psu_driver_current_cap_01a()` = min(the node's
rated current from `CAP` `imax`, its available power from `PWR` `pmax` ÷ output voltage). The
voltage is the node's measured output while it is outputting, else Max Voltage (the VLIM2 sent to
the vehicle) — so a low-voltage vehicle gets more current, and the 0x508 figure during the handshake
is conservative. `task_tes_sm` feeds `min(Max Current setting, cap)` into the SM as
`max_current_01a`, updated only when it moved by 0.5 A so 0x508 does not jitter; no PSU, or a node
that reports neither → the setting as before (every unit in the field). The node clamps again on its
side with its own measured voltage. `/status`: `psu_ac_v`, `psu_pmax_w`, `psu_ac_limit_a`,
`psu_ac_src` (0 unknown, 1 outlet setting, 2 AC charging station — reserved for J1772 / Type-2 on
the node), `psu_power_current`, `psu_power_limited`; the web status card and the app show them as
可用功率. The **Max Current setting** itself can only go up to the node's rated current (`current_limit` in `/config`; decided 2026-10-17 — the power limit stays automatic, so a low-voltage vehicle still gets more). LianMing: 1.3 kW on 110 V, 6 kW on 220 V (by band), outlet limit (default 15 A) set on the LianMing,
module 60 A rated / 66 A max — see that repo's **Available power**.

**`psu_status_t` kept its first five fields** (`voltage`, `current`, `connected`, `rssi`,
`fail_streak`) with the same meaning, so `task_tes_sm`, `display_svc` and `network_svc`
did not change. Everything after them (node identity, last `ST`, `SET`/`ACK` sequence,
link counters) is new. Node identity is cleared on disconnect — the next node may be a
different one.

**Invalid readings are reported as 0.** `ST` flags voltage/current invalid when the node
is not outputting (LianMing: below 1 V, the old `V=`/`HB` boundary), and the driver
turns that into 0. That matters: when `psu_voltage == 0` (PSU standby), SM falls back to
ADC voltage for OLED display and 0x509 CAN output — prevents vehicle from seeing 0V/0A
and aborting. A node that reported its idle noise as a valid 0.3 V would silently break
that fallback.

**Units in the field never see any of this.** They run knob power supplies with nothing
on the PSU UART, so no frame ever arrives, `connected` stays false and the SM stays on
the ADC-only path exactly as before. The driver only sends `HELO` once something is
talking, so an empty UART stays silent.

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

**ESP-NOW pairing and link authentication (2026-09-30, on `dev`, not hardware-tested).**
The old flow — TES saved the MAC of the first `PSU_HELLO` it heard, nobody confirmed
anything — could pair with the wrong unit, and nothing stopped a forged `$SET`. Both
halves now live in the PSU-Link submodule (`psu_pair`, `psu_sess`; design and attack
tests there), shared with the LianMing PSU Controller:

1. **Pairing = Bluetooth-style numeric comparison.** `POST /psu/pair` (IDLE only — X25519
   runs in `task_hal_poll` for tens of ms, and START/STOP are repurposed) starts it; the
   PSU broadcasts its X25519 key, TES replies, the PSU commits to its nonce before seeing
   TES's, and both derive the same **6-digit code**. OLED shows it large (`render_pair()`
   in `display_svc`, overriding even the settings menu) and `/control` pops a dialog.
   The user confirms on **both** devices: TES by START (STOP = cancel) or the dialog's
   button (`POST /psu/pair/confirm`), the PSU on its own buttons. Only then do both
   store the peer MAC and a 32-byte long-term key. A wrong unit or a man in the middle
   shows a different number; the commitment keeps an attacker's odds at 10⁻⁶.
2. **Every ESP-NOW frame is authenticated.** ESP-NOW's own LMK encryption does not stop
   forgery — broadcast and unencrypted unicast are always delivered and the callback
   cannot tell them apart — so it is not used. Instead a 3-message handshake derives a
   fresh connection key from the LTK each time either side starts, and every line
   carries `~<counter><HMAC tag>`. `handle_espnow_line()` in `psu_driver.c` passes only
   verified lines on; without a tag it accepts only pairing and handshake messages.
   Forged, altered, reflected and replayed frames are dropped and counted
   (`psu_status_t.auth_rejects`). UART is a wire and is not authenticated.

`s_sess` is shared between `task_hal_poll` (receive, handshake) and `task_tes_sm` (SET),
so it sits behind a mutex; sending happens inside the lock so counters leave in order.
`task_hal_poll`'s stack went 4 → 6 KB for mbedTLS ECP.

**Breaking change, by decision:** a TES on this firmware only pairs with a LianMing
running the matching firmware, and an existing pairing (MAC only, no key) becomes
`needs_repair` — the UI says so and asks for a re-pair. Acceptable because ESP-NOW has
not shipped on `main`; nobody in the field depends on the old flow. A legacy
`PSU_HELLO` seen during pairing is reported as "PSU firmware too old".

**To verify on hardware:** both screens show the same code; confirming on one side
alone times out after 60 s with nothing stored; the MAC the PSU sees for TES matches
`esp_wifi_get_mac(WIFI_IF_STA)` (otherwise key confirmation fails — safe, but pairing
never completes); after pairing, `auth_rejects` stays 0 and `link_auth` is true;
rebooting either side re-establishes the link within ~2 s; `hal_poll` stack headroom
during pairing.

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
| `stop_e` | uint32 | 1000 | Energy stop target (Wh, 100–20000), stop mode 3 |
| `oled_lang` | uint32 | 0 | OLED language: 0 中文, 1 English |
| `oled_anim` | uint32 | 0 | Charging animation: 0 battery+arrows, 1 battery, 2 arrows, 3 off |
| `ss_min` | uint32 | 5 | Screensaver delay in minutes: 0 (off), 5, 15, 30 |
| `ss_style` | uint32 | 0 | Screensaver style: 0 turtle, 1 dark |
| `notify_url` | str[128] | "" | ntfy/webhook URL; empty = disabled |
| `push_v2` | blob[4×100] | empty | Mobile-app phones: Expo push token, name, registration time (`push_svc`, not in `charger_config_t`; migrated from the older token-only `push_toks`) |
| `push_lang` | blob[4] | all 0 | Notification language per `push_v2` slot: 0 中文, 1 English (missing = all Chinese) |
| `chg_mark` | u32 | 0 | Charging-in-progress mark for the post-reboot notice: `0x100 \| start SOC`, 0 = not charging |
| `mqtt_url` | str[128] | "" | MQTT broker URL; empty = disabled |
| `mqtt_topic` | str[64] | "" | MQTT topic prefix |
| `sched_en` | uint8 | 0 | Scheduler master switch |
| `sched_start` | uint16 | 0 | Start time (minutes from midnight) |
| `sched_stop_en` | uint8 | 0 | Auto-stop enable |
| `sched_stop` | uint16 | 360 | Stop time (minutes from midnight, default 06:00) |
| `auto_s` | bool | false | Beta auto-start |
| `psu_trans` | uint32 | 0 | PSU transport: 0=UART, 1=ESP-NOW |
| `mqtt_cmd` | bool | **true** | 允許透過 MQTT 遠端啟停。false = 只發佈狀態、不訂閱 cmd。預設 true 是相容考量，見 Security 一節 |
| `psu_mac` | blob[6] | — | ESP-NOW peer MAC (PSU 的 MAC 地址，配對後寫入）|
| `psu_ltk` | blob[32] | — | ESP-NOW 配對的長期金鑰。**秘密**：不進 `charger_config_t`（那個結構會被 `GET /config` 整包送出），只經 `config_svc_get_psu_ltk()` 讀。有 `psu_mac` 沒有 `psu_ltk` = 舊版配對，需重新配對 |
| `sta_en` | bool | **true** | false = 固定 AP 模式但保留 SSID／密碼。預設必須為 true，否則 OTA 上來的舊機器會全部掉進 AP 模式 |
| `wifi_nets` | blob[485] | — | 記住的 WiFi（`wifi_list_t`，最多 5 組，最新的在前）。沒有就從 `wifi_ssid`／`wifi_pass` 搬；第一組會寫回那兩個鍵 |
| `ap_pass` | str[64] | 隨機 10 碼 | 連不上 WiFi 時自動熱點的 WPA2 密碼（首次設定的熱點仍不加密） |
| `dev_name` | str[24] | "" | 裝置顯示名稱；空 = 顯示 `TES Charger <id>`。不影響主機名 |
| `sess_seq` | uint32 | 0 | (namespace `tes_hist`) 充電 session 流水號，供 trace_svc 使用 |

Charge history: namespace `"tes_hist"`, blob key `"log"` (**564 bytes**, 20 × `charge_record_t` circular buffer; the 484-byte pre-2026-10-17 format is migrated on boot). Same namespace: `"energy"` (580-byte monthly totals, `energy_book_t`), `"eff"` (u32, PSU efficiency % for the cost estimate, default 90) and `"price"` (u32, NT$/kWh × 100, 0 = no cost estimate). See Charge Session History.

---

## Feature Summaries

### Auto-Voltage
NVS key `auto_v`. Overrides `max_voltage` in RAM only (NVS not written), 40 V up to the board's limit. Logic in
`services/auto_volt.c` — pure, data in / data out (2026-10-06; it used to wait 1 s and read once):

- **Boot** (`main.c`, boot logo "Auto Setting Voltage..." on the OLED): sample every 100 ms, use
  1-second averages, stop when the average rose < 0.3 V over 2 s, take the highest average;
  at most 10 s. The controller is powered by the PSU itself, so the PSU is often still ramping
  when it boots — a single read caught a low value.
- **IDLE** (`task_tes_sm`): keeps sampling while there is no load and raises `max_voltage`
  when the 1-second average is ≥ 0.2 V above it. Only ever up; samples are dropped on leaving
  IDLE so charging voltage never gets in.
- **Never changes during a charging session** (decided 2026-10-06). The step runs *after*
  `tes_sm_tick()` and checks the post-tick state: the override only reaches the SM on the next
  tick, so running it before the tick let a value computed on the START tick land in the first
  PARAM_EXCHANGE tick — VLIM2 moving mid-handshake. This covers Auto Volt only; a Max Voltage
  typed by the user is still applied immediately, as before.
- **Why averages, not the raw maximum:** the value goes out as VLIM2 in 0x508, the vehicle's
  own over-voltage threshold. A spike read as the maximum would loosen that protection; a
  low value is the safe direction (worst case a "voltage too low" fault).

**Per-board voltage ceiling (2026-10-15).** V1.1/V1.2 read at most ~114 V (see **What V1.3 needs
from the firmware** §3), so on those boards Max Voltage, Stop Voltage and Auto Volt stop at **110 V**
(4 V of margin); V1.3 keeps 120 V. The value lives in `BOARD_REVS[].max_01v` (`adc_driver.c`) and is
exposed as `adc_driver_board()->max_volt_01v`, `/config` and `/status` `voltage_limit`. A stored value
above it (old boards could be set to 120 V) is clamped and written back once at boot (`main.c`);
`POST /config` caps 110–120 V at the limit instead of dropping the field, and the OLED menu and the web
inputs simply stop at it — no extra message, by the author's choice. Auto Volt treats a reading up to
2 V over the limit as read error and uses the limit; **more than 2 V over** means the supply itself is
set too high: the reading is not used and is reported as `/status` `psu_over_volt` (V, 0 = none), shown
on the web status card, in the app, and on the OLED's bottom line in IDLE (`!PSU 113.4V>110V`). It is
re-evaluated every second in IDLE, so turning the knob down clears it.

### Stop Mode
Four mutually exclusive termination conditions (NVS key `stop_m`). SM checks in `run_monitoring()`:
- `STOP_MODE_SOC=0`: BMS SOC ≥ target_soc
- `STOP_MODE_VOLTAGE=1`: output voltage ≥ stop_voltage_01v/10.0 (NVS key `stop_v`)
- `STOP_MODE_TIMER=2`: charge_elapsed_ms ≥ charge_timer_min × 60000 (NVS key `timer_m`)
- `STOP_MODE_ENERGY=3` (2026-10-17): energy delivered this session ≥ `stop_energy_wh` (NVS key
  `stop_e`, Wh, 100–20000, default 1000) → `STOP_REASON_ENERGY` (7), counted as a completed charge
  (green LED). The energy is `task_tes_sm`'s V×I sum, passed in as `session_energy_wh`. **Without a
  PSU link it is an estimate** (current = Max Current setting), and when the BMS tapers it reads high,
  so the session may stop early — never late; SOC 100 % and the BMS still stop it. The web UI and app
  say so, the OLED prefixes the number with `~`. Both enum values were appended to TES-Protocol
  (`eb72b20`). REST/MQTT carry `stop_energy` in **kWh** (one decimal).

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

**Mobile app push (`push_svc`).** Every event above goes through `notify_svc_broadcast()`, which sends to
`notify_url` *and* to every phone registered by the mobile app. The app registers its Expo push token
with `POST /push {"op":"add","token":"ExponentPushToken[...]","name":"<phone>"}` (CSRF header required; at
most `PUSH_MAX_TOKENS` = 4 phones, the 5th gets 409). Each entry stores the token, a phone name and the
registration time (Unix seconds, 0 if NTP had not synced) in one NVS blob `push_v2`, **not** in
`charger_config_t` — that struct is serialised by `GET /config`, which only reports `push_tokens` (a count).
The older token-only blob `push_toks` is migrated on boot. **Managing the phones:** `GET /push` lists
`{slot,name,added,tail}` — `tail` is the last 6 characters inside the brackets, so the app can recognise
"this phone" without the firmware ever returning a full token; `{"op":"remove_slot","slot":n}` removes
another phone, `{"op":"remove","token":...}` this one. Names are sanitised on the way in (no `"`, `\`
or control characters, cut on a UTF-8 boundary) so they can go into JSON unescaped.
Sending is one HTTPS POST per phone to `https://exp.host/--/api/v2/push/send` with
`channelId: "charging"` (the app creates that Android channel; keep both sides in sync). A response
containing `DeviceNotRegistered` removes that token. Buffers are heap-allocated because `task_notify`
has 6 KB of stack — check its `stack free` in `task_monitor` after the first real push.
A token can only push to the phone it came from, so it is not treated as a secret.

**Each phone gets its own language (2026-10-17).** `POST /push` `add` takes `"lang":"zh"|"en"` (the app
sends the language it is displaying, and re-registers when that changes); omitted = unchanged, a new
phone without it is Chinese. It is kept per slot in a separate 4-byte NVS blob `push_lang`, so
`push_v2` kept its format and a unit upgrading by OTA simply has every phone on Chinese. `GET /push`
reports `lang` per phone. Every `notify_svc_broadcast()` call passes both texts (`title_zh, msg_zh,
title_en, msg_en`), and `boot_notice_build()` takes an `en` flag and is built once per language at
boot. `notify_url` (webhook / ntfy) has no language and stays Chinese. Data-only pushes (live progress)
carry no text, so they are unaffected — the app formats those in its own language.

**Notice after a reboot (`boot_notice.c`, host-tested).** A panic, watchdog, brownout or power cut gives the
firmware no chance to say that charging stopped. So `task_notify` writes NVS `chg_mark` (`0x100 | start SOC`)
when charging starts and clears it when charging ends normally — one write each per session. On boot
`notify_svc_init()` reads the mark and `esp_reset_reason()`: mark set → "充電中斷" with the reason (any
reason, including power loss) plus a `live_end` so the app drops its stale Now Bar progress; no mark but
panic/watchdog/brownout → "控制器異常重啟"; otherwise nothing. It is sent once the network is up (retried
every 5 s), then the mark is cleared — unless a new session has already started and written its own.

**Live charging progress for the app (Android Now Bar / Live Updates).** While CHARGING, `task_notify`
wakes every 5 s (`LIVE_POLL_MS`) even without events and sends a *data-only* push (no title/body,
`ttl` 300 s) to registered phones — `{"k":"live","id","m","p","soc","tsoc","v","sv","el","tm","rem","w","e","se"}`
(`e`/`se` = delivered / target Wh, for stop mode 3),
voltages in 0.1 V, all integers so no `%f` on the 6 KB stack. `p` is the progress the user asked for,
**by stop mode**: SOC → soc ÷ target, voltage → from the voltage at charge start to the stop voltage,
timer → elapsed ÷ set time, energy → delivered ÷ target (`live_progress.c`, pure C, host-tested). A push goes out when `p` changes,
at most once per 30 s (`live_should_send`); leaving CHARGING sends `{"k":"live_end","id","r"}` so the
app clears its ongoing notification. The app builds the notification itself in a background task — the
firmware never sends display text for it, and `notify_url` does not get these.

### Charge Session History
`task_tes_sm` accumulates V×I during CHARGING (`energy_wh += V*A/360000.0f` per 10 ms tick). Publishes `EVT_SESSION_COMPLETE` with a `session_event_t` (`log_svc.h`, **32 bytes**): a `charge_record_t` — `charge_session_t` plus `start_epoch` — and the start uptime. `log_svc` stores the last 20 records as an NVS blob (`session_log_t` = 4 + 20×28 = **564 bytes**).

**Start time (2026-10-17).** `start_epoch` is the Unix time CHARGING was entered. It is wrapped
around `charge_session_t` in `log_svc.h` rather than added to `tes_types.h`, because that header is
the TES-Protocol submodule shared with two other repos that never use the struct. Not synced at
the start but synced by the end → back-computed from the uptime. Still not synced at the end →
saved as 0 and kept on a RAM pending list; `task_log` wakes every 30 s and dates them once the
clock is set (same boot only, same `esp_timer` base). After a reboot they stay undated.

**Monthly totals (`energy_month.c`, pure, host-tested).** Only 20 records are kept, too few for
a month view, so `log_svc` also adds every session to a per-month book: up to 36 months, oldest
dropped, months in UTC+8 by integer date math (no TZ dependency). Each month holds `wh`,
`est_wh` (sessions with `energy_estimated` — no PSU, current = Max Current setting), and the two
session counts; `undated` counts sessions that never got a start time. `GET /energy` returns it
with the cost settings. **This is energy into the battery (DC), not the electricity bill**: the
web UI and app divide by the PSU efficiency and multiply by the price only when a price is set,
and draw the estimated part lighter. Records migrated from the old format have no date and are
not in the book (and not counted as undated).

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

**⚠️ `session_event_t` fills the payload.** `charger_event_t.payload` went 24 → 32 bytes on
2026-10-17 for it (8 subscribers × 8-deep queues = +512 bytes of internal RAM); `_Static_assert`s
in `task_tes_sm.c` and `log_svc.c` block further growth. Widen the payload first.

**Changing the record size needs a migration.** `log_svc_init()` checks the stored blob length;
the 484-byte `charge_session_t` format is converted (`session_log_v1_t`, start time 0) so an OTA
keeps the history. Anything else unrecognised starts fresh. Next time the record changes, add
another branch there rather than discarding.

`GET /history` returns newest-first JSON array (max 20 entries), each with a
`session_id` linking to the trace buffers below, plus `fault_source` / `fault_ctx_a` and
`start_epoch` (0 = unknown; shown as the 日期 column).
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
   設定只有 60.0 V → 請換用 72.0 V 以上的電源". (Until 2026-10-17 it said "raise Max Voltage to 72.0 V";
   that was wrong advice — Max Voltage is VLIM2, the vehicle's over-voltage threshold, and must match what
   the PSU really outputs. Not enough voltage means not enough PSU.)

`enter_fault(sm, out, tick_ms, src, ctx_a, ctx_b)` — every call site must state its context.

Web UI (`FAULTS` table in index.html): title / why / action per source, with `ctx` rendered
into the text. `FAULT_SRC_EMERGENCY_VEHICLE` is styled **informational (amber), not red** —
the iE125 sends 0x5F0 after every normal charge end, so a red alarm there would be wrong.
The panel shows whenever `fault_source > 0`; the old condition also required `fault_flags`,
which hid every fault that doesn't set one (PSU lost, CP lost, voltage limit).

The OLED shows a two-line description plus the context values, with the raw code demoted to
a footnote — in Chinese or English (`oled_lang`, see **OLED: Chinese, animations, screensaver**).

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
gives session-relative time. `epoch_s` is recorded as 0 until NTP syncs. Since 2026-10-17
`/trace` (`start_epoch`) and `/tracelog` back-fill those zeros **at read time** once the clock
is set: `now − (uptime_now − t_ms)` (`trace_epoch_backfill()` in `network_svc.c`). That is
exact because traces live in RAM and so always belong to the current boot, on the same
`esp_timer` base. The web UI's `開機 +…` / `+MM:SS.mmm` fallback now only shows while the
unit has still not synced.

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
NVS keys: `mqtt_url` (empty = disabled), `mqtt_topic`. Publishes `{prefix}/status` every 10 s (CHARGING) or 30 s (other), immediately on state change. LWT: `{"state":"offline"}` retained. Subscribes `{prefix}/cmd` QoS 1 for `{"cmd":"start"/"stop"}` → `g_btn_event_queue`. The status JSON
carries `stop_mode` and `stop_energy` (kWh, 2026-10-17, so the app and Cloud PWA can show delivered / target
remotely; buffers went 384 → 448 bytes) and `remote_cmd` (= `mqtt_cmd_enabled`, 2026-10-14) so the app can tell from outside the LAN
whether commands will be heeded; the subscription itself only changes on reconnect, so after toggling
the setting the two can disagree until a reboot. Uses ESP-IDF `esp-mqtt` component. `GET /mqtt/link` returns Cloud PWA URL with broker/topic in fragment.

**Cloud PWA** (`docs/monitor.html`): MQTT.js via WebSocket, config from URL fragment (`#b=host&p=wsport&t=prefix`); stored in `localStorage` after first use. GitHub Pages (HTTPS) → Service Worker works → offline caching valid.

**It is the iPhone users' remote control** until the mobile app ships on iOS, so since 2026-10-14 it
matches the app: start asks for confirmation; commands are published with `retain:false` explicitly (a
retained start would fire on every reconnect of the charger); the buttons follow the state (start only
when idle, stop only while a session runs, both off when the link or the charger is offline); and
`remote_cmd` in the status disables them with an explanation when it is false — when absent (firmware
before v3.6.2) a note says commands may be ignored. If the state has not moved 20 s after a command
the page says the charger did not react. iOS gets `apple-touch-icon` (`docs/monitor-icon.png`, copied
to Pages by the workflow) and, in Safari, a tip to add it to the home screen **before** entering the
credentials — a home-screen web app has its own storage, separate from Safari's.

**WebSocket must be wss, and cloud brokers need credentials (fixed 2026-10-14).** The PWA lives on
GitHub Pages (HTTPS), where browsers block plaintext `ws://` as mixed content — yet it connected with
`ws://` and `broker_ws_port()` returned plaintext ports (8000 / 8080 / 8083), so it could not connect
to anything. It now always uses `wss://`; `broker_ws_port()` returns 8884 for HiveMQ, 8081 for
test.mosquitto.org and 8084 otherwise (EMQX Cloud `*.emqxsl.com`, broker.emqx.io). The page has
username/password fields, kept in that browser's `localStorage` and **never** put in the shared
`#b=…&p=…&t=…` link; for `*.emqxsl.com` / `*.hivemq.cloud` it stays on the settings panel until they
are filled. Context: HiveMQ Cloud stopped accepting new free Serverless clusters (retired
2026-12-31); the mobile app's remote wizard now recommends EMQX Cloud Serverless (TLS 8883, wss 8084).

---

## Mobile App

**The app itself is developed in a separate, non-public repo** (Expo / React Native, Android and iOS).
**It has every feature of the web UI** (`index.html`, `hw.html`, `devices.html`) — the author's rule — so
**a feature added to the web UI should be added to the app too**, and this firmware has to keep the
interfaces the app relies on stable:

- **LAN:** every endpoint in the REST table below is used, including `/trace`, `/tracelog`, `/hw.json`,
  `/hw/board.svg`, `/hw/test`, `/wifi/scan`, `/ota/upload` and the PSU pairing pair. `GET /devices` matters
  in particular: the app has no mDNS of its own and uses it to find other units. All POSTs carry
  `X-TES-Request`. `POST /config` range checks must stay in step with the app's copy. The `/status`
  fault fields (`fault_source`, `fault_ctx_a/b`) are explained by a copy of the `FAULTS` table — add a
  fault source here, add its text there.
- **Remote:** MQTT `{prefix}/status` and the LWT `{"state":"offline"}` — see MQTT Remote Monitoring.
  Since app 0.4.0 (2026-10-14) the app also sends `{prefix}/cmd`, but only when its broker needs a
  username/password and the status says `remote_cmd: true` — the broker's credentials are the only
  authentication the command has.
- **Push:** `POST /push` and the Expo send in `push_svc` — see Webhook / ntfy Push Notification.

Renaming or removing a field in any of these breaks installed apps; add fields instead.

---

## Scheduler + Auto-Start Integration (Planned)

**Problem:** when both `sched_enabled` and `auto_start` are on, CP/CAN edges outside the charging window could trigger unintended charging.

**Planned solution:** add `bool in_charging_window` to `tes_sm_inputs_t`. Gate auto-start CP/CAN edge detection on this field. `scheduler_svc_is_in_window()` returns `true` when `sched_enabled=false`. Scheduler's `EVT_BUTTON_START` path (manual START route) clears `charge_complete_latched` directly — no re-plug needed.

**Implement after auto_start hardware testing.**

---

## Known Technical Debt

### 🟡 Half solved (2026-10-18): internal RAM starvation fixed; sending is still slow

Measured with `/hw.json` (the procedure below) on three units:

| | stock `sdkconfig` | + WiFi/lwIP in PSRAM | + mbedTLS in PSRAM too |
|---|---|---|---|
| internal RAM free, idle | ~97 KB | ~81 KB | **~113 KB** |
| lowest since boot | 17.5 KB; **276 bytes** on a unit doing a GitHub OTA with MQTT TLS up | ~40 KB | **~70 KB** |
| 1.56 MB `POST /ota/upload`, unthrottled | stalled at 58 % / 88 % (~4.5 KB/s) | — | **11.5 s, 136 KB/s** |
| `/control` (121 KB) ×10 | 8–49 KB/s | 5–72 KB/s | 3–311 KB/s, some timeouts |
| TIME_WAIT / active PCBs | 0 / ≤ 6 of 16 | | |

- **Receiving was internal RAM.** At 276 bytes free the HTTP server stopped accepting connections (ping
  still answered), OTA from GitHub stalled at 14 %, and a restart request did not complete. The fix is
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` + `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` in `sdkconfig.defaults`
  (app config only — reaches deployed units by OTA). MQTT over TLS checked working with it.
- **Hypothesis 2 is ruled out**: TIME_WAIT stayed 0 and active PCBs ≤ 6 throughout.
- **Sending (`/control`) is still slow and RAM is not why** — with 70 KB to spare it still stalls in roughly
  1 s steps, which looks like retransmission timeouts. Not diagnosed yet. Two things are known to make it
  worse: `LWIP_TCP_SND_BUF` / `TCP_WND` were 5760 (4 segments) — **raised to 14400 (10 segments, receive mailbox 12)
on 2026-10-20**, affordable because lwIP buffers are in PSRAM; not measured yet: compare an upload OTA and
`/control` ×10 against a unit still on 5760, with `netlog` and the TCP card — and httpd has one worker, so `/hw.json` and the
  app's `/status` polls queue behind a slow page and time out (5 s) — the snapshot never shows the stall
  itself. Next: look at it from the PC side (packet capture) or give `/hw.json` its own path.

### 🟡 Upload OTA stalling at 94–96 % (2026-10-18/20, cause not found)

Seen twice on one unit (`0bdbe6f`, RAM fix already in): progress stopped at 94–96 %, curl got no
answer for 300 s, the unit kept answering `/status`, did not reboot and ended with `ota_running` false on
the old firmware — the TCP flow trickled, nothing crashed. Two things made it worse and are fixed
(2026-10-20): one 30 s `httpd_req_recv` timeout aborted the whole upload (now up to
`OTA_RECV_TIMEOUTS` = 3, as in ESP-IDF's own example), and `ota_svc_upload_write()` published an
`EVT_OTA_PROGRESS` per 4 KB (~380 per image, each waking every subscriber; now on percent change).
**Next time it stalls, read `/hw.json` right away**: `netlog` has `ota` / `ota_stall` with timestamps,
and the TCP card shows the upload connection's window, retransmits and RTO.

### 🔴 Unresolved (original notes): `/control` throughput collapses after 1–2 loads

Investigated 2026-09-11, **not fixed, cause not identified**. Reproducible and
characterised, but diagnosis stalled for lack of device-side visibility.

Measured on a verified-clean link (0 % loss, 17 ms RTT to the device; 0 % / 1 ms to the
router in the same run):

| Request | Result |
|---|---|
| `/control` (94 KB) 1st–2nd load | **0.32–0.65 s (144–290 KB/s)** — normal |
| every load after that | **11–30 s (8 KB/s or worse)**, sometimes hits a 30 s timeout |
| after ~60 s idle | recovers, then collapses again |
| `Connection: close` | no difference |
| `/status` (1.5 KB) *during* the collapse | still 25–236 ms |

That last row is the key constraint on any explanation: **the server still accepts and
answers promptly — only bulk throughput dies.** Socket/connection exhaustion is therefore
ruled out; it would delay everything.

Two hypotheses, not yet distinguished:
1. **Internal DRAM starvation throttling WiFi dynamic TX buffers.**
   `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32` and
   `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is **not** set, so those buffers can only come
   from internal RAM. A small response needs 1–2 buffers (fine); a bulk transfer needs
   many (starved). Fits the symptom exactly.
2. **lwIP TCP PCB resources.** `CONFIG_LWIP_TCP_MSL=60000` matches the ~60 s recovery
   constant suspiciously well (`CONFIG_LWIP_MAX_ACTIVE_TCP=16`).

Neither is a leak — it recovers on its own.

**Next step is data, not code:** `task_monitor` already prints `heap free / min / psram`
plus every task's stack watermark every 10 s. Reading that over USB *while provoking the
collapse* should settle it immediately. Do not start tuning without it.

**The `/hw` page now carries that data without USB (2026-09-30).** Its 系統 card shows
internal RAM free / minimum / largest block, and the TCP card lists every lwIP PCB —
state, peer, send-queue length, writable buffer, cwnd, retransmit count, RTO and idle
time — plus TIME_WAIT count against `MEMP_NUM_TCP_PCB` and httpd's open sockets against
`max_open_sockets`. `/hw.json` is small, so it should keep updating through a collapse.
Procedure: open `/hw` in one tab, reload `/control` in another until it collapses, and
watch. Hypothesis 1 shows as the bulk connection's retransmits and RTO climbing while
internal RAM dips; hypothesis 2 as TIME_WAIT filling the pool. The PCB lists are read in
the tcpip thread via `tcpip_api_call()` — `LWIP_TCPIP_CORE_LOCKING` is off, so walking
them from httpd directly would race lwIP.

> ⚠️ **Do not "fix" this with gzip.** Compressing the 94 KB page to ~20 KB would raise the
> number of loads before collapse from ~2 to ~8 and look like a fix while the underlying
> resource problem remains. (This was nearly done — the first measurements were taken
> during an unrelated transient RF fault, 13 % packet loss, which produced a plausible but
> wrong "the page is too big" conclusion. Re-measure link quality before trusting any
> throughput number here.)

### 🟡 To evaluate: the ESP32 module is rated to 65 °C; PSRAM ECC raises it to 85 °C

Recorded 2026-09-28, **nothing changed yet.** U1 is `ESP32-S3-WROOM-1-N16R8`
(the V1.3 BOM, in the private hardware repo). The **R8 variants (octal PSRAM) are rated
−40 to 65 °C ambient**, not the 85 °C of the plain modules. The
[datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
states that enabling PSRAM ECC raises that to **85 °C**, at the cost of 1/16 of the
PSRAM, i.e. 8 MB → 7.5 MB.

Why it matters: the module, not the enclosure, is the part with the lowest
temperature rating in the unit. A controller left in a parked vehicle, or mounted on a
warm PSU, can pass 65 °C inside its enclosure in a Taiwanese summer. This was found
while choosing an enclosure material (PETG, ~70 °C) — the enclosure outlasts the module
either way.

The change is one line, `CONFIG_SPIRAM_ECC_ENABLE=y` in `sdkconfig.defaults` (it
depends on `SPIRAM_MODE_OCT`, which is set). Things to check **before** shipping it:

- **PSRAM is not only data here.** `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and
  `CONFIG_SPIRAM_RODATA` are on, so code and rodata are copied into PSRAM at boot,
  besides the ~656 KB of `trace_svc` buffers. Measured headroom was ~6.4 MB free, so
  losing 512 KB is affordable — but confirm ECC works together with execute-from-PSRAM
  on this IDF version, and read `task_monitor`'s `psram` figure afterwards.
- **Throughput.** ECC adds overhead to every PSRAM access; code runs from PSRAM, so
  measure the SM tick and the `/control` page load before and after.
- **OTA path.** PSRAM is initialised by the app, so this should reach deployed units
  by OTA with no partition-table or bootloader change — verify on a board that was
  flashed with the current firmware, then updated by OTA, not flashed fresh.
- The firmware requires 16 MB flash + octal PSRAM (`sdkconfig.defaults`), so every
  unit running it carries an R8 module — the win would reach units already in the
  field, not only V1.3.

### Other

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

## Security

This device closes a relay onto a vehicle's HV battery, so "someone can control it"
is a physical-consequence problem, not just a privacy one. What is implemented, what
is deliberately not, and why.

### Implemented

**CSRF protection.** The ten state-changing endpoints (`POST /config`, `/start`,
`/stop`, `/ota`, `/ota/upload`, `/notify/test`, `/psu/pair`, `/psu/pair/confirm`,
`/hw/test`, `/reboot`) require an
`X-TES-Request` header; `csrf_ok()` in `network_svc.c` rejects the rest with 403.

Why a header works: a custom header forces the browser to send a CORS preflight, and
this server registers no `OPTIONS` handler, so cross-site requests die there. Same-origin
requests (the device's own page) never go through CORS at all. Read-only endpoints are
untouched, so the multi-device dashboard's cross-origin `GET /status` still works.

**This is not authentication.** Anything that speaks HTTP directly — curl, a script,
any program on the LAN — can set the header itself. It stops a webpage from acting on
the user's behalf; it stops nothing else. Scripts must send the header (see Release & OTA).

The web UI adds it by wrapping `window.fetch` rather than at each call site — a new
endpoint would otherwise silently 403 and the cause is not obvious from the symptom.

**MQTT.** `mqtts://` works now (`esp_crt_bundle_attach`; without a CA source TLS could
only fail, so users were effectively forced onto plaintext). Credentials go in the URI:
`mqtts://user:pass@host:8883`. `mqtt_cmd_enabled` (NVS `mqtt_cmd`) turns off the `cmd`
subscription — status keeps publishing, but no path exists to command the charger.

> ⚠️ `mqtt_cmd_enabled` **defaults to true** — compatibility over safe-by-default, so
> that OTA doesn't silently remove remote start/stop from people already using it. The
> consequence is that the existing exposure is not fixed automatically; users must turn
> it off or move to an authenticated broker. Worth revisiting.

### Known gaps — not fixed

| Gap | Exposure | Consequence |
|---|---|---|
| **No authentication anywhere** | Anyone on the LAN / in AP range | `/ota/upload` takes arbitrary firmware — full takeover of a device wired to HV |
| **AP mode is open** (`WIFI_AUTH_OPEN`) | Anyone in radio range | Grants the precondition for the row above |
| **Plain HTTP** | Same-segment sniffing | Config and status readable and modifiable in transit |
| **No flash encryption / secure boot / NVS encryption** | Physical access | WiFi password recoverable from flash |
| **Public MQTT brokers still permitted** | Internet-wide | On a no-account broker, `tes/+/status` enumerates every online unit — the topic never has to be guessed |

`POST /config` deserves attention beyond "someone starts a charge": `max_voltage` feeds
VLIM2 in 0x508, the vehicle's fault-detection voltage ceiling. Setting it wrong disables
the vehicle's own overvoltage protection.

### Why not HTTPS

Asked and evaluated; the answer is no, and the reason is certificates, not effort.

- **Self-signed baked into firmware** — the private key ships inside a public binary, so
  it stops no real attacker, and every visit shows a full-page browser warning.
- **Per-device self-signed at first boot** — key is no longer shared, still untrusted,
  still warns.
- **A real CA (Let's Encrypt)** — needs a public DNS name and reachability or DNS-API
  credentials, plus 90-day renewal. Not available to a LAN device.

The certificate's SAN would also have to cover `tes-<id>.local`, `tes-charger.local`,
`192.168.4.1` **and a DHCP address that changes** — the last one cannot be covered, so
a name-mismatch warning stacks on top of the untrusted-CA one. For non-technical users
that is worse than HTTP: it trains them to click through security warnings.

Three things would also break: the device-list dashboard (cross-origin fetches to other
units, each with an untrusted cert), Service Worker registration (a click-through cert is
not a secure context, so PWA caching still would not work), and every `curl` call would
need `-k`.

**Most importantly, TLS does not address the actual gap.** The problem is that requests
are unauthenticated; encrypting them leaves them just as unauthenticated. **Authentication
first, transport encryption later — if ever.** Where TLS genuinely matters here is traffic
that leaves the LAN, i.e. MQTT, which is why `mqtts://` was added instead.

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

**WiFi modes** (rewritten 2026-10-17):
- No network saved → hotspot `TES-Charger-<id>` (**open**), IP `192.168.4.1` — first setup, as before
- `sta_enabled == false` (NVS `sta_en`) → the same open hotspot, saved networks kept intact
- Otherwise → STA. It **scans and joins the strongest of up to 5 saved networks**; if none is
  reachable 30 s after boot (60 s after losing one) it **also opens the hotspot, WPA2** with a per-unit
  password, and keeps scanning in the background (every 10 s before the hotspot, 60 s after)

Why: a unit set up at home and taken elsewhere used to retry the home network forever and never open
the hotspot, so no phone could reach it — and the switch that forces the hotspot is on the web page it
could not reach. Taking it out is the case to design for: add the local WiFi once through the hotspot
and next time it joins by itself, so pushes and MQTT work while the owner is away (a phone hotspot does
not: the phone leaves with the owner).

| Piece | Where |
|---|---|
| Saved networks | `wifi_list.c` (pure): `wifi_list_t` = 5 × {ssid, pass}, newest first, NVS blob `wifi_nets` (485 B). The first entry is also written to the old `wifi_ssid`/`wifi_pass` keys (and `charger_config_t.wifi_ssid`), so a downgrade still connects; units without the blob migrate those keys on boot |
| When to scan / open the hotspot | `wifi_plan.c` (pure, host-tested): delays above; with the station up, the hotspot closes after 30 s with no client on it (never kicks a phone off); `hold` = ESP-NOW PSU and a session running → no scanning at all, because a scan hops channels and the PSU link faults after 2 s |
| ESP-IDF glue | `network_svc.c` "WiFi：記住的網路、自動熱點": `network_svc_wifi_tick()` from `task_network` every 100 ms. Scans are async; `SCAN_DONE` only sets a flag (the event task has 2.3 KB of stack) and the tick reads the results, picks the strongest saved SSID not yet failed this round, connects with the scanned channel. A failed network is skipped until the next round; connect attempts time out after 20 s. `s_scan_lock` makes the background scan and `GET /wifi/scan` take turns |
| Hotspot | not opened while a scan or a connect attempt is running (2026-10-20): opening it switches the mode and pins the radio to the current channel, which broke that attempt and cost another round; channel = the current one (no hop for an ESP-NOW PSU); its security is chosen when it starts and **not changed while it runs**, so a phone that just added a network over the open setup hotspot is not dropped |
| Password | NVS `ap_pass`, 10 characters generated **once**, on the first boot without the key (it survives reboots and OTA; only a full NVS erase makes a new one), from an alphabet without 0/o/1/l/i; the user can set their own in the web UI / app (修改熱點密碼) — a running hotspot keeps the old one until it next opens; OLED menu row `AP pass` / 熱點密碼 under WiFi Info; `GET /config` `ap_pass` (anyone on the LAN can already control the unit); `POST /config` `ap_pass` changes it (8–63 visible ASCII) |

`POST /config`: `{"wifi_add":{"ssid","pass"}}` (pass omitted = keep a saved one's password / open for a
new one), `{"wifi_remove":"ssid"}` (removing the connected one disconnects). Both apply live — no reboot.
The old `wifi_ssid` + `wifi_pass` fields still work and mean "add this one"; an empty `wifi_ssid` is
ignored. A WiFi error comes back as `{"ok":false,"error":...}` (other fields in the request are saved).
SSIDs and passwords are UTF-8 and limited in **bytes**, as WiFi itself is: SSID ≤ 32 (about 10 Chinese
characters), password 8–63; the web UI and app count bytes too. Old routers that broadcast a Chinese
SSID in Big5/GBK will not match a name typed in UTF-8 — pick it from the scan list instead. SSIDs are
chosen by anyone nearby, so the scan list renders them as text, never through `innerHTML` (it did
until 2026-10-17).
`GET /config` has `wifi_nets` (SSIDs only, never passwords), `wifi_max`, `ap_pass`; `/status` has
`sta_ssid`, `ap_on`, `ap_clients`. `GET /wifi/scan` answers 409 while `hold`.

**Event log without USB (2026-10-20):** `/hw.json` `netlog` is the last 48 WiFi / ESP-NOW events,
oldest first, `[t_ms, name, a, b]` (`drivers/netlog.h`): `wifi_try` (channel, attempt), `wifi_up`
(channel), `wifi_down` (reason, 1 = a connect attempt failed), `wifi_timeout`, `scan` / `scan_done`
(count), `ap_on` (channel, secure) / `ap_off`, `radio_busy` (1/0), `psu_up`, `psu_down` (1 timeout /
2 no MAC ACK, fail streak), `sess_ready` (handshake count), `chn` (channel told to the node, on change), and for `POST /ota/upload`: `ota` every 10 % (percent, KB),
`ota_stall` when one receive waited over 2 s (ms, KB), `ota_end` (0 ok / 1 gave up / 2 write failed, KB).
Added because WiFi and the ESP-NOW link affect each other and `/status` only keeps the last reason code —
the order of events is what tells them apart. Read it right after a drop.

**Not tested on hardware yet:** joining the strongest of two saved networks; wrong password on one →
the next one; hotspot appears 30 s after boot away from home and the phone reaches 192.168.4.1 with
the OLED password; hotspot closes 30 s after the phone leaves once home WiFi is back; an ESP-NOW PSU
keeps its link through all of it (and no scan while charging); Android reaching 192.168.4.1 on a WiFi
without internet.

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

**Dead connections (fixed 2026-10-14).** httpd has 7 sockets (`max_open_sockets`) and no idle
timeout. A phone that leaves home switches from WiFi to mobile data without closing its keep-alive
connections, so each one held a slot forever; once all 7 were taken, every new connection was
accepted and closed at once (`curl: Empty reply from server`) — web UI and app dead, MQTT fine, only
a reboot helped. Seen on a unit at home that morning. Two fixes: `cfg.lru_purge_enable = true`
(a full server closes the least recently used socket instead of refusing the new one) and TCP
keepalive in `http_sock_open()` (idle 30 s, 3 probes 10 s apart). This is a different problem from
the `/control` throughput collapse below, where `/status` still answered.

HTML is served `Cache-Control: no-cache` (`UI_CACHE_CONTROL`). It was `max-age=86400`,
which left users on a stale UI for a day after each OTA with no way to notice. The page is
tens of KB over LAN; re-fetching costs far less than showing an outdated interface. This
does not affect offline support — the Service Worker's Cache API is independent of the
HTTP cache.

**REST API (port 80, CORS `*` on reads).** Every `POST` below requires the
`X-TES-Request` header — see **Security** above. Reads need nothing.


### Web UI language — `web/i18n.js` (2026-10-17)

The pages are still written in Chinese; `i18n.js` holds a Chinese → English dictionary and, in
English mode, translates text nodes, `placeholder`/`title`/`aria-label`, `document.title` and
`alert`/`confirm` text — including everything rendered later, through a `MutationObserver`.
Lookup order: whole fragment → `{}` patterns (numbers and other variable parts) → known phrases
replaced inside longer composed strings → leftover full-width punctuation made half-width. A
string with no entry just stays Chinese in English mode; nothing breaks. **When adding Chinese UI
text, add its English to `EN`.** Language: `localStorage.tes_lang`, else the browser language;
the 中/EN button sits in each page's `#lang-sw`/`.lang-sw` slot (fixed top-right if a page has
none) and reloads the page (translation is one-way). Dates use `I18N.locale`.

Loaded by `/control`, `/`, `/hw`, and by `docs/monitor.html` (the Pages workflow copies
`i18n.js` next to it). ~52 KB; served with `Cache-Control: no-cache` + ETag of the firmware
image hash, so after the first load it costs a 304 until the next OTA.

Gotcha fixed while writing it: never re-set an attribute or text node to the same value inside
the observer — that fires the observer again and hangs the page.

To find what is still untranslated: open the page in English (`localStorage.tes_lang='en'`) and
walk `document.body` text nodes for CJK characters; the device name and the 中文 label are the
only expected hits.

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

### Hardware status page (`/hw`) and bench test mode — written 2026-09-30, not yet run on a board

`/control` links to it under the firmware version. The page is a top view of the board
drawn from the KiCad file, with each part at its real position; state is shown by
colouring the parts themselves (outputs glow, pressed buttons turn blue, faults get a red
outline) plus a few value tags (CP, output voltage at the 120 V sense pads, board ID on
U5, chip temperature on U1, PSU link on H2, CAN on U12). Cards beside it carry the full
numbers, including every task's stack high-water mark — the `task_monitor` line, without
needing USB.

**The production board's drawing is not in this firmware (2026-10-05).** The page asks
the device for `GET /hw/board.svg`, which answers per board:

| Board | Drawing |
|---|---|
| valid factory record (`factory_svc`: our signature, this chip's MAC, drawing hash) | the full production drawing, read gzip'd from the `tes_factory` partition |
| V1.1 / V1.2 (AIN3 level 0) — every unit in the field, treated as genuine | `web/hw_board_base.svg`: the board minus the V1.3-only parts (D9, D10, R33–R37) and the V1.3 silk text. Their placement was already public in the EasyEDA projects, so this discloses nothing new |
| anything else (V1.3+ without a valid record, unknown revision) | 404 — the page shows the numbers only |

The factory record is written at production by `tools/provision_factory.py` in the
private hardware repo: version, serial, MAC, AIN3 level and the gzip'd drawing, signed
with ECDSA P-256. The **public key** is compiled into `factory_svc.c`; the private key
lives only on the author's PC (`%USERPROFILE%/.tes/`). It writes flash only — no eFuse —
so a mistake is fixed by writing again. A copied record fails the MAC check, a forged one
the signature. A modified firmware can of course skip the check; this decides only what
the official firmware shows, it is not a security boundary. `factory_svc` verifies
lazily on the first `/hw` request, in the httpd worker (8 KB stack) — ECDSA does not fit
`app_main`'s 3.5 KB. `/hw.json` reports the record's state and serial under `factory`.

Both drawings are generated from the KiCad board by `tools/make_hw_board_svg.py` in the
private hardware repo; it writes `hw_board_base.svg` here and the full drawing there.
Each footprint is a `<g id="fp-<ref>">` and the page's JS only depends on those ids.
Back-side footprints are drawn too, with only their
through-hole pads and a dashed outline — H2, the PSU UART header, is mounted on the back
and was missing until that was added.

**The OLED is not in the KiCad file** — it is a 0.96″ SSD1306 module on the H1 header,
stacked over the ESP32-S3 module. Its glass is drawn from the enclosure lid's display
window (the lid STEP in the private hardware repo, a 26.50 × 19.59 mm cut-out). Lid
coordinates map onto the board as **`x_pcb = x_lid + 152.07`, `y_pcb = 104.66 − y_lid`**:
fitted on the four mounting holes, then checked against every button, LED and the
BOOT/EN pin-holes in the lid — all within 0.1 mm. Worth reusing for enclosure work.

`/hw.json` is separate from `/status` on purpose: small, and independent of `/control`'s
unresolved throughput collapse. It reports what was **actually written to the pins**
(`hal_gpio_outputs_get()`), not what the SM wants — the two differ while LEDs blink and
during a bench test. Buttons come as held (`g_btn_stable`) plus pressed-since-last-read
(`g_btn_latch`, cleared by `atomic_exchange`), because a short press falls between two
500 ms polls. Chip temperature is the ESP32-S3 die sensor (20–100 °C range), typically
10–20 °C above ambient — see the 65 °C module rating item under Known Technical Debt.

**Bench test mode** lets the page switch the DC relay, coupler lock, VP and the three
LEDs by hand, to check a board. The rules, and where they live:

| | |
|---|---|
| Allowed only when | SM in IDLE, no emergency latch, CP < 1.9 V, no 0x500 for 3 s — i.e. no vehicle |
| Checked | every 10 ms tick in `task_tes_sm.c` `bench_test_override()`, between `tes_sm_tick()` and `execute_outputs()` |
| Ends | the same tick any condition fails (START, vehicle plugged in, emergency), or after a 10 s lease without keepalive (page sends one every 2 s; closing it sends `stop`) |
| After it ends | outputs are the SM's own again; it does **not** re-enter by itself — otherwise vehicle → end → VP off → CP drops → re-enter would oscillate |
| Starts from | everything off; one owner at a time (a random id per page — a second page takes over, the first one's keepalives are refused) |
| Not touched | PSU setpoints and CAN transmission — only GPIO outputs are overridden |

`hwtest_svc` holds the request and lease only; the conditions sit next to the SM because
they read the SM's own inputs. The SM itself is untouched and keeps ticking — the
override is applied to its outputs struct, so the "data in, data out" rule still holds.
Closing the DC relay puts PSU voltage on the gun's DC pins; the page asks for
confirmation and points out that the 120 V sense reading then shows the PSU voltage,
which is the way to check a board's divider coefficient against a meter.

This is **not** authentication: like every other POST it only has the CSRF header, so
anyone on the LAN could drive the outputs while no vehicle is connected. That is the
same exposure as `/start` and `/ota/upload` (see Security → Known gaps), not a new one.

**The OLED says so.** While a bench test runs, `display_svc` replaces the status screen
with an inverted `BENCH TEST` screen showing the pins as actually driven (RELAY / LOCK /
VP, LED Y/G/R) and how to exit; after it ends, `TEST ENDED` plus the reason stays for 3 s
(`hwtest_status_t.ended_ago_ms`). Someone standing at the unit would otherwise hear a
relay click in standby with nothing to explain it. The settings menu still opens as usual.

**To verify on hardware:** every button lights up on the drawing; each output toggles
and clicks; plugging in a vehicle (or a CP test plug) ends the test within one tick; the
lease ends it ~10 s after pulling WiFi; `task_tes_sm` and `network` stack headroom after
a few minutes on the page.

### Device list page

`/` serves **`web/devices.html`** — a list of every TES controller on the LAN; the control
UI moved to **`/control`**. `GET /devices` does the discovery **on the device** via
`mdns_query_ptr("_http","_tcp", 2000ms, 20)`, because browsers have no mDNS-browse API and
subnet-scanning from JS is slow and often blocked.

**Only one controller found → straight to `/control`** (2026-10-06): on the page's first,
automatic scan, if the list holds just this unit, `location.replace('/control')` — a list of
one has nothing to choose. A manual rescan, or `/?list` (what the control page's ‹ link now
points at), stays on the list; otherwise ‹ would bounce straight back.

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
| GET | `/status` | JSON snapshot: state, voltage, current, soc, target_soc, stop_mode, stop_voltage, timer, fault, fault_source, stop_reason, wifi, ip, ntp_synced, local_time, power_w, energy_wh, device_id, device_name, display_name, hostname, ap_ssid, firmware_version, hw_rev, hw_known, hw_id_level, hw_id_v |
| GET | `/config` | JSON config: all `charger_config_t` fields |
| POST | `/config` | Partial update (any subset); `wifi_add` / `wifi_remove` / `ap_pass` apply live, `sta_enabled` needs a reboot (see WiFi modes). Also `psu_eff` (50–100) and `kwh_price` (0–20) for the monthly chart, stored by `log_svc`, and the OLED settings `oled_lang` / `oled_anim` / `ss_min` / `ss_style` |
| POST | `/start` | Sends `EVT_BUTTON_START` to `g_btn_event_queue` |
| POST | `/stop` | Sends `EVT_BUTTON_STOP` to `g_btn_event_queue` |
| POST | `/ota` | Pull firmware from URL (default: GitHub Releases latest) |
| POST | `/ota/upload` | Upload binary (`application/octet-stream`); progress via `/status` |
| GET | `/history` | Last 20 charge sessions (newest-first), incl. `start_epoch`, `session_id`, `fault_source`, `fault_ctx_a` |
| GET | `/energy` | Monthly energy `{"months":[{"ym","wh","est_wh","sessions","est_sessions"}],"undated","psu_eff","kwh_price"}`, oldest first, ≤ 36 months |
| GET | `/trace` | Charge-curve samples for a session + session list (chunked) |
| GET | `/tracelog` | Timestamped value-change log for a session (chunked) |
| GET | `/i18n.js` | Chinese → English dictionary + language switch for the web pages (ETag = firmware hash, 304 until an OTA) |
| GET | `/manifest.json` | PWA manifest |
| GET | `/sw.js` | Service Worker |
| GET | `/icon.svg` | App icon |
| GET | `/wifi/scan` | Scan nearby APs (max 20: ssid, rssi, secured); 409 while charging with an ESP-NOW PSU |
| POST | `/notify/test` | Send a test notification to `notify_url` and every registered phone |
| GET | `/push` | Registered phones: `{"max":4,"phones":[{"slot","name","added","tail","lang"}]}` — never the full token |
| POST | `/push` | Mobile app: `{"op":"add","token","name","lang"}` (`lang` `zh`/`en`, optional) / `{"op":"remove","token"}` / `{"op":"remove_slot","slot"}` → `{"ok":true,"count":n}`; 409 when 4 phones are already registered |
| POST | `/reboot` | Restart after 500 ms. **409** while charging (PARAM_EXCHANGE … ENDING) — `restart_svc`, same reason as the OTA check; an unreadable state counts as busy. The web UI button confirms first, then waits for the unit to come back and reloads |
| POST | `/psu/pair` | Start ESP-NOW pairing (requires psu_transport=1, charger idle); progress and code in `/status` → `psu_pair` |
| POST | `/psu/pair/confirm` | `{"accept":true}` = codes match, `false` = cancel (same as START / STOP on the unit) |
| GET | `/mqtt/link` | Cloud PWA URL with broker/topic fragment |
| GET | `/hw` | Hardware status page (board drawing + live pin/ADC/PSU/CAN/system state) |
| GET | `/hw.json` | Data for `/hw`, polled every 500 ms |
| POST | `/hw/test` | Bench test mode: `{"cmd":"start"/"set"/"keepalive"/"stop","owner":n,"mask":n}` |

**CMake notes for embedded web UI:**
- HTML embedded via `EMBED_TXTFILES "web/index.html"`; symbol `_binary_index_html_start` / `_binary_index_html_end`
- mDNS: managed component `espressif/mdns` in `idf_component.yml`; CMakeLists REQUIRES entry `espressif__mdns` (double underscore)
- `max_uri_handlers = 32`; currently 30 handlers registered
- `web/devices.html` is a second `EMBED_TXTFILES` entry → `_binary_devices_html_start/_end`
- `sw.js` cache bumped to `tes-v3`; app shell is now `/` **and** `/control`
- `drivers` component REQUIRES `esp_wifi` (for ESP-NOW in `psu_driver.c`)

**PWA note:** Service Worker requires HTTPS. On `http://tes-charger.local` (plain HTTP), SW registration is silently blocked — offline caching does not work. "Add to Home Screen" shortcut works over HTTP.

---

## Release & OTA

**Release:** `git tag v3.x.x && git push origin v3.x.x` → GitHub Actions builds with ESP-IDF v5.5.5（與開發機同版，見 Build 一節）, creates Release with `tes_charger.bin` (OTA) and `tes_charger_flash.bin` (initial flash), deploys GitHub Pages. `docs/manifest.json` uses relative path `./tes_charger_flash.bin`; Pages source must be **GitHub Actions**.

**OTA:**
- First flash: GitHub Pages tool at `https://a950523a.github.io/TES-Taiwan-Electric-Scooter-Charging-Controller/`
- Web UI pull: "更新至最新韌體" → `POST /ota`
- Manual upload: `POST /ota/upload` — **needs the CSRF header** (see Security below):
  ```bash
  curl -H "X-TES-Request: 1" --data-binary @tes_charger.bin http://tes-charger.local/ota/upload
  ```
  Without the header the device answers `403 missing X-TES-Request header`.

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
|   |   +-- web/hw.html         hardware status page, served at /hw (board SVG generated in the private hardware repo)
|   |   +-- hwtest_svc.c/.h     bench test mode: request + lease (conditions live in task_tes_sm.c)
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
+-- schematic/              V1.3 原理圖 PDF（唯一公開的硬體檔；layout、BOM、生產檔在私有 repo）
```
