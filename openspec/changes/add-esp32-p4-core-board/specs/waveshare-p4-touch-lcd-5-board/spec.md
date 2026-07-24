## ADDED Requirements

### Requirement: Feasibility spike gates hardware-specific implementation
The project SHALL confirm, on real hardware, that (a) the Arduino framework can drive this board's MIPI-DSI panel through GFX Library for Arduino or an equivalent Arduino-compatible display library, and (b) NimBLE-Arduino can operate over the onboard ESP32-C6-MINI's ESP-Hosted SDIO link, before `board.h`/`display.cpp`/`touch.cpp`/`ble`-related board code is written against either assumption. If either check fails, the board port SHALL be re-scoped (e.g. to an ESP-IDF-native display or BLE path) rather than proceeding on an unverified assumption.

#### Scenario: Spike confirms the Arduino/GFX/NimBLE path works
- **WHEN** the spike successfully renders a test pattern via GFX Library (or an equivalent Arduino-compatible library) on the DSI panel and successfully advertises/connects over BLE through the ESP-Hosted link
- **THEN** the rest of the board port proceeds using the same HAL-implementation pattern as every other board, with no shared-code changes required

#### Scenario: Spike finds the Arduino path does not work for the display or BLE
- **WHEN** GFX Library (or its equivalent) cannot drive this panel under Arduino, or NimBLE-Arduino cannot operate over the ESP-Hosted link
- **THEN** the port is re-scoped around the specific gap found (e.g. ESP-IDF-native display driving called from board-local code, or an alternate BLE integration path) before any further board-specific files are written, and the gap is documented rather than worked around silently

### Requirement: Shared UI renders correctly at 720×1280 portrait
The system SHALL render the existing shared UI (splash, usage, bluetooth screens from `ui.cpp`/`splash.cpp`) on this board's 720×1280 portrait panel with no changes to shared code, using `BoardCaps` to report the correct dimensions so `compute_layout()` selects an appropriate breakpoint.

#### Scenario: Usage screen fits the panel without clipping
- **WHEN** the usage screen is displayed on this board
- **THEN** all UI elements (battery meter, current/weekly panels, logo) render fully on-screen with no clipping against the panel's 720×1280 bounds, verified via a physical screenshot or on-hardware photo

#### Scenario: Splash animation renders centered
- **WHEN** the splash screen is displayed on boot
- **THEN** the 20×20 pixel-art animation renders centered using the existing `CELL = min(W,H)/20` sizing logic, unmodified

### Requirement: Touch input drives the shared UI via the HAL contract
The system SHALL implement `touch_hal_init()`/`touch_hal_read()` for this board's touch controller (assumed GT911 pending hardware confirmation per the feasibility spike), applying whatever axis swap/mirror this specific panel needs, so touch gestures already wired into the shared UI work without modification.

#### Scenario: Touch coordinates map correctly to on-screen elements
- **WHEN** the user touches a UI element (e.g. a screen-switch tap zone)
- **THEN** the reported touch coordinates match the physical touch location without axis swap or mirror errors

### Requirement: BOOT button is the sole physical input; no secondary-button gestures
The system SHALL implement `input_hal_is_held(PRIMARY)` for the BOOT button and report no secondary button via `BoardCaps` (`button_count = 1`), since this board's only other control is a hardware RST line, not a readable GPIO. Gestures that require a second button (screen-cycling via PWR, hold-to-pair, splash-cycling) SHALL be unavailable on this board, matching the existing `waveshare_lcd_185b` posture.

#### Scenario: BOOT button drives voice-mode / PTT
- **WHEN** the user holds the BOOT button
- **THEN** the shared input logic treats it as the PRIMARY button (Space/voice-mode), identical to every other board's BOOT button behavior

#### Scenario: No secondary-button gesture is exposed
- **WHEN** the shared UI or HID logic checks for a SECONDARY button press
- **THEN** the board's `BoardCaps` reports no secondary button and those code paths behave the same as they do on `waveshare_lcd_185b`

### Requirement: Fixed orientation, no auto-rotation
The system SHALL report no IMU capability via `BoardCaps` (`BOARD_HAS_IMU = 0`) and render the UI in a single fixed portrait orientation, since this board has no IMU.

#### Scenario: UI stays in portrait regardless of physical board orientation
- **WHEN** the board is physically rotated
- **THEN** the UI does not rotate, since rotation logic is compiled out for this board (matching AMOLED-1.8/2.06/C6 boards' fixed-orientation posture)

### Requirement: No battery percentage reported
The system SHALL implement `power_hal_battery_pct()` and `power_hal_is_charging()` to report "no battery present" values, since this board discloses no PMU or fuel-gauge chip in its documentation, rather than reading an unconfirmed ADC pin.

#### Scenario: Battery icon reflects no-battery state
- **WHEN** the usage screen renders its battery indicator on this board
- **THEN** it renders using the same no-battery/unknown state the shared UI already supports for boards without a PMU, not a fabricated percentage

### Requirement: BLE connectivity matches the existing daemon GATT contract
The system SHALL expose the same custom data service and characteristic UUIDs, JSON payload schema, and HID keyboard service that every other board's `ble.cpp` exposes, routed through the onboard ESP32-C6-MINI co-processor's Wi-Fi 6 / BLE 5 radio via ESP-Hosted, so the existing host daemon requires no changes to support this board.

#### Scenario: Daemon connects and delivers usage data without daemon changes
- **WHEN** the host daemon connects to this board over BLE
- **THEN** it writes usage data to the same characteristic UUID and the firmware parses and displays it identically to every other board, with no daemon-side code changes

#### Scenario: HID keyboard link works alongside the data service
- **WHEN** the BOOT button triggers a HID keypress (Space/voice-mode)
- **THEN** the OS receives the keypress over the same BLE HID service every other board exposes, concurrently with the data-service connection
