# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. Adding a board means dropping in
a new folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Six ports today (three SoC families — Xtensa S3, RISC-V C6, RISC-V P4 — four panel shapes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, BLE) verified on hardware.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`. Display, touch, battery, IMU init, and BLE verified on hardware; the ES8311 chime path is not wired up (`sound.cpp` no-ops).
- `boards/waveshare_lcd_185b/` — Waveshare ESP32-S3-Touch-LCD-1.85B (ST77916 TFT, 360×360 **round**, CST816S touch, BQ27220 fuel gauge). Build env: `waveshare_lcd_185b`. **Verified on physical hardware (2026-07-02).** Different hardware family from the AMOLED ports: no AXP2101 PMU (battery comes from the BQ27220 fuel gauge instead — see `fuel_gauge.cpp`), no IO expander (LCD/touch reset are direct GPIOs), and **no secondary/PWR button** — stock hardware only exposes BOOT + a hardware RESET, so splash-cycling, brightness-cycling, and the hold-to-pair gesture are unavailable on this board (`power.cpp`'s PWR edge functions always return false). Backlight is a physical LEDC PWM pin (GPIO5), unlike the AMOLED boards' command-based OLED brightness. Pins verified against Waveshare's official example repo (`waveshareteam/ESP32-S3-Touch-LCD-1.85B`).
- `boards/waveshare_p4_touch_lcd_5/` — Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (HX8394 **MIPI-DSI** panel, 720×1280 portrait, GT911 touch, ESP32-P4 SoC, BLE via the onboard ESP32-C6-MINI over ESP-Hosted). Build env: `waveshare_p4_touch_lcd_5`. **First MIPI-DSI port, first RISC-V-P4 port, first companion-radio BLE port; verified end-to-end on physical hardware (2026-07-23/24), including a real macOS pairing.** BLE runs a board-local `ble.cpp` against arduino-esp32's bundled `BLE` library, not shared `ble.cpp` (h2zero/NimBLE-Arduino doesn't build for P4) — see the dedicated section below.
  - **ST77916 init gotcha (see `st77916_init.h`):** Arduino_GFX's built-in default init table is tuned for a different physical panel batch than this board ships and renders garbled/torn on real hardware. Waveshare's own demo ships two vendor-tuned tables ("version_1"/"version_2") and probes a register at boot to pick the right one — this board uses "version_2" hardcoded (verified; "version_1" produces a solid black screen on the unit tested). **Crucially, neither Waveshare table sets COLMOD** — without an explicit `WRITE_C8_D8, 0x3A, 0x05` (note: 0x05, not the MIPI-standard 0x55 — ST77916 uses its own encoding), colors render as uniform gray regardless of what RGB565 data is sent. This was the actual root cause after ruling out QSPI clock speed (tried 1MHz through 40MHz, no difference) and the vendor table choice.
  - **Round-safe usage screen (2026-07-21).** `compute_layout()` and `init_usage_screen()` in `ui.cpp` branch on `BoardCaps::is_round` (set true only here) into a dedicated layout: the rectangular current/weekly bar panels are replaced by two `lv_arc` semicircle gauges sharing one ring (`make_gauge_arc`/`make_usage_gauge_round`) — top half = current, bottom half = weekly, each independently filled 0–100% via `set_gauge()`, with a thin divider bar drawn across the ring's full width *after* both arcs (so it renders on top, reading as a clean cut rather than getting hidden under the arc) to keep the two halves visually distinct. No logo (no room for the 80px mark) and no title text — the header is the battery meter alone, dead-center top, scaled to ~2/3 size via `lv_image_set_scale()` (LVGL zooms around the image's own center, so the logical 48×48 positioning math is unchanged; only the rendered footprint shrinks) since a full-size icon there either clips the round bezel or pokes into the ring. `round_r_out` (116, down from an initial 130) trades a bit of ring size for a taller header band — this geometry was tuned against *physical* bezel-cropping feedback that `screenshot.sh`'s raw framebuffer dump can't detect on its own (the screenshot shows the LVGL canvas, not what the curved cover glass physically obscures), so treat the current numbers as a good-enough starting point, not gospel — expect another pass if the physical crop persists. New round ports inherit the same branch via their own `is_round = true` cap but will likely need their own geometry pass if the panel isn't ~360×360.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → Space/voice-mode, active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → cycle screens; on splash → cycle animations). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

### AMOLED-2.06 (watch form factor) — `waveshare_amoled_206`
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the `Arduino_CO5300` constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (23 was picked empirically for centering; Waveshare's reference library uses 22). The 2.16 dodges this because its 480×480 viewport fills the controller's RAM.
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline FocalTech reader as the 1.8 port (no GPLv3 `Arduino_DriveBus` dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY IRQs (short / long / positive), same path as the 2.16 — no IO expander.
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. The amp path is unverified on this board, so `sound.cpp` no-ops (same posture as the C6 1.8) — the shared `chime.cpp` engine is ready to wire up once it's tested on hardware.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wired directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → cycle screens; hold-to-pair). **No third button**.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

### P4 Touch LCD-5 — `waveshare_p4_touch_lcd_5`
Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (SKU 33762). First non-QSPI/SPI board and first ESP32-P4 (RISC-V, no native radio) port. See `openspec/changes/add-esp32-p4-core-board/` (design.md/tasks.md) for the full bring-up story — the summary below is the durable reference.

- Display: **HX8394** via **MIPI-DSI** (2-lane, 720×1280 portrait) — driven directly against ESP-IDF's `esp_lcd_mipi_dsi`/`esp_lcd_panel_ops` APIs from `display.cpp`, **not** GFX Library for Arduino (it has no DSI panel class at all). The driver itself is Waveshare's official `esp_lcd_hx8394` source, vendored directly into `boards/waveshare_p4_touch_lcd_5/hx8394/` (MIT license, provenance noted in that folder) rather than pulled via PlatformIO's component manager — **`framework = arduino` never processes `idf_component.yml`** (confirmed empirically: a root-level or `src/`-level file is silently ignored, no fetch attempt), so any ESP-IDF managed component needed on this board has to be vendored by hand the same way.
  - DSI PHY power: ESP32-P4's MIPI DPHY needs the chip's internal adjustable LDO at channel 3 / 2500mV — this is a fixed hardware constant of the SoC, not a per-board choice, matching Espressif's own DSI examples.
  - **DMA2D gotcha:** `esp_lcd_panel_draw_bitmap()`'s async DMA2D path (`dpi_config.flags.use_dma2d = true`) queues the copy and returns before it's actually done; LVGL believes the source buffer is free to reuse as soon as the call returns, since it just calls `lv_display_flush_ready()` right after. Symptom on hardware: backlight and DSI stream both work, a raw solid-color `display_hal_fill_screen()` renders fine, but real LVGL/UI content never appears — no crash, no error, just silently stale pixels. Fixed by using the driver's synchronous CPU-copy path (`use_dma2d = false`) instead; a retry loop on the driver's own busy state (`ESP_ERR_INVALID_STATE`) handles back-to-back `draw_bitmap` calls regardless.
  - Single framebuffer (`num_fbs = 1`) — Waveshare's own BSP bumps this to 3 for tearing avoidance, but that needs cooperating buffer-rotation bookkeeping this board's plain HAL doesn't have.
- Touch: **GT911** via I2C (SDA=7, SCL=8, addr=0x5D, no RST/INT pins wired on this board — hand-rolled inline reader in `touch.cpp`, no `esp_lcd_touch` dependency).
  - **Register layout (get this right — it caused a two-session debugging saga):** the point-1 record starts at **`0x814F`**, and coordinate pairs are **little-endian**:
    `0x814F` track id, `0x8150`/`0x8151` X low/high, `0x8152`/`0x8153` Y low/high, `0x8154`/`0x8155` size low/high. Verified against Espressif's official `esp_lcd_touch_gt911` component (a copy lives in the sibling `../ESP-Streaming-Deck` project's `managed_components/`).
  - **Resolved 2026-07-27 — the "unreliable touch coordinates" on this board were entirely a driver bug, not hardware.** The reader started its record read at `0x8150` and assumed *that* byte was the track id, so every field was shifted one byte, and it also treated the pairs as big-endian. The net effect was combining bytes from *adjacent* fields: `x = (X_high << 8) | Y_low` and `y = (Y_high << 8) | size_low`. That single off-by-one produced every symptom previously blamed on flaky hardware or "human aiming variance":
    - X readings compressed and genuinely **non-monotonic** (tapping further right could read lower) — because X was largely reporting *Y's low byte*.
    - Y appearing to **saturate near ~1080** and moving only ~10-15px across large finger movements — because it was dominated by `Y_high << 8` (changing only every 256px) plus the touch-size byte as noise. `y = (3 << 8) | size` ≈ 768–1023 for anything in the lower half of the panel.
    - Taps "landing 150-300px short" near the bottom of the screen.
    Several workarounds had been built on top of the bad data and are all now **removed**: an empirical 5/4 X scale factor, a "reject any report that isn't exactly 1 touch point" filter, a two-sample agreement filter (whose comparison almost never ran anyway — the first read clears the `0x814E` status bit, so the immediate second read finds no fresh data), a 300px `global_click_cb` dead zone, and a claim that buttons needed ≥100px inset from the bezel. **Lesson: when a hand-rolled register reader produces coordinates that are self-consistent but geometrically impossible, suspect the field offsets before the hardware** — and diff against the vendor/official driver early rather than accumulating empirical corrections.
- Buttons: GPIO 0 (BOOT → Space/voice-mode) only. **No PWR button** — this board's only other control is a hardware RST line, not a readable GPIO (same posture as `waveshare_lcd_185b`), so splash-cycling / hold-to-pair are unavailable. Since the shared hold-to-pair gesture (which calls `ble_clear_bonds()`) can never fire without a PWR button, a `clearbonds` serial command was added to `main.cpp`'s `check_serial_cmd()` (alongside `screenshot`/`buzz`) as the only way to clear bonds on this board — harmless on every other board too, just redundant with the physical gesture there.
- Power: no PMU or fuel gauge disclosed by Waveshare (only a bare MX1.25 3.7V connector) — `power.cpp` is a deliberate "no battery" stub, not a placeholder to fill in later without new hardware investigation.
- IMU: none. Fixed orientation.
- **BLE works, via a board-local `ble.cpp` — not shared `ble.cpp`.** `h2zero/NimBLE-Arduino` (what shared `ble.cpp` is written against) fails to build for ESP32-P4 outright — confirmed on real hardware (a genuine compiler error in its own FreeRTOS porting layer, not a config issue) and corroborated by the library's own maintainer (h2zero/NimBLE-Arduino#906: *"This repo cannot support [ESP32-P4]... You will need to use esp-nimble-cpp and esp-idf"*). The fix that actually works: **arduino-esp32's own *bundled* `BLE` library** (`#include <BLEDevice.h>`, not `<NimBLEDevice.h>`) — a completely different C++ wrapper that links against the same precompiled NimBLE-over-ESP-Hosted stack already baked into the installed `framework-arduinoespressif32-libs` package (`CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=1` and `libbt.a` ship with it already; no component-manager workaround, no hybrid `framework = espidf, arduino` needed — plain `framework = arduino` is enough). This board's `[env:waveshare_p4_touch_lcd_5]` excludes shared `ble.cpp` from `build_src_filter` and substitutes its own `boards/waveshare_p4_touch_lcd_5/ble.cpp`, reimplementing the same `ble.h` contract against this different library. Onboard co-processor: **ESP32-C6-MINI** over SDIO (ESP-Hosted) — the SDIO pins (CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17, RESET=54) are arduino-esp32's generic `esp32p4` variant defaults and needed no board-specific override; confirmed against the official schematic that Waveshare wired this board the same way.
  - **API differences from NimBLE-Arduino** (the bundled `BLE` library is an older generation): server/characteristic callbacks receive a raw `ble_gap_conn_desc*` instead of a rich `NimBLEConnInfo&` — build addresses/state from the raw struct (`BLEAddress(desc->peer_id_addr)`, `desc->sec_state.encrypted`, etc.) instead of calling wrapper methods. HID setters drop the `set` prefix (`reportMap`/`manufacturer`/`pnp`/`hidInfo` vs `setReportMap`/...). No C++ wrapper for bond enumeration/deletion at all — goes straight through NimBLE's own `ble_store_util_bonded_peers`/`ble_store_util_delete_peer`/`ble_store_clear` C functions. No `onAuthenticationComplete` callback — the single-owner-lock's owner-claiming (see shared `ble.cpp`'s header for the full rationale) happens entirely in `onWrite`'s existing encrypted-link check instead, and the Windows supervision-timeout pushback arms directly from `onConnect`'s own `conn_desc` fields rather than a separate auth-complete hook.
  - **Gotcha (cost a full crash-reboot-loop debugging pass — see `openspec/changes/add-esp32-p4-core-board/design.md` for the story):** `BLEHIDDevice::manufacturer(String)` (the setter) writes to `m_manufacturerCharacteristic`, but that member is *only* created by the no-arg `manufacturer()` getter — unlike `reportMap`/`pnp`/`hidInfo`/`setBatteryLevel`, whose backing characteristics are all pre-created in `BLEHIDDevice`'s own constructor. Calling the setter without calling the getter first is a null-pointer dereference into `setValue()` (`Guru Meditation Error: Load access fault`, silent crash-reboot loop, no compile-time warning). Fix: call `hid_dev->manufacturer();` (discard the return value) once before `hid_dev->manufacturer("Anthropic");`.
  - **Gotcha:** `BLEHIDDevice::pnp()` packs each 16-bit field big-endian (high-byte-first), but the Bluetooth SIG's PnP ID characteristic (0x2A50) spec requires little-endian. Confirmed on hardware via `system_profiler SPBluetoothDataType`: macOS showed a byte-swapped Vendor/Product ID (`0xE502`/`0x0100` instead of the intended `0x02E5`/`0x0001`) until each field was pre-swapped before calling `pnp()` to cancel out the library's own swap.
  - **Debugging note:** `Serial.print`/`printf` output was highly unreliable on this board throughout BLE bring-up (lines silently missing, especially anything printed within the first ~750ms of boot or in quick succession) — `esp_log.h`'s `ESP_LOGE(tag, ...)` proved completely reliable in every case tested and is the better tool for bisecting hangs/crashes on this board specifically.
  - **macOS bonding desync (2026-07-24):** after a real successful pairing + data flow, the device disconnected and the host daemon's `retrieveConnectedPeripheralsWithServices_` lookup (see `daemon/claude_usage_daemon.py`) stopped finding it at all, even though `system_profiler` still showed it "Connected" as an HID keyboard — the OS's native HID auto-connect and CoreBluetooth's app-facing connected-peripheral cache had gone out of sync (most likely triggered by the intervening reflash). Neither side self-healed. Fix: **forget the device in System Settings → Bluetooth (not just disconnect — `blueutil --unpair` alone did not clear it) and re-pair from scratch**; that's the general recovery for this symptom on any board, not P4-specific. Reproduced again during the media-controls work below — every reflash mid-connection seems to trigger it, not a one-off.
- **BLE HID requires LE Secure Connections AND an OS-flow pairing (learned the hard way 2026-07-27).** Two independent requirements, both non-obvious:
  - `BLESecurity::setAuthenticationMode(true, false, true)` — the third arg (SC) **must** stay `true`. With legacy pairing, macOS pairs and even lists the device, but never attaches its HID stack: it never writes either report's CCCD, so every `ble_keyboard_press()`/`ble_consumer_press()` notify is a silent no-op. Verified by toggling this one flag.
  - The device must be paired via **System Settings → Bluetooth** (which runs the "set up your keyboard" assistant). A CoreBluetooth *app-level* connect (e.g. a `bleak`/Python script, which is a tempting way to force a connection when the daemon can't find the device) bonds for GATT access only and never enrols HID — `system_profiler SPBluetoothDataType` shows no "Minor Type: Keyboard" line in that case, which is the quick way to tell the two apart. Under a proper OS pairing macOS also *holds* the connection and auto-reconnects after a reflash, which is the behaviour `daemon/claude_usage_daemon.py`'s `discover_target()` is designed around.
- **Register every GATT service BEFORE the server starts (2026-07-27).** `BLEHIDDevice::startServices()` internally calls `BLEServer::start()`, which activates the whole GATT database. The custom data service used to be created *after* that call and registered by a second `server->start()` — i.e. bolted onto an already-running NimBLE GATT server, which needs a full re-registration to take effect. Symptom: the daemon connected fine but every write failed with `Characteristic ...0002 was not found`, and an app-level service dump showed only the *first* batch (`0x180A`, `0x180F`, plus `0x1812` which macOS hides) — the custom service was simply missing. Correct order in `ble_init()`: create the custom service and its characteristics, `svc->start()` to add it, then `hid_dev->startServices()` last so the server starts exactly once. **macOS caches the GATT database per bond, so after changing service structure you must forget + re-pair before the host will see the change** — otherwise a correct firmware fix looks like it did nothing.
- **Vendored + patched BLE library: `firmware/lib/BLE/` (added 2026-07-27).** arduino-esp32's bundled `BLE` library registers only **one characteristic per UUID per service** on its NimBLE path: `BLEService::addCharacteristic()` finds an existing characteristic with the same UUID and then just clears `m_removed` on *that* one, never inserting the new object. The new `BLECharacteristic` stays a live C++ object that was never registered, so its handle remains `0xFFFF` (`NULL_HANDLE`) and it is invisible to every host. Since **every** HID report characteristic uses UUID `0x2A4D`, only the first `BLEHIDDevice::inputReport()` ever registered — which is why the media buttons could never work no matter what the host did. Proven with an order-swap experiment (whichever report was created *second* came back with handle `0xFFFF`). Duplicate UUIDs are legal in GATT (reports are disambiguated by their `0x2908` Report Reference descriptor) and the library's own map is keyed by pointer, so the fix is a few lines: only take the un-remove path when the *same object* is being re-added. PlatformIO's LDF prefers the project-local copy and only links it where `<BLEDevice.h>` is included, i.e. this board alone — every other env builds untouched. **This is the 4th bug found in this library on this board** (after the `manufacturer()` null deref, the `pnp()` byte-swap, and this); when something in it silently does nothing, suspect the library.
- **Media/volume control bar — this board only (`BoardCaps.has_media_controls`, see `openspec/changes/add-media-volume-controls`).** A second BLE HID input report (Report ID 2, Consumer usage page 0x0C, single 16-bit usage-code selector) drives real OS-level Volume Up/Down, Mute, Play/Pause, and Next Track from five touch buttons rendered under the usage panels. Gated by a new `BoardCaps` flag — deliberately *not* inferred from screen geometry, since AMOLED-216/206 share this board's "large" (`height >= 460`) layout breakpoint but must stay unaffected. Shared `ble.cpp` (every non-P4 board) implements the same `ble_consumer_press()`/`ble_consumer_release()` API as a no-op stub — real implementation lives only in this board's `ble.cpp`, so a future board enabling the flag on the NimBLE stack needs its own report-map addition mirroring this one, not a shared-code change. `compute_layout()`'s control-bar geometry is bottom-anchored off `scr_h` rather than tied to the panel offsets, since this breakpoint's absolute pixel constants were tuned for a 480-tall screen and this board is 1280 tall — anchoring off the top would leave the bar stranded mid-screen with hundreds of pixels of dead space below it. The bar is parented to `usage_container`, **not** `usage_group`: the latter is hidden whenever usage data goes stale, which made the media buttons vanish behind the idle "Listening…" screen exactly when you'd still want to change the volume. `update_view_state()` hides the bar only on the pairing view (BLE down, where presses can't work anyway). It is also explicitly raised with `lv_obj_move_foreground()` after `build_pair_group()`/`build_idle_group()` — those are created later and span everything below the header, so in LVGL's create-order z-stacking they otherwise sit on top and silently swallow every tap aimed at the bar.
- **Center rotator (clock + market quotes) — the only board that gets one today.** The dead band between the weekly panel (ends y=416, from the 480-tall-tuned "large" breakpoint) and the bottom-anchored media bar (y=850) is ~434px on this 1280-tall panel. `compute_layout()` fills it with a panel that cycles the wall clock and up to `MAX_QUOTES` (3) market quotes, 6s each, fading the text stack in on each switch, with page dots along the bottom. Enabling it is **geometric, not a `BoardCaps` flag** (unlike `has_media_controls`): a board gets one iff ≥240px of slack is left after the panels, which no other board comes close to — the decision *is* "is there a big empty band here", and being purely decorative it can't misbehave the way an unimplemented BLE report can. It lives in `usage_group`, so it hides with the rest of the numbers when data goes stale (a stale price is misleading), unlike the media bar which deliberately survives into the idle view. On boards with a rotator the title clock is suppressed and the title stays "Usage" — the same clock twice on one screen reads as a bug. Quotes arrive as an optional `"q":[{"n","p","d"}]` payload field; the daemon pre-formats the price string (currency/precision stay host-side) and only the percent change crosses as a number, because the firmware colors it by sign.
- **Host→device serial input does not work on this board (found 2026-07-30).** Serial *output* comes through (boot logs at least — see the reliability caveat above), but nothing written to the CDC port reaches `Serial.available()`: `screenshot`, `buzz`, and `clearbonds` were all sent with `\n`, `\r\n`, and `\r` terminators against a freshly flashed build and produced no response at all. Consequences: **`screenshot.sh` cannot capture this board** (it hangs forever waiting for `SCREENSHOT_START`, since its read loop has no overall timeout), so UI changes here must be eyeballed on hardware like a C6 port; and the `clearbonds` serial command documented above as the *only* way to clear bonds on this PWR-button-less board is in practice unreachable. Not investigated further.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    waveshare_amoled_216_c6/— C6: SH8601 + CST9217 + AXP PKEY, no PSRAM
    waveshare_amoled_18_c6/ — C6: SH8601 + FT3168 + AXP PKEY + TCA9554 (gates power), no PSRAM
    waveshare_amoled_206/   — CO5300 + FT3168 + AXP PKEY, no IO expander, 32 MB, no rotation
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — 3-screen UI (splash, usage, bluetooth). compute_layout() picks fonts/positions from board_caps() (responsive — current breakpoint: H >= 460 → large, else compact)
  splash.{h,cpp}            — 20×20 pixel-art engine. CELL = min(W,H)/20, centered.
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard
  data.h                    — UsageData struct
  icons.h                   — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (S3, default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (S3)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build 2.16 (C6)
pio run -d firmware -e waveshare_amoled_18_c6                                   # build 1.8 (C6)
pio run -d firmware -e waveshare_amoled_206                                     # build 2.06 (S3, watch)
pio run -d firmware -e waveshare_lcd_185b                                       # build round 1.85B (S3)
pio run -d firmware -e waveshare_p4_touch_lcd_5                                 # build P4 Touch LCD-5 (P4, no BLE)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
# C6 boards: same native USB-JTAG flashing; flag a chip mismatch ("This chip is ESP32-C6,
# not ESP32-S3") means you picked an S3 env — use a *_c6 env for C6 hardware. Same applies
# to the P4 env (error will say "This chip is ESP32-P4, not ESP32-S3").
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- **P4 center rotator + market quotes (2026-07-30).** Filled the P4's ~434px dead band with a rotating clock/quotes panel (details in the P4 board section) and taught the Python daemons a `tickers` config option. Quotes come from Yahoo's `v8/finance/chart/<sym>?range=1d&interval=1d` endpoint — `/v7/finance/quote` now needs a cookie+crumb handshake, while the chart endpoint still works with just a browser-ish User-Agent and returns both the last price and the previous close. Cached 5 min host-side so a 60s usage poll isn't a 60s quote poll. `tickers = spacex` resolves to **SPCX** (Space Exploration Technologies Corp., NasdaqGS — listed June 2026, after the model knowledge cutoff that first sent this down a "SpaceX is private, use the DXYZ proxy" detour); the device always shows the symbol actually priced. The Bash Linux daemon does **not** send `q` (would need a JSON parser it avoids); firmware degrades to the clock alone.
- **AMOLED-1.8 chime verified on hardware + EXIO2 touch-kill fix (2026-07-13).** The 1.8's `amp_enable` hook drove both GPIO 46 and XCA9554 EXIO2 ("the unused one is harmless") — but pulling EXIO2 low takes the FT3168 off the I2C bus (chip stops ACKing; IDF reports it as `ESP_ERR_INVALID_STATE`, which reads like a driver wedge and cost a long I2S red-herring chase). Amp enable is GPIO 46 only; EXIO2 must stay HIGH. Chime, touch, buttons, and BLE bond persistence all verified on a real 1.8.
- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

Bash daemon (`daemon/claude-usage-daemon.sh`) reads OAuth token, polls Anthropic API, sends JSON over BLE GATT. Run with `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path to the script — repoint it when switching between the worktree and the main checkout.

**Discovery & resilience:**

- Connects by name (`"Clawdmeter"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- **macOS device lookup (`retrieve_connected_macos`) must fall back to `0x180A`/`0x180F` (2026-07-27).** The daemon never scans by name — it only attaches to a peripheral the OS already holds, via `retrieveConnectedPeripheralsWithServices_`. But once the device is paired as a BLE HID keyboard (required for its buttons to work), that call returns **empty** for our custom service UUID *and* for `0x1812`: macOS only indexes services it discovered itself, never does a full discovery on an app's behalf, and deliberately hides HID from all app-level BLE clients. It does return the device for `0x180A` (Device Information) and `0x180F` (Battery), so those are used as name-matched fallbacks. Without them the daemon logs `Device not held by OS; waiting` forever while the device sits there connected. The custom service only becomes visible *after* connecting and discovering — which is why the lookup can't rely on it.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC. Multi-candidate scans pick `head -1` and let the failure cycle converge.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
