#pragma once

// Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (SKU 33762) — 5" 720x1280 portrait
// MIPI-DSI panel, ESP32-P4 SoC (no native radio; an onboard ESP32-C6-MINI
// provides Wi-Fi 6 / BLE 5 over SDIO via ESP-Hosted).
//
// First non-QSPI/SPI board in this project: the panel is MIPI-DSI, driven
// directly against ESP-IDF's esp_lcd_mipi_dsi/esp_lcd_panel_ops APIs (called
// from Arduino framework code — confirmed empirically to link and flash) via
// a vendored HX8394 driver (see hx8394/), not GFX Library for Arduino (which
// has no DSI panel class at all).
//
// BLE is NOT wired up on this board. h2zero/NimBLE-Arduino fails to build
// for ESP32-P4 today (confirmed on real hardware — a compile-time bug in its
// own FreeRTOS porting layer, and its maintainer confirms this is a known,
// unresolved limitation, not a config issue: h2zero/NimBLE-Arduino#906). The
// ESP-IDF-native fallback (esp-nimble-cpp + esp_hosted + esp_wifi_remote)
// also fails today under PlatformIO in either framework (h2zero/esp-nimble-cpp#377),
// traced here to PlatformIO's `framework = arduino` build never invoking the
// idf_component.yml component manager that BLE-on-P4 needs. See
// openspec/changes/add-esp32-p4-core-board/design.md for the full writeup.
// This board's PlatformIO env excludes shared ble.cpp and substitutes
// ble_stub.cpp (implements the same ble.h contract as a permanent no-op)
// until the upstream blocker clears.

#define BOARD_NAME           "Waveshare ESP32-P4-WIFI6-Touch-LCD-5"

// ---- Display geometry (portrait) ----
#define LCD_WIDTH            720
#define LCD_HEIGHT           1280

// ---- MIPI-DSI display (HX8394 via esp_lcd_mipi_dsi, vendored driver) ----
#define LCD_RESET_GPIO       27
#define LCD_BACKLIGHT_GPIO   26
#define LCD_MIPI_DSI_LANE_NUM          2
#define LCD_MIPI_DSI_LANE_BITRATE_MBPS 700   // HX8394_PANEL_BUS_DSI_2CH_CONFIG default
// ESP32-P4's MIPI DPHY needs a dedicated 2.5V rail from the chip's internal
// adjustable LDO. Channel 3 / 2500mV is the standard ESP32-P4 hardware
// constant used across Espressif's own DSI examples and Waveshare's BSP —
// not a board-specific choice.
#define MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define LCD_BACKLIGHT_LEDC_CHANNEL      1
#define LCD_BACKLIGHT_LEDC_TIMER        1
#define LCD_BACKLIGHT_LEDC_FREQ_HZ      5000
#define LCD_BACKLIGHT_LEDC_RES_BITS     10   // 0..1023 duty range

// ---- I2C bus (touch only on this board) ----
#define IIC_SDA              7
#define IIC_SCL              8

// ---- Touch (GT911 via hand-rolled I2C reader — no RST/INT pins wired) ----
#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_BACKUP    0x14

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// No secondary button and no PWR button on this board: the only other
// control is a hardware RST line (not a readable GPIO), same posture as
// waveshare_lcd_185b. Screen-cycling / hold-to-pair / splash-cycling
// gestures that depend on a second button are unavailable here.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0   // no IMU
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0   // no PMU/fuel-gauge disclosed on this board
#define BOARD_HAS_IO_EXPANDER      0
