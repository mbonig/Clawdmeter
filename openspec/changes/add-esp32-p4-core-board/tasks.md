## 1. Feasibility spike (do this before anything else)

- [x] 1.1 Set up a minimal ESP32-P4 Arduino/PlatformIO project targeting this board — **done with real hardware**: user connected the actual board mid-session (esptool confirms genuine ESP32-P4 rev v1.3). Built and flashed successfully using the exact pinned toolchain (`55.03.38-1`, arduino-esp32 3.3.8) — no newer platform release needed for basic build/flash/boot.
- [x] 1.2 ~~Probe the actual display driver and touch controller chips on the physical unit~~ — resolved via Waveshare's official `waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5` BSP source: **HX8394** display (not the originally-assumed EK79007) + **GT911** touch, I2C SDA=GPIO7/SCL=GPIO8, backlight=GPIO26, LCD_RST=GPIO27, no touch RST/INT pins. design.md and proposal.md updated.
- [x] 1.3 Get a MIPI-DSI test pattern rendering via GFX Library for Arduino (or confirm neither works under Arduino) — **GFX Library confirmed no-go** (no DSI class). But went further and got a positive result: vendored Waveshare's official `esp_lcd_hx8394` driver source directly (bypassing GFX Library and the component manager) and it **built, linked, and flashed successfully on the real board** under `framework = arduino`. Display is portable via vendored ESP-IDF-native driver code — see design.md Round 2 findings. Full pixel-on-panel bring-up (power/reset/backlight sequencing) is deferred to section 3, not a feasibility blocker.
- [x] 1.4 Bring up the onboard ESP32-C6-MINI over ESP-Hosted (SDIO) far enough to confirm Wi-Fi and/or BLE functions at all from the P4 side — **not attempted directly**; superseded by 1.5's more decisive finding (the BLE host stack itself doesn't build, so there's nothing meaningful to bring up yet on the P4 side).
- [x] 1.5 Attempt a minimal NimBLE-Arduino advertise/connect over the ESP-Hosted link — **done, hardware-verified negative result**: a live build of this project's actual `h2zero/NimBLE-Arduino@^2.1.1` dependency (resolved to latest 2.5.0) against the real P4 target fails with genuine compiler errors in NimBLE-Arduino's own FreeRTOS porting layer. Corroborated by the library maintainer's own statement (h2zero/NimBLE-Arduino#906) that Arduino-core BLE on P4 isn't supported by design, and by a second independent bug report showing the ESP-IDF-native fallback (`esp-nimble-cpp`) also fails under PlatformIO (both `framework = arduino` and `framework = espidf`) with a `syscfg.h` component-resolution error — root-caused here to PlatformIO's `framework = arduino` build never invoking the `idf_component.yml` component manager at all (confirmed by testing directly).
- [x] 1.6 Write up spike findings and update design.md's Open Questions — done (see design.md's "Spike Findings — Round 2"). **Revised result: this is a split verdict, not a uniform block. Display+touch are portable now (vendored driver approach, hardware-verified). BLE is not portable today under any framework combination tried, and this is corroborated by multiple independent sources, not just this session's testing. Stopping here per this task's own rule — do not continue to section 2 until the user decides how to proceed given the BLE-specific blocker (see design.md's numbered options).**

## 2. Board scaffold

- [x] 2.1 `cp -r firmware/src/boards/template firmware/src/boards/waveshare_p4_touch_lcd_5` — done, plus a vendored `hx8394/` subfolder (Waveshare's official `esp_lcd_hx8394` driver, MIT-licensed, with provenance notes) since GFX Library has no MIPI-DSI class.
- [x] 2.2 Fill in `board.h`: HX8394 display, GT911 touch confirmed. LCD_WIDTH=720/LCD_HEIGHT=1280 native portrait. `BOARD_HAS_SECONDARY_BUTTON=0`, `BOARD_HAS_ROTATION=0`, `BOARD_HAS_IMU=0`, `BOARD_HAS_BATTERY=0`, `BOARD_HAS_IO_EXPANDER=0` — all confirmed correct on hardware.
- [x] 2.3 Added `[env:waveshare_p4_touch_lcd_5]` to `firmware/platformio.ini`: `board = esp32-p4`, `build_src_filter` excludes shared `ble.cpp` (see section 8), 32MB flash/`default_32MB.csv`, no GFX Library/SensorLib/XPowersLib/NimBLE-Arduino deps (none needed for this board).

## 3. Display

- [x] 3.1 `display_hal_init()`/`display_hal_begin()` implemented directly against `esp_lcd_mipi_dsi`/`esp_lcd_panel_ops` (not GFX Library) via the vendored HX8394 driver. DSI PHY LDO (channel 3, 2500mV — standard ESP32-P4 hardware constant), DSI bus, panel IO, panel create/reset/init/disp_on, LEDC backlight on GPIO26 — all verified working on real hardware.
- [x] 3.2 `display_hal_draw_bitmap()`/`display_hal_round_area()` implemented. Real bug found and fixed on hardware: the DPI panel's async DMA2D copy path let LVGL believe a source buffer was free before the copy actually finished (silent no-op rendering, not a crash) — switched to the synchronous CPU-copy path (`use_dma2d = false`) plus a retry loop on the driver's own busy state, which is what actually surfaces as `ESP_ERR_INVALID_STATE`. `round_area` is a no-op (no alignment requirement for this panel).
- [x] 3.3 `display_hal_set_brightness()` — LEDC PWM on GPIO26, confirmed working.
- [x] 3.4 LVGL buffer sizing (40-row strips, PSRAM) verified fine at 720×1280 — no PSRAM pressure.

## 4. Touch

- [x] 4.1 `touch_hal_init()`/`touch_hal_read()` implemented (hand-rolled GT911 reader, no RST/INT pins on this board). GT911 confirmed present at I2C 0x5D.
- [x] 4.2 Coordinate mapping verified and fixed on hardware: this unit's GT911 register layout has the coordinate bytes in high-byte-first order (opposite of some published GT911 register maps) — found by dumping raw register bytes and comparing against an actual tap location. Taps now correctly navigate the UI (splash → pairing-hint screen).

## 5. Input

- [x] 5.1 `input.cpp` implemented for BOOT only, following the `waveshare_lcd_185b` single-button pattern.

## 6. Power (stub)

- [x] 6.1 `power.cpp` implemented — "no battery present" stub, no-op PWR functions.

## 7. IMU (stub) / orientation

- [x] 7.1 `imu.cpp` no-op stub implemented. Fixed-orientation UI code paths work correctly with no shared-code changes (confirmed visually on hardware).

## 8. BLE / wireless bring-up

- [ ] 8.1 **Not attempted — deferred, per user decision.** The spike (section 1) found BLE-on-P4 is not portable under this project's Arduino/PlatformIO toolchain today (confirmed on real hardware: `h2zero/NimBLE-Arduino` fails to build for ESP32-P4; corroborated by the library maintainer and a second independent bug report). The user chose to ship display+touch now rather than attempt the much larger ESP-Hosted/esp-nimble-cpp vendoring effort. Revisit if/when upstream (NimBLE-Arduino, PlatformIO's component manager for `esp32p4`) fixes this.
- [x] 8.2 **Resolved without touching shared `ble.cpp` at all**, exactly as hoped: this board's PlatformIO env excludes `ble.cpp` from its `build_src_filter` and substitutes a new `ble_stub.cpp` (implements the same `ble.h` contract as a permanent no-op — `BLE_STATE_DISCONNECTED` always, no data ever available). `main.cpp` and every other shared file are untouched. Confirmed on hardware: the UI correctly shows the "not paired" pairing-hint screen instead of hanging or crashing on the stubbed BLE state.
- [ ] 8.3 Not applicable while BLE is stubbed — no daemon pairing possible yet. Re-open once 8.1 is revisited.

## 9. Visual QA

- [x] 9.1–9.3 Done via direct hardware iteration rather than `screenshot.sh` (works fine on this board — `LV_USE_SNAPSHOT` isn't disabled here since PSRAM is present — but live hardware was faster given the debugging already underway). Confirmed on the physical panel: splash animation renders correctly, touch navigates to the pairing-hint screen, and the pairing-hint text ("To pair... hold the power button...") is fully readable with no clipping at the existing "large" breakpoint's font sizes — no new breakpoint needed. **Found and fixed a real, general (non-P4-specific) bug in shared `ui.cpp` while debugging**: `compute_layout()`'s "large" and "compact" breakpoints never set `L.pct_font`/`L.reset_font` (left as null pointers), which `make_usage_panel()` passes straight into `lv_obj_set_style_text_font()`. This was apparently harmless on every existing Xtensa board shipped so far but hung LVGL's renderer outright on ESP32-P4 — fixed by setting both fields to sensible values (`font_styrene_48`/`font_styrene_20` for large, `font_styrene_48`/`font_styrene_16` for compact) matching what `ui_update()` already uses as the real font once data arrives. This fix benefits every board, not just this one.
- [ ] 9.4 N/A — no temporary default-screen change was needed; all QA was done by tapping past the splash screen live on hardware.

## 10. Docs

- [x] 10.1 Added to `CLAUDE.md`'s board list and a new "P4 Touch LCD-5" subsection under "Hardware (critical pins)" — chips, pins, the DMA2D/GT911-byte-order gotchas, and the BLE-deferred status with root-cause citations.
- [x] 10.2 Added a build command line for this board to `CLAUDE.md`'s "Build / flash" section.
- [ ] 10.3 Consider a short note in `docs/porting/adding-a-board.md` about MIPI-DSI boards (vendor the driver directly, use the synchronous CPU-copy path not DMA2D) and about the `L.pct_font`/`L.reset_font` null-pointer gotcha in `compute_layout()` — both are genuinely reusable lessons now, not speculative.
