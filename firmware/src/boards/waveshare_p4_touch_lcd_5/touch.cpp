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
//   reg 0x8150: point 1 record (8 bytes) — [track_id, xH, xL, yH, yL, sL, sH, rsvd]
//   (high byte before low byte here — confirmed empirically on hardware;
//   this unit's coordinate bytes are the opposite order from some published
//   GT911 register maps, which list xL before xH.)

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
    bool ok = gt911_read(0x8150, rec, sizeof(rec));
    gt911_write16(0x814E, 0x00);  // tell the controller we've consumed this sample
    if (!ok) return false;
    *out_x = ((uint16_t)rec[1] << 8) | rec[2];
    *out_y = ((uint16_t)rec[3] << 8) | rec[4];
    return true;
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *pressed = false;
    if (!gt911_addr) return;

    uint16_t x1, y1;
    if (!gt911_read_point(&x1, &y1)) return;

    // Second, independent sample taken immediately after the first — reject
    // if they disagree by more than a small tolerance. This board's GT911 has
    // no RST/INT pins wired, so touch_hal_read() polls raw registers with no
    // controller-side debouncing; single-sample reads have been observed on
    // real hardware to occasionally report wildly wrong coordinates (300+px
    // off for a barely-moved finger) — most likely a torn I2C read or a
    // stale/wrong multi-touch point slot, not real finger motion, since two
    // genuine back-to-back samples of the same physical touch should closely
    // agree. A dropped cycle here just means the next poll (milliseconds
    // later) tries again — no functional loss for a real, held touch.
    uint16_t x2, y2;
    if (!gt911_read_point(&x2, &y2)) {
        // No second sample available this instant — accept the first alone
        // rather than dropping a legitimate touch outright.
        *x = x1;
        *y = y1;
        *pressed = true;
        return;
    }

    const uint16_t TOLERANCE = 40;
    uint16_t dx = x1 > x2 ? x1 - x2 : x2 - x1;
    uint16_t dy = y1 > y2 ? y1 - y2 : y2 - y1;
    if (dx <= TOLERANCE && dy <= TOLERANCE) {
        *x = x2;
        *y = y2;
        *pressed = true;
    }
    // else: the two samples disagree too much — treat as noise, drop this cycle.
}
