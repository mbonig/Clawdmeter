#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// No IO expander and no companion-chip bring-up needed here: the onboard
// ESP32-C6-MINI (Wi-Fi 6 / BLE 5 co-processor) is not brought up by this
// port at all — see board.h and ble_stub.cpp for why BLE isn't wired up yet.
// The HX8394 panel's reset is driven by esp_lcd_panel_reset() from inside
// display_hal_begin(), not pulsed here.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
}
