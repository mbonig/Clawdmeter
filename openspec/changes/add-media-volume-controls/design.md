## Context

The board already advertises a BLE HID service (`ble.cpp` / board-local `boards/waveshare_p4_touch_lcd_5/ble.cpp`) with a single Report ID 1 keyboard collection, used today only for Space (voice-mode PTT) and Shift+Tab (mode toggle) from the physical buttons. `../ESP-Streaming-Deck` (a sibling project, read for reference, not imported as a dependency) proves the pattern this change follows: a second HID collection under the Consumer Control usage page (0x0C), sent as its own Report ID, drives real OS-level volume/media keys — no daemon or host-side software involved, since the OS's HID stack interprets the usage codes itself.

This change is scoped to the P4 board (`waveshare_p4_touch_lcd_5`) only, by explicit product decision — not because of a technical limitation on other boards. Two independent BLE stacks implement `ble.h` today (shared `ble.cpp` via NimBLE-Arduino for every S3/C6 board, and `boards/waveshare_p4_touch_lcd_5/ble.cpp` via arduino-esp32's bundled `BLE` library for the P4 — see `CLAUDE.md`'s P4 section for why these can't be unified). Only the P4's board-local implementation needs the real Consumer Control report; shared `ble.cpp` only needs enough of the API surface to keep linking.

The usage screen (`ui.cpp`) currently has no concept of a persistent touch button — the only touch handling today is `global_click_cb` (any tap toggles splash/idle) and the splash's tap-to-advance. This change introduces the project's first real touch *buttons* with dedicated hit targets and press/release semantics, but only renders them on the one board that opts in.

## Goals / Non-Goals

**Goals:**
- Volume Up, Volume Down, Mute, Play/Pause, and Next Track as real BLE HID consumer-control keys, driven by five on-screen touch buttons, on the P4 board only.
- A `BoardCaps.has_media_controls` flag as the single source of truth for whether a board gets this feature — not screen size, not `is_round`, not a breakpoint — so scope is explicit and future boards default to "off" until deliberately opted in.
- Fits below the existing Current/Weekly usage panels on the P4's 720×1280 panel without clipping.
- No daemon changes.
- Zero behavioral or visual change on every board other than the P4.

**Non-Goals:**
- Every other board (round `waveshare_lcd_185b`, AMOLED-216/206, AMOLED-18 S3/C6, LCD-1.54) — explicitly out of scope for this change, gated off by the new flag defaulting to `false`. A future change can flip the flag on for another board once its layout is worked out; this change does not need to anticipate that geometry.
- Reflecting real playback/volume *state* back onto the icons (e.g. swapping Play↔Pause glyph, or a volume level readout) — these are one-way HID key presses, matching ESP-Streaming-Deck's own model; the host OS has no channel back to the board to report state.
- Press-and-hold repeat for Volume Up/Down (tap-to-step only in this change) — flagged as a natural follow-up, not required for v1.

## Decisions

### 1. Gate by a new `BoardCaps` flag, not by geometry
Add `bool has_media_controls` to `BoardCaps` (`hal/board_caps.h`), set `.has_media_controls = true` only in `boards/waveshare_p4_touch_lcd_5/caps.cpp`, and add an explicit `.has_media_controls = false` to every other board's `caps.cpp`. `ui.cpp` and `main.cpp` branch on `board_caps().has_media_controls`, never on `height`/`is_round` — matches the project's existing convention ("optional features are guarded by `BoardCaps` (runtime) ... rather than `#ifdef BOARD_*`") and rule #10 in `CLAUDE.md`.

**Alternative considered**: infer eligibility from existing geometry (`height >= 460 && !is_round`), which was the original (pre-feedback) design. Rejected on explicit user direction — geometry would silently enable this on AMOLED-216/206 too, and a flag is one line per board versus a geometry heuristic that has to be re-reasoned about every time a new board's dimensions happen to match.

### 2. Consumer Control HID report shape (P4 board-local `ble.cpp` only)
Add a second collection to the P4's HID report map, Report ID 2, following the widely-used minimal consumer-control descriptor (same shape ESP-Streaming-Deck and most Arduino/ESP32 BLE media-key examples use):

```
0x05, 0x0C,        // Usage Page (Consumer)
0x09, 0x01,        // Usage (Consumer Control)
0xA1, 0x01,        // Collection (Application)
0x85, 0x02,        //   Report ID (2)
0x15, 0x00,        //   Logical Minimum (0)
0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
0x19, 0x00,        //   Usage Minimum (0)
0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
0x75, 0x10,        //   Report Size (16)
0x95, 0x01,        //   Report Count (1)
0x81, 0x00,        //   Input (Data, Array, Absolute)
0xC0               // End Collection
```
A single 16-bit "selector" value per report: send the usage code to press, `0x0000` to release. Usage codes needed: Volume Increment `0x00E9`, Volume Decrement `0x00EA`, Mute `0x00E2`, Play/Pause `0x00CD`, Scan Next Track `0x00B5` — all comfortably inside the 0–0x3FF logical range.

**Alternative considered**: a bitmap report (one bit per usage, several usages assertable at once). Rejected — nothing in this feature needs simultaneous multi-key presses, and the array form is simpler and matches the reference project.

### 3. `ble.h` API shape, and the shared-`ble.cpp` stub
```c
enum ble_consumer_key_t {
    BLE_CONSUMER_VOLUME_UP,
    BLE_CONSUMER_VOLUME_DOWN,
    BLE_CONSUMER_MUTE,
    BLE_CONSUMER_PLAY_PAUSE,
    BLE_CONSUMER_NEXT_TRACK,
};
void ble_consumer_press(ble_consumer_key_t key);
void ble_consumer_release(void);
```
Declared unconditionally in `ble.h` (every board links against some `ble.cpp`, and `ui.cpp` is shared code with no `#ifdef BOARD_*`, so the symbol must exist everywhere). The P4's board-local `ble.cpp` implements it for real, mirroring `ble_keyboard_press`/`ble_keyboard_release`'s existing connection-state guard and cached-characteristic pattern. Shared `ble.cpp` (every non-P4 board) implements both functions as a deliberate no-op — no report map change, no new characteristic — since `has_media_controls` is `false` everywhere it's linked, these are dead code paths that exist purely to satisfy the link.

**Why not `#ifdef` the declarations out of `ble.h` for non-P4 boards instead?** Shared `ui.cpp` would then need a `#ifdef BOARD_WAVESHARE_P4...` around the button-wiring code, which is exactly the pattern rule #10 forbids. A no-op stub keeps every board's `ble.cpp` satisfying the same unconditional contract, and the `has_media_controls` flag (not conditional compilation) is what actually decides whether the dead code path is ever reached.

### 4. Touch button model in `ui.cpp`
On-screen buttons use `LV_EVENT_PRESSED` → `ble_consumer_press(key)` and `LV_EVENT_RELEASED`/`LV_EVENT_PRESS_LOST` → `ble_consumer_release()` (not `LV_EVENT_CLICKED`), so a press reads as a real, releasable key the same way the physical buttons already work in `main.cpp` — this also leaves room for the future press-and-hold-repeat enhancement without an API change later. Each button is a plain `lv_obj_t` (icon image child) rather than `lv_btn_create()`, matching the project's existing preference for hand-styled `lv_obj_t` panels over LVGL's built-in widgets (see `make_usage_panel`). The whole bar is created and shown only `if (board_caps().has_media_controls)`.

### 5. Layout: P4's "large" breakpoint only
`compute_layout()`'s existing `height >= 460` branch (shared by AMOLED-216, AMOLED-206, and P4) gains a conditional sub-block: `if (board_caps().has_media_controls) { /* trim usage_panel_h, set control_bar_h/geometry */ }`, applied only inside that guard so AMOLED-216/206 (which also hit this branch, but have the flag `false`) see no change at all to their existing constants. Exact pixel values are tuned during implementation against a real P4 screenshot (`screenshot.sh`), following the project's existing "inherit the closer breakpoint, polish on hardware" convention. The P4's 720×1280 panel has hundreds of pixels of unused room below the panels already (per the P4 board's tasks.md from the original port: "no clipping" at the existing breakpoint's font sizes), so this is a straightforward addition of a new band, not a tight squeeze.

### 6. Icon sourcing
Five new Lucide-sourced icons, converted through the existing `tools/png_to_lvgl.js` pipeline (white-tinted RGB565A8, same as the battery icons) and spliced into `icons.h`: `volume-x` (mute), `volume-1` (volume down), `volume-2` (volume up), `play` (static play/pause glyph — no real playback-state feedback to justify toggling it, consistent with the Non-Goals above), `skip-forward` (next track). No hand-authored art, matching every existing icon in this project. These are added to the shared `icons.h` even though only one board renders them — consistent with how every other shared icon (including board-specific-in-practice ones) already lives there, not behind a board-specific header.

### 7. Button arrangement
Single row of five icon buttons, evenly spaced across `content_w`, positioned in the new bottom band: `[Vol−] [Mute] [Vol+] [Play/Pause] [Next]`.

## Risks / Trade-offs

- **[Risk]** A no-op stub in shared `ble.cpp` is easy to forget to update if a future change ever wants to enable this on another NimBLE-based board → **Mitigation**: the stub's implementation comment explicitly says "real implementation lives in the P4 board's board-local ble.cpp; mirror that HID report map here if enabling this flag on another NimBLE board," so the next person (or session) doing that work is pointed at the reference implementation instead of starting from scratch.
- **[Risk]** Per-board pixel tuning is inherently iterative and screenshot-dependent, same as every prior layout change in this project → **Mitigation**: tune against real P4 hardware/`screenshot.sh` before considering the change done, same QA bar as every existing UI change.
- **[Trade-off]** Static Play/Pause icon (no state reflection) may read as ambiguous to users expecting a toggle indicator → accepted, consistent with the reference project's own one-way model and this project's Non-Goals.
- **[Risk, discovered during implementation]** The P4's GT911 touch driver has real coordinate-reliability problems this feature was the first to expose — a top-of-screen calibration tap landed pixel-perfect, but deliberate near-bottom taps landed 150-300+ px short on different occasions, and one test found a genuinely non-monotonic result (tapping further right produced a *lower* X reading), most likely GT911 multi-touch point-slot confusion rather than a scale/axis bug (the controller's own configured resolution registers read exactly 720×1280, matching the panel, ruling out a scale mismatch). → **Mitigation shipped in this change**: `touch_hal_read()` now takes two independent samples and rejects results that disagree by more than 40px (filters single-sample torn/corrupt reads, but not a consistently-wrong-but-self-agreeing read), and control-bar buttons are inset 100px from each screen edge and enlarged to 88px to stay inside the empirically-reliable window. **Not mitigated**: the underlying point-slot-confusion hypothesis is untested — by explicit user decision (2026-07-24), full root-cause (disambiguating GT911's multiple point slots, or diffing against Waveshare's reference driver) is scoped out of this change and tracked as a separate follow-up (tasks.md section 9).

## Open Questions

- Exact trimmed panel height for the P4's control-bar band — placeholder values in tasks.md, finalized against a real screenshot during implementation.
- Whether Volume Up/Down should eventually get press-and-hold repeat — explicitly deferred, not blocking this change.
- Whether the touch-coordinate issue (see Risks above) is really GT911 point-slot confusion or something else — needs its own dedicated debugging pass, not resolved here.
- Whether a future change should extend `has_media_controls` to other boards — explicitly not decided here; this change only wires the flag for P4 and leaves every other board's flag `false`.
