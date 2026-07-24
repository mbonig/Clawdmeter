#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);

// BLE HID consumer control (media/volume keys). Real implementation lives in
// boards/waveshare_p4_touch_lcd_5/ble.cpp — every other board's ble.cpp is a
// deliberate no-op (see that file), gated off by BoardCaps.has_media_controls
// rather than this API itself.
enum ble_consumer_key_t {
    BLE_CONSUMER_VOLUME_UP,
    BLE_CONSUMER_VOLUME_DOWN,
    BLE_CONSUMER_MUTE,
    BLE_CONSUMER_PLAY_PAUSE,
    BLE_CONSUMER_NEXT_TRACK,
};
void ble_consumer_press(ble_consumer_key_t key);
void ble_consumer_release(void);
