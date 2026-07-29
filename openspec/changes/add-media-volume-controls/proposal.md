## Why

The board already sits on the desk as an always-visible companion display next to the machine it's paired with — the same physical position a dedicated media/volume controller like [ESP-Streaming-Deck](../../../ESP-Streaming-Deck) occupies. Reusing that BLE HID link (already open for the Space/Shift+Tab keys) to also send system volume and playback keys turns idle screen space into something the user reaches for daily, at effectively zero extra hardware cost. This ships first, and only, on the P4 board (`waveshare_p4_touch_lcd_5`) — its 720×1280 panel is the only one with real unused vertical room below the usage panels; every other board is left untouched.

## What Changes

- Add a new `bool has_media_controls` field to `BoardCaps`, set `true` only in the P4 board's `caps.cpp` and explicitly `false` everywhere else. This flag — not screen geometry — is what gates both the BLE report and the UI control bar, so scope stays exactly one board even if a future board happens to share the P4's height breakpoint.
- Add BLE HID **Consumer Control** support (Report ID 2, usage page 0x0C) to the P4 board's board-local `ble.cpp` only, exposing volume up/down, mute, play/pause, and next-track as real OS-level media keys. Shared `ble.cpp` (used by every other board) gets a no-op stub of the same API so the shared `ble.h` contract still links everywhere.
- Add a `ble_consumer_press(key)` / `ble_consumer_release()` pair to the shared `ble.h` contract.
- Add a touch-driven control bar to the bottom of the usage screen (`ui.cpp`) with five buttons: **Vol−, Mute, Vol+, Play/Pause, Next** — rendered only when `board_caps().has_media_controls` is true. New Lucide-sourced icons converted through the existing `tools/png_to_lvgl.js` pipeline, added to `icons.h`.
- Trim the P4's usage-panel vertical footprint in `compute_layout()`'s existing "large" breakpoint just enough to open a bottom band for the control bar — scoped so it only takes effect when `has_media_controls` is set, leaving every other "large" board (AMOLED-216, AMOLED-206) with today's unchanged geometry.
- **Explicitly out of scope, by design, not just by screen size**: every board other than the P4, including the round `waveshare_lcd_185b` and every other rectangular board (AMOLED-216, AMOLED-206, AMOLED-18 S3/C6, LCD-1.54). All keep today's usage-screen-only behavior with zero visual or behavioral change.

## Capabilities

### New Capabilities
- `media-volume-controls`: BLE HID Consumer Control reporting plus the on-screen touch control bar that drives it, gated by a new `BoardCaps.has_media_controls` flag enabled only on the P4 board.

### Modified Capabilities
(none — no existing capability specs exist for this project yet; the usage-screen layout changes are implementation details of the new capability above, not a documented behavior change to an existing spec)

## Impact

- **Firmware, shared**: `hal/board_caps.h` (new field), every board's `caps.cpp` (explicit `.has_media_controls = false` except P4's `true`), `ble.h` (new API), `ble.cpp` (no-op stub only), `ui.cpp` (`compute_layout()`'s "large" breakpoint gains a conditional control-bar band, `init_usage_screen()` renders the bar only when the flag is set), `icons.h` (5 new icon arrays).
- **Firmware, board-local**: `boards/waveshare_p4_touch_lcd_5/ble.cpp` gets the real Consumer Control HID report and `caps.cpp` sets the new flag.
- **No daemon changes** — consumer-control keys are interpreted by the host OS directly over the same BLE HID link already used for Space/Shift+Tab; the daemon never sees them.
- **No changes to any other board's `ble.cpp`, `caps.cpp` geometry, or usage-screen rendering** beyond adding the one explicit `false` field.
