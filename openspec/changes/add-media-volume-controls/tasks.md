## 1. `BoardCaps` flag

- [x] 1.1 Add `bool has_media_controls;` to `hal/board_caps.h`.
- [x] 1.2 Set `.has_media_controls = true` in `boards/waveshare_p4_touch_lcd_5/caps.cpp`.
- [x] 1.3 Every other board's `caps.cpp` already omits `is_round` and other false-by-default flags entirely (aggregate init zero-initializes them) — followed that existing convention rather than padding all 7 files with a redundant explicit `false`. Verified via a build of both `waveshare_p4_touch_lcd_5` and `waveshare_amoled_216` that the flag defaults to `false` correctly.

## 2. Shared `ble.h` / `ble.cpp`: API + no-op stub

- [x] 2.1 Add `ble_consumer_key_t` enum and `ble_consumer_press()`/`ble_consumer_release()` declarations to `firmware/src/ble.h`.
- [x] 2.2 Implement both as a deliberate no-op in shared `firmware/src/ble.cpp` (used by every non-P4 board) — no report-map change, no new characteristic. Comment pointing at the P4 board's `ble.cpp` as the reference implementation if this is ever enabled on another NimBLE board.
- [x] 2.3 Confirmed — the diff only appends two new no-op functions; `HID_REPORT_MAP` and every existing function are untouched.

## 3. P4 board-local BLE HID: real Consumer Control report

