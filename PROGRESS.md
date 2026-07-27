# P4 BLE + touch debugging — RESOLVED (2026-07-27)

Everything this file was tracking is fixed and verified on hardware: usage data,
BLE HID keyboard (physical BOOT button types Space), and all five media/volume
buttons driving real OS-level actions.

The durable knowledge has been folded into the permanent docs — read those, not
this file:

- **`CLAUDE.md`**, P4 board section — the GT911 register layout, the BLE HID
  Secure-Connections + OS-pairing requirement, the vendored/patched BLE library,
  and the control-bar parenting/z-order notes.
- **`openspec/changes/add-media-volume-controls/tasks.md`** — section 9 has the
  full touch root-cause writeup; `design.md`'s Risks section records what the
  original hypotheses got wrong.

## The three actual root causes (all firmware/library, none of them the host)

1. **Report 2 was never in the GATT database.** arduino-esp32's bundled BLE
   library registers only one characteristic per UUID per service on its NimBLE
   path, and every HID report uses UUID `0x2A4D` — so the second
   `inputReport()` returned a live object with handle `0xFFFF` that no host could
   see. Fixed by vendoring the library to `firmware/lib/BLE/` and patching
   `BLEService::addCharacteristic()`.
2. **macOS never attached HID.** It requires LE Secure Connections *and* pairing
   through the OS's own Bluetooth flow. A CoreBluetooth app-level connect bonds
   for GATT only and never enrols HID (no "Minor Type: Keyboard").
3. **Touch coordinates were corrupt.** `touch.cpp` read the point record from
   `0x8150` instead of `0x814F` and treated little-endian pairs as big-endian,
   combining bytes from adjacent fields.

## Process lessons worth keeping

- **Verify your test harness before trusting its results.** A long series of
  "volume didn't change" measurements were meaningless: the `mediatest` serial
  command never reached the device, because serial *input* doesn't work on this
  board (the same reason `clearbonds` and `screenshot` fail). Confirmed only
  after adding a device-side counter showing the handler was never entered.
- **Instrument the device instead of guessing from the host.** Nearly all real
  progress came from putting state on-screen — CCCD subscribe flags, GATT
  handles, press-time coordinates. The `0xFFFF` handle and the `sub c=-1` reading
  each collapsed a whole class of theories immediately.
- **Diff against the vendor/official driver early.** The GT911 answer was sitting
  in the sibling `../ESP-Streaming-Deck` project's `managed_components/` the
  entire time. Two sessions of empirical corrections (scale factors, sample
  filters, dead zones, "bigger buttons") were all fitting garbage data.
- **Beware stacking workarounds on an unverified diagnosis.** Every mitigation
  built on the "flaky hardware" theory had to be removed once the real bug was
  found; each had also made the code harder to reason about in the meantime.
- Operationally: `blueutil --unpair` doesn't work for BLE (use System Settings →
  Forget), `sudo` needs a real Terminal (not this session's shell), and holding a
  connection open with a Python/`bleak` script is a trap — it *looks* like it
  fixes the daemon's discovery but produces a GATT-only bond that breaks HID.

This file can be deleted; it's kept only as a short record of how the debugging
went. Nothing here is load-bearing.
