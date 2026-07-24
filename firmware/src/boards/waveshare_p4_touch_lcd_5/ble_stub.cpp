#include "../../ble.h"

// Permanent no-op stand-in for shared ble.cpp on this board only. NimBLE
// doesn't build for ESP32-P4 today (see board.h for the full writeup); this
// board's PlatformIO env excludes ble.cpp and substitutes this file so
// main.cpp's unconditional ble_* calls still link. Swap this back out for
// the real ble.cpp once the upstream blocker clears — no shared-code changes
// needed either way, since main.cpp only ever calls through ble.h.

void ble_init(void) {}
void ble_tick(void) {}
ble_state_t ble_get_state(void) { return BLE_STATE_DISCONNECTED; }
const char* ble_get_device_name(void) { return "BLE unavailable"; }
const char* ble_get_mac_address(void) { return "--:--:--:--:--:--"; }
void ble_clear_bonds(void) {}
bool ble_has_bonds(void) { return false; }
bool ble_has_data(void) { return false; }
const char* ble_get_data(void) { return ""; }
void ble_send_ack(void) {}
void ble_send_nack(void) {}
void ble_request_refresh(void) {}

void ble_set_battery_level(int) {}

void ble_keyboard_press(uint8_t, uint8_t) {}
void ble_keyboard_release(void) {}