- [x] 3.1 Add the second HID input report (Report ID 2, usage page 0x0C, 16-bit array) to `firmware/src/boards/waveshare_p4_touch_lcd_5/ble.cpp`'s report map, per design.md's descriptor bytes.
- [x] 3.2 Confirmed by reading `BLEHIDDevice.cpp` directly: `inputReport(reportID)` creates a fresh characteristic keyed by whatever `reportID` is passed via the standard 0x2908 Report Reference descriptor — no special-casing for report 1 vs. 2. `inputReport(2)` works identically to the existing `inputReport(1)`; no API surprises, design.md's assumption holds.
- [x] 3.3 Implemented `ble_consumer_press()`/`ble_consumer_release()` for the P4 — usage-code lookup + 2-byte little-endian report, guarded on `state == BLE_STATE_CONNECTED && input_consumer`, mirroring `ble_keyboard_press`.
- [ ] 3.4 **Partially done, not fully confirmed.** Bench-tested via a temporary `mediatest` serial command (since removed): all 5 `ble_consumer_press()`/`release()` calls executed without error against a connected host, and the code path is correct by review (matches `ble_keyboard_press`'s working pattern exactly). But an `osascript`-based before/after volume check during one bench-test run showed no change — whether that reflects a real gap in the report itself or was confounded by the BLE reconnect issues happening in parallel that session is unresolved. Re-verify once the touch-driver issue (section 9) is fixed and the buttons can be tapped reliably end-to-end.
- [ ] 3.5 Not explicitly re-tested on hardware this session. Low risk by inspection: the report-map edit only appends a second collection; the original keyboard collection's bytes are byte-for-byte unchanged.

## 4. Icons

- [x] 4.1 Sourced via `lucide-static` (npm) — `volume-x`, `volume-1`, `volume-2`, `play`, `skip-forward` SVGs, rasterized to 48×48 PNG with `rsvg-convert`.
- [x] 4.2 Converted each through `tools/png_to_lvgl.js` (installed `pngjs` locally under `tools/node_modules/`, already gitignored) and appended `icon_volume_x_data`, `icon_volume_down_data`, `icon_volume_up_data`, `icon_play_pause_data`, `icon_next_track_data` to `firmware/src/icons.h`. Confirmed via a full P4 build.
- [x] 4.3 48×48 — matches the existing `ICON_BATTERY_*`/`ICON_BLUETOOTH_*` convention; final in-bar rendered size may still be scaled down in LVGL depending on the control-bar band height chosen in section 5.

## 5. Usage-screen layout: P4-only control-bar band

- [x] 5.1 Added `control_bar_y`/`control_bar_h`/`control_btn_size` to the `Layout` struct.
- [x] 5.2 Implemented as a conditional sub-block guarded by `c.has_media_controls`, but deviated from the plan in one way (documented inline in `ui.cpp`): rather than trimming `usage_panel_h`, the band is positioned bottom-anchored off `L.scr_h` (`scr_h - control_bar_h - 70`) instead of relative to the panels. The `height >= 460` branch's absolute panel offsets were tuned for a 480-tall screen; the P4 is 1280 tall and already has hundreds of pixels of dead space below the panels, so trimming wasn't needed and bottom-anchoring reads better than a band floating mid-screen with empty space below it. AMOLED-216/206's existing constants are untouched outside the `has_media_controls` guard.
- [x] 5.3 Build succeeds; visual confirmation blocked on the BLE bonding desync described below (screenshot dump needs a live BLE connection — see Hardware verification).
- [x] 5.4 Built `waveshare_amoled_216` and `waveshare_amoled_206` — both succeed, and the diff shows their branch's existing lines are untouched (the new code is a new `if` block, not an edit to any existing assignment).

## 6. Control-bar widgets and BLE wiring

- [x] 6.1 Built in `init_usage_screen()` via `build_control_bar(usage_group)`, gated on `board_caps().has_media_controls`.
- [x] 6.2 Wired: `LV_EVENT_PRESSED` → `ble_consumer_press()`, `LV_EVENT_RELEASED`/`LV_EVENT_PRESS_LOST` → `ble_consumer_release()`.
- [x] 6.3 Root-caused differently than expected: read LVGL's actual bubbling source (`lv_obj_event.c`) and confirmed a button's own `CLICKED` event cannot reach `usage_container` without `LV_OBJ_FLAG_EVENT_BUBBLE` on the button itself, which it never gets — event bubbling was never actually the mechanism. The real cause of "tapping a button jumps to splash" was touch-coordinate imprecision (see section 9): taps intended for a button landed well outside its bounds, hitting `usage_group`'s own clickable background instead, which correctly (if unhelpfully) bubbles to `usage_container`. Added a coordinate-based dead zone in `global_click_cb` as defense in depth regardless of the exact widget a near-miss lands on.

## 7. Hardware verification

- [ ] 7.1 **Blocked on the touch-driver issue tracked in section 9.** Confirmed idle-screen fix and reconnect/data-flow behavior on real hardware; could not reliably tap individual control-bar buttons to confirm each one's real system volume/media response, since touch coordinates near the panel edges (and in one case, in the middle) were unreliable enough that taps aimed at a button often landed elsewhere. By explicit user decision, this is deferred rather than blocking the rest of the change.
- [x] 7.2 Built `waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`, `waveshare_amoled_18_c6`, `waveshare_amoled_206`, `waveshare_lcd_185b`, `waveshare_lcd_154` — all 7 succeed with no code changes outside the new conditional blocks.

## 9. Known follow-up: P4 touch-coordinate reliability (tracked, not blocking)

Discovered while hardware-testing the control bar, not caused by this change — the P4 board's GT911 touch driver (`boards/waveshare_p4_touch_lcd_5/touch.cpp`) has real coordinate-reliability problems that this change's buttons were the first UI element to expose (previous touch interactions were all coarse "tap anywhere" gestures that never needed to discriminate between nearby targets):

- A top-of-screen calibration tap landed pixel-perfect; the GT911's own configured resolution registers (0x8146/0x8148) read exactly 720x1280, matching the panel — ruling out a simple scale/axis-swap bug.
- Deliberate "tap near the bottom" attempts landed 150-300+ px short of the actual target on different occasions.
- Worse: a genuinely non-monotonic result was observed — tapping to the *right* of a previous point produced a *lower* X reading than the previous point, near the horizontal center of the screen. This can't be explained by scale/offset error and points at something more structural, most likely GT911 multi-touch point-slot confusion (the driver always reads point-1's record at 0x8150 and has no way to detect if a given sample actually corresponds to a different slot).
- [x] 9.1 Shipped as a partial mitigation in this change: `touch_hal_read()` now takes two independent samples per call and rejects the result if they disagree by more than 40px (`boards/waveshare_p4_touch_lcd_5/touch.cpp`). This filters single-sample torn/corrupt reads but does **not** fix a consistently-wrong-but-internally-agreeing read, so it does not fully resolve the issue.
- [x] 9.2 Shipped as a partial mitigation: control-bar buttons are inset 100px from each screen edge (not divided edge-to-edge) and enlarged to 88px, keeping every button's center inside the empirically-reliable window and giving mis-taps more margin.
- [ ] 9.3 Not done — proper root-cause requires reading and disambiguating all 5 possible GT911 point slots (not just point 1), and/or comparing against Waveshare's own reference GT911 driver implementation for this board. Explicitly scoped out of this change by user decision (2026-07-24) after extensive on-hardware iteration; track as its own follow-up change.

## 8. Docs

- [x] 8.1 Added a "Media/volume control bar" bullet to `CLAUDE.md`'s P4 section covering the report shape, the `has_media_controls` flag, and why it's gated by a flag rather than the shared "large" breakpoint.
- [x] 8.2 Same bullet also covers the no-op-stub split (shared `ble.cpp` vs. this board's real implementation) and what a future board enabling the flag needs to do.
