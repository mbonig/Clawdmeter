## Why

The user owns a Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (SKU 33762) and wants Clawdmeter running on it. It's a rectangular 5" 720×1280 portrait panel, but it's a different hardware class from every board this project has ported so far: first ESP32-P4 SoC (RISC-V, no native Wi-Fi/BLE radio), first MIPI-DSI panel (every existing port is QSPI/SPI), and first board where BLE has to come from a companion ESP32-C6-MINI co-processor over SDIO rather than the main MCU's own radio.

## What Changes

- Add a new board folder `firmware/src/boards/waveshare_p4_touch_lcd_5/` implementing the existing HAL contract (`display_hal`, `touch_hal`, `input_hal`, `power_hal`, `imu_hal`) for this board:
  - Display: MIPI-DSI 2-lane panel, 720×1280 portrait, driver chip **HX8394**, confirmed from Waveshare's official `waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5` BSP source (not disclosed on the docs page itself; an earlier guess of EK79007 from a different, unofficial community repo was wrong for this SKU).
  - Touch: **GT911** over I2C, confirmed from the same official BSP source (I2C SDA=GPIO7, SCL=GPIO8; no dedicated touch RST/INT pins — I2C-only).
  - Buttons: **BOOT only**. The board's second control is a hardware RST line (like the LCD-1.85B's missing PWR button), not a readable GPIO — so screen-cycling / hold-to-pair / splash-cycling gestures that depend on a second button are unavailable here, same posture as `waveshare_lcd_185b`.
  - Power: no PMU chip and no fuel gauge disclosed in Waveshare's docs — only an MX1.25 3.7V battery connector. Battery percentage reporting is **not implemented** in this change; `power_hal_battery_pct()` returns a stub until hardware investigation finds a real source (raw ADC divider, or none).
  - IMU: none on this board — no auto-rotation, fixed orientation (matches the AMOLED-1.8/2.06/C6 ports' posture).
- Add a new PlatformIO env `[env:waveshare_p4_touch_lcd_5]` targeting the ESP32-P4, bringing up the onboard ESP32-C6-MINI as a Wi-Fi 6 / BLE 5 co-processor over SDIO via ESP-Hosted, so the shared `ble.cpp` (NimBLE-Arduino) can keep working unmodified if ESP-Hosted's virtual HCI transport is transparent to it — **to be confirmed by the feasibility spike below**, not assumed.
- **First task is an explicit feasibility spike**, not board.h fill-in: confirm (a) Arduino-framework maturity for ESP32-P4 + MIPI-DSI under pioarduino (Waveshare's own docs for this SKU recommend ESP-IDF and call Arduino support "currently limited"), (b) whether GFX Library for Arduino supports MIPI-DSI panels at all (it's SPI/QSPI-only in every board this project has shipped), (c) whether NimBLE-Arduino can run transparently over ESP-Hosted's SDIO-tunneled BLE, and (d) the real display/touch controller chips on the actual unit. This is intentionally sequenced before the rest of the port because a "no" on (a)/(b)/(c) changes the shape of the whole change (e.g. dropping to ESP-IDF-native display/BLE code called from Arduino, or vendoring a driver) rather than being a normal per-board HAL fill-in.
- Explicitly **out of scope** for this change: the OV5647 camera (MIPI-CSI), the onboard ES8311/ES7210 audio codec + mic array, and battery percentage — same "not wired up yet" posture this project already takes with unverified subsystems on other boards (e.g. the 2.06 board's chime path).

## Capabilities

### New Capabilities
- `waveshare-p4-touch-lcd-5-board`: the HAL implementation and PlatformIO build environment that bring Clawdmeter's shared UI/splash/BLE code up on the ESP32-P4-WIFI6-Touch-LCD-5 — MIPI-DSI display, I2C touch, BOOT-only input, no-battery power stub, no-IMU fixed orientation, and Wi-Fi6/BLE5 via the onboard ESP32-C6-MINI co-processor.

### Modified Capabilities
(none — no existing capability specs are captured for prior board ports; this is the first board port tracked through OpenSpec)

## Impact

- **New**: `firmware/src/boards/waveshare_p4_touch_lcd_5/` (board.h, board_init.cpp, display.cpp, touch.cpp, input.cpp, power.cpp, imu.cpp, caps.cpp), a new `[env:waveshare_p4_touch_lcd_5]` block in `firmware/platformio.ini`, and whatever `lib_deps`/build flags the feasibility spike determines are needed for MIPI-DSI + ESP-Hosted (may include a newer pioarduino platform release than the pinned `55.03.38-1`, and/or a MIPI-DSI-capable display library alongside or instead of GFX Library for Arduino).
- **Possibly new pattern in `board_init.cpp`**: bringing up a companion radio chip over SDIO before BLE/Wi-Fi init, which no existing board does (existing IO-expander bring-up in `board_init.cpp` is the closest precedent).
- **Conditional shared-code risk**: if ESP-Hosted's BLE transport isn't transparent to NimBLE-Arduino, `firmware/src/ble.cpp` and/or `firmware/src/hal/` may need new abstraction beyond a per-board file — this would break the project's "zero `#ifdef BOARD_*` in shared code" rule and is called out explicitly so it isn't discovered mid-implementation. The spike's findings determine whether this happens; if it does, it may warrant being split into a follow-up change rather than folded silently into this one.
- **Docs**: `CLAUDE.md`'s board list and `docs/porting/adding-a-board.md` gain this board once verified; `adding-a-board.md`'s "at minimum" hardware list currently assumes QSPI/SPI + a same-chip radio, both of which this board violates, so the guide likely needs a short "companion-radio / DSI boards" note either way.
- **Risk profile**: materially higher-risk than prior ports (first DSI panel, first non-native-radio BLE, docs explicitly warn Arduino support is limited) — mitigated by sequencing the spike before any hardware-specific code is written.
