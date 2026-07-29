## ADDED Requirements

### Requirement: `BoardCaps` gains an explicit media-controls flag
The system SHALL add `bool has_media_controls` to `BoardCaps` (`hal/board_caps.h`), set to `true` only in the P4 board's (`waveshare_p4_touch_lcd_5`) `caps.cpp` and explicitly `false` in every other board's `caps.cpp`. Shared code SHALL gate all media/volume-control behavior on this flag, never on screen geometry (`height`, `is_round`) or a compile-time `#ifdef BOARD_*`.

#### Scenario: Only the P4 board reports the capability
- **WHEN** `board_caps().has_media_controls` is read on any board
- **THEN** it is `true` only when running on the P4 board (`waveshare_p4_touch_lcd_5`) and `false` on every other board, including boards that share the P4's "large" layout breakpoint (AMOLED-216, AMOLED-206)

### Requirement: BLE HID exposes a Consumer Control report on the P4 board
The system SHALL add a second HID input report (Report ID 2, usage page 0x0C "Consumer") to the P4 board's board-local `ble.cpp`, capable of sending Volume Increment, Volume Decrement, Mute, Play/Pause, and Scan Next Track usage codes. Shared `ble.cpp` (every other board) SHALL implement the same `ble.h` API as a no-op that sends no HID report.

#### Scenario: P4 host receives a real system volume key
- **WHEN** the P4 board sends a Volume Increment (or Decrement, or Mute) consumer-control report over its BLE HID connection
- **THEN** the paired host OS raises/lowers/mutes system volume exactly as it would for a physical media keyboard key, with no daemon or host-side application involvement

#### Scenario: P4 host receives a real media transport key
- **WHEN** the P4 board sends a Play/Pause or Scan Next Track consumer-control report
- **THEN** the paired host OS's active media session responds exactly as it would for a physical media keyboard key

#### Scenario: Non-P4 boards send no consumer-control report
- **WHEN** `ble_consumer_press()` or `ble_consumer_release()` is called on any board other than the P4
- **THEN** shared `ble.cpp`'s no-op implementation runs, no HID report is sent, and no existing BLE behavior (keyboard reports, data-service characteristics) on that board is affected

### Requirement: `ble.h` exposes a press/release API for consumer-control keys
The system SHALL provide `ble_consumer_press(ble_consumer_key_t key)` and `ble_consumer_release(void)` in the shared `ble.h` contract, declared unconditionally (no `#ifdef BOARD_*`) so every board's `ble.cpp` links against the same symbol, mirroring the existing `ble_keyboard_press`/`ble_keyboard_release` press/release model (one active key at a time, explicit release, no-op when not connected).

#### Scenario: Press then release produces a clean key-down/key-up pair on the P4
- **WHEN** `ble_consumer_press(BLE_CONSUMER_MUTE)` is called followed by `ble_consumer_release()` on the P4 board
- **THEN** the host OS observes a single mute key-down event followed by a key-up event, with no repeat or stuck-key behavior

#### Scenario: No-op when not connected
- **WHEN** `ble_consumer_press()` or `ble_consumer_release()` is called on the P4 board while BLE is not in the connected state
- **THEN** the call has no effect and does not crash or queue a report for later delivery

### Requirement: Usage screen shows a touch-driven media/volume control bar only when the flag is set
The system SHALL render five touch buttons (Volume Down, Mute, Volume Up, Play/Pause, Next Track) in a control bar below the existing Current/Weekly usage panels only when `board_caps().has_media_controls` is `true`. Each button SHALL call `ble_consumer_press()` on touch-down and `ble_consumer_release()` on touch-up or press-lost.

#### Scenario: Touching a control-bar button on the P4 sends the matching key
- **WHEN** the user touches the Volume Up button on the P4 board and lifts their finger
- **THEN** `ble_consumer_press(BLE_CONSUMER_VOLUME_UP)` fires on touch-down and `ble_consumer_release()` fires on touch-up

#### Scenario: Control bar fits without clipping the existing usage panels on the P4
- **WHEN** the usage screen renders on the P4 board
- **THEN** the Current/Weekly usage panels, the control bar, and the status line all render fully on-screen with no overlap or clipping, verified via `screenshot.sh` or an on-hardware photo

#### Scenario: Every other board keeps today's behavior unchanged
- **WHEN** the usage screen renders on any board other than the P4, including boards sharing the P4's layout breakpoint (AMOLED-216, AMOLED-206) or the round `waveshare_lcd_185b`
- **THEN** no control bar is rendered, `compute_layout()`'s existing constants for that board are unchanged, and the usage screen behaves exactly as it did before this change

### Requirement: Control-bar icons are sourced from Lucide, not hand-authored
The system SHALL source all five new control-bar icons from Lucide SVGs, converted through the existing `tools/png_to_lvgl.js` pipeline into `icons.h`, consistent with every existing icon in the project.

#### Scenario: New icons match the existing icon format
- **WHEN** the five new icons (`volume-x`, `volume-1`, `volume-2`, `play`, `skip-forward`) are added to `icons.h`
- **THEN** they use the same RGB565A8 white-tinted format as the existing battery icons, generated via `tools/png_to_lvgl.js`, not hand-drawn pixel data
