#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal inline reader for the GT911 (16-bit register addressing) — same
// "vendor a small reader instead of an external dependency" posture this
// project already uses for FT3168/CST816 touch controllers elsewhere. GT911
// was considered for vendoring as an official ESP-IDF component (like the
// HX8394 display driver), but it also needs the separate `esp_lcd_touch`
// base component, so hand-rolling stays smaller and dependency-free.
//
// This board has no TOUCH_RST/TOUCH_INT pins wired (both GPIO_NUM_NC in
// Waveshare's own BSP), so there's no reset to pulse and no interrupt to
// latch on — touch_hal_read() just polls the status register directly each
// call. A single ~9-byte I2C transaction at 400kHz is well under the HAL
// contract's 5ms budget.
//
//   reg 0x814E: bit7 = data-ready, bits[3:0] = touch point count (0-5)
//   reg 0x814F: point 1 record (8 bytes) — [track_id, xL, xH, yL, yH, sL, sH, rsvd]
//   Little-endian coordinate pairs, record starting at 0x814F. Verified against
//   Espressif's official esp_lcd_touch_gt911 component. An earlier version of this
//   file read the record from 0x8150 and treated the pairs as big-endian, which
//   silently mixed bytes from adjacent fields — see gt911_read_point() for the full
//   story and why several "coordinate accuracy" workarounds were removed with it.

static uint8_t gt911_addr = 0;

static bool gt911_write16(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool gt911_read(uint16_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(gt911_addr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool gt911_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void touch_hal_init(void) {
    if (gt911_probe(GT911_ADDR_PRIMARY)) {
        gt911_addr = GT911_ADDR_PRIMARY;
    } else if (gt911_probe(GT911_ADDR_BACKUP)) {
        gt911_addr = GT911_ADDR_BACKUP;
    } else {
        Serial.println("Touch GT911 not found on either address");
        return;
    }
    Serial.printf("Touch GT911 found at 0x%02X\n", gt911_addr);
}

// Reads one point-1 record. Returns false if no fresh sample was available
// (caller should NOT treat that as "not pressed" — it means "couldn't sample
// right now", distinct from a real released touch).
//
// Register layout, verified against Espressif's official esp_lcd_touch_gt911
// component (read from the sibling ESP-Streaming-Deck project's managed_components):
//
//   0x814E        status: bit7 = data ready, bits[3:0] = point count
//   0x814F        point 1 track id
//   0x8150/0x8151 point 1 X low / X high   (little-endian)
//   0x8152/0x8153 point 1 Y low / Y high   (little-endian)
//   0x8154/0x8155 point 1 size low / high
//
// This reader previously started the record read at 0x8150 and assumed *that* was the
// track id, i.e. every field was shifted one byte, and it also treated the pairs as
// big-endian. The result was that it combined bytes from adjacent fields:
//   x = (X_high << 8) | Y_low          y = (Y_high << 8) | size_low
// which is the true cause of the "mangled touch grid" chased for two sessions — the
// compressed and genuinely non-monotonic X readings, and a Y that moved by only
// ~10-15px across large finger movements (it was dominated by Y_high<<8, changing
// only every 256px, plus the touch-size byte as noise). It also explains the earlier
// "Y saturates near 1080" reading: y = (3 << 8) | size ≈ 768..1023 for anything in
// the lower half of the panel.
//
// Everything built on top of that misreading has been removed: the empirical 5/4 X
// scale factor (it was fitting garbage), and the "reject any report that isn't exactly
// 1 point" + double-sample-agreement filters (both were attempts to suppress the
// symptoms). Match the official driver instead.
static bool gt911_read_point(uint16_t* out_x, uint16_t* out_y) {
    uint8_t status = 0;
    if (!gt911_read(0x814E, &status, 1)) return false;
    if (!(status & 0x80)) return false;  // no fresh data

    uint8_t points = status & 0x0F;
    if (points == 0 || points > 5) {
        gt911_write16(0x814E, 0x00);  // clear and bail
        return false;
    }

    uint8_t rec[8];
    bool ok = gt911_read(0x814F, rec, sizeof(rec));
    gt911_write16(0x814E, 0x00);  // tell the controller we've consumed this sample
    if (!ok) return false;

    // rec[0] = track id, rec[1..2] = X (LE), rec[3..4] = Y (LE)
    *out_x = (uint16_t)rec[1] | ((uint16_t)rec[2] << 8);
    *out_y = (uint16_t)rec[3] | ((uint16_t)rec[4] << 8);
    return true;
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *pressed = false;
    if (!gt911_addr) return;

    // Single read is enough now that the register offset/byte order is correct. The
    // previous double-sample "agreement" filter existed to suppress wildly wrong
    // coordinates that were actually caused by the off-by-one field misread (see
    // gt911_read_point above), and in practice its comparison almost never ran anyway:
    // the first read clears the 0x814E status bit, so the immediate second read found
    // no fresh data and fell through to a single-sample accept.
    uint16_t rx, ry;
    if (!gt911_read_point(&rx, &ry)) return;

    *x = rx;
    *y = ry;
    *pressed = true;
}
