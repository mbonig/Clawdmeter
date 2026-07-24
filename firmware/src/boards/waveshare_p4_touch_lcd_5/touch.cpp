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

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *pressed = false;
    if (!gt911_addr) return;

    uint8_t status = 0;
    if (!gt911_read(0x814E, &status, 1)) return;
    if (!(status & 0x80)) return;  // no fresh data

    uint8_t points = status & 0x0F;
    if (points == 0 || points > 5) {
        gt911_write16(0x814E, 0x00);  // clear and bail
        return;
    }

    uint8_t rec[8];
    if (gt911_read(0x8150, rec, sizeof(rec))) {
        *x = ((uint16_t)rec[1] << 8) | rec[2];
        *y = ((uint16_t)rec[3] << 8) | rec[4];
        *pressed = true;
    }
    gt911_write16(0x814E, 0x00);  // tell the controller we've consumed this sample
}
