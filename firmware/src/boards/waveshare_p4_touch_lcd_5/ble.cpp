#include "../../ble.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <host/ble_gap.h>
#include <host/ble_store.h>
#include <Preferences.h>

// P4-specific reimplementation of shared ble.cpp's contract. h2zero/NimBLE-Arduino
// (which shared ble.cpp is written against) fails to build for ESP32-P4 outright
// — see board.h and openspec/changes/add-esp32-p4-core-board/design.md. This board
// instead uses arduino-esp32's own bundled `BLE` library, which links against the
// same precompiled NimBLE-over-ESP-Hosted stack already baked into the framework
// (confirmed empirically: CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=1 and libbt.a already
// ship with the installed framework-arduinoespressif32-libs package) — verified
// end-to-end on hardware: this board is discoverable via a real BLE scan.
//
// The bundled `BLE` library's API is an older generation than NimBLE-Arduino's:
// server/characteristic callbacks receive a raw `ble_gap_conn_desc*` instead of a
// rich `NimBLEConnInfo&` wrapper, HID setter names drop the `set` prefix
// (reportMap/manufacturer/pnp/hidInfo vs setReportMap/setManufacturer/...), and
// there's no C++ wrapper for bond enumeration/deletion at all — those go straight
// through NimBLE's own `ble_store_util_*` C functions. Notably, there's also no
// onAuthenticationComplete-equivalent callback; the single-owner-lock logic that
// used it purely for *early* claiming has been folded entirely into onWrite's
// existing encrypted-link check (already present in the original), which is the
// one place data security actually matters — a connection that never writes never
// needs an owner claimed. The Windows supervision-timeout pushback (see
// onConnParamsUpdate) is now armed directly from onConnect's own conn_desc fields
// instead of a separate onAuthenticationComplete hook, since ble_gap_conn_desc
// already carries the negotiated timeout at connect time — this covers the
// bonded-reconnect case (no later update event fires) the original relied on
// onAuthenticationComplete for, with one call site instead of two.

#define DEVICE_NAME "Clawdmeter"

#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"

#define BLE_BUF_SIZE 512
#define MAX_TRACKED_BONDS 8

static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xC0,
};

static BLEServer* server = nullptr;
static BLEHIDDevice* hid_dev = nullptr;
static BLECharacteristic* input_kbd = nullptr;
static BLECharacteristic* tx_char = nullptr;
static BLECharacteristic* rx_char = nullptr;
static BLECharacteristic* req_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;
static uint16_t current_conn_handle = 0xFFFF;

static const uint16_t CONN_HANDLE_NONE  = 0xFFFF;
static const uint16_t DESIRED_TIMEOUT   = 600;   // x10ms = 6s, matches PPCP
static volatile uint16_t param_fix_handle = CONN_HANDLE_NONE;
static volatile uint32_t param_fix_at_ms  = 0;
static volatile uint16_t param_fix_spent  = CONN_HANDLE_NONE;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static char mac_str[18];

// --- Single-owner lock (see ble.cpp for the full rationale) -----------------
static Preferences prefs;
static char owner_addr[18] = {0};
static bool owner_set = false;
static const char* ZERO_ADDR = "00:00:00:00:00:00";

static void save_owner() {
    prefs.begin("clawd", false);
    prefs.putString("owner", owner_addr);
    prefs.end();
}

static void clear_owner() {
    owner_set = false;
    owner_addr[0] = '\0';
    prefs.begin("clawd", false);
    prefs.remove("owner");
    prefs.end();
}

static void load_owner() {
    prefs.begin("clawd", true);
    String o = prefs.getString("owner", "");
    prefs.end();
    if (o.length() == 17) {
        strncpy(owner_addr, o.c_str(), sizeof(owner_addr) - 1);
        owner_addr[sizeof(owner_addr) - 1] = '\0';
        owner_set = true;
        Serial.printf("BLE: owner loaded = %s\n", owner_addr);
    }
}

// Delete every stored bond that isn't the owner. Bundled BLE library has no
// C++ wrapper for bond enumeration/deletion — goes straight through NimBLE's
// own store utility functions.
static void prune_foreign_bonds() {
    if (!owner_set) return;
    ble_addr_t addrs[MAX_TRACKED_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(addrs, &n, MAX_TRACKED_BONDS) != 0) return;
    for (int i = 0; i < n; i++) {
        String a = BLEAddress(addrs[i]).toString();
        if (strcmp(a.c_str(), owner_addr) != 0) {
            Serial.printf("BLE: pruning non-owner bond %s\n", a.c_str());
            ble_store_util_delete_peer(&addrs[i]);
        }
    }
}

static void claim_owner(const String& id) {
    strncpy(owner_addr, id.c_str(), sizeof(owner_addr) - 1);
    owner_addr[sizeof(owner_addr) - 1] = '\0';
    owner_set = true;
    save_owner();
    Serial.printf("BLE: owner claimed = %s\n", owner_addr);
    prune_foreign_bonds();
}

static void arm_conn_param_pushback(uint16_t conn_handle, uint16_t timeout) {
    if (timeout < DESIRED_TIMEOUT && conn_handle != param_fix_spent) {
        param_fix_handle = conn_handle;
        param_fix_at_ms  = millis() + 2000;
    }
}

static void start_advertising() {
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->reset();
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(BLEUUID((uint16_t)0x1812));  // BLE HID Service
    adv->setName(DEVICE_NAME);
    BLEAdvertisementData scanResp;
    scanResp.setCompleteServices(BLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    bool ok = adv->start();
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s, ble_gap_conn_desc* desc) override {
        state = BLE_STATE_CONNECTED;
        current_conn_handle = desc->conn_handle;
        String addr = BLEAddress(desc->peer_ota_addr).toString();
        Serial.printf("BLE: connected from %s (active=%u)\n",
            addr.c_str(), (unsigned)s->getConnectedCount());
        Serial.printf("BLE: connparams itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            desc->conn_itvl, desc->conn_itvl * 1.25f,
            desc->conn_latency, desc->supervision_timeout, desc->supervision_timeout * 10);
        // Bonded reconnects resume at the central's previously negotiated
        // parameters with no later onConnParamsUpdate event, so this must be
        // checked here too, not just there (see file header for why this
        // replaces the original's onAuthenticationComplete-based arming).
        arm_conn_param_pushback(desc->conn_handle, desc->supervision_timeout);
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(BLEServer* s, ble_gap_conn_desc* desc) override {
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        if (current_conn_handle == desc->conn_handle) current_conn_handle = CONN_HANDLE_NONE;
        if (param_fix_handle == desc->conn_handle) param_fix_handle = CONN_HANDLE_NONE;
        if (param_fix_spent  == desc->conn_handle) param_fix_spent  = CONN_HANDLE_NONE;
        Serial.printf("BLE: disconnected (remaining=%u)\n", (unsigned)s->getConnectedCount());
    }

    void onConnParamsUpdate(uint16_t conn_handle, uint16_t interval, uint16_t latency, uint16_t timeout, uint8_t status) override {
        Serial.printf("BLE: connparams update itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            interval, interval * 1.25f, latency, timeout, timeout * 10);
        arm_conn_param_pushback(conn_handle, timeout);
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* chr, ble_gap_conn_desc* desc) override {
        // Only accept usage data over an encrypted link, and only from the
        // owner machine — see ble.cpp for the full rationale. The bundled BLE
        // library has no onAuthenticationComplete hook, so owner-claiming
        // that used to happen there now happens exclusively here, on the
        // first encrypted write — functionally equivalent, since a
        // connection that never writes never needed an owner claimed anyway.
        String id = BLEAddress(desc->peer_id_addr).toString();
        if (!desc->sec_state.encrypted) {
            Serial.println("BLE: dropping RX write from unencrypted link");
            return;
        }
        if (!owner_set && id != ZERO_ADDR) {
            claim_owner(id);
        }
        if (owner_set && strcmp(id.c_str(), owner_addr) != 0) {
            Serial.printf("BLE: dropping RX write from non-owner %s\n", id.c_str());
            return;
        }
        String val = chr->getValue();
        size_t len = std::min((size_t)val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
};

class ReqCallbacks : public BLECharacteristicCallbacks {
    void onSubscribe(BLECharacteristic* chr, ble_gap_conn_desc* desc, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

void ble_init(void) {
    BLEDevice::init(DEVICE_NAME);
    BLESecurity::setAuthenticationMode(true, false, true);  // bonding, no MITM, SC

    load_owner();
    prune_foreign_bonds();

    BLEAddress addr = BLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = BLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- HID keyboard service ---
    hid_dev = new BLEHIDDevice(server);
    hid_dev->reportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    // manufacturer() (no-arg) lazily creates m_manufacturerCharacteristic —
    // unlike reportMap/pnp/hidInfo/setBatteryLevel, whose backing
    // characteristics are all pre-created in BLEHIDDevice's own constructor.
    // Calling manufacturer(name) without this first is a null-pointer
    // dereference into setValue() — confirmed on hardware (Guru Meditation
    // Load access fault, bisected via per-call ESP_LOGE markers since Serial
    // output is unreliable on this board's native USB CDC).
    hid_dev->manufacturer();
    hid_dev->manufacturer("Anthropic");
    // BLEHIDDevice::pnp() packs each 16-bit field big-endian
    // (high-byte-first) — confirmed by reading the implementation directly
    // — but the Bluetooth SIG's PnP ID characteristic (0x2A50) spec requires
    // little-endian. Confirmed on hardware via `system_profiler
    // SPBluetoothDataType`: macOS showed Vendor ID 0xE502 / Product ID
    // 0x0100 for values passed here as 0x02E5 / 0x0001 — exactly the
    // byte-swapped result of this bug. Pre-swapping each field here so the
    // buggy packer's output ends up correct.
    hid_dev->pnp(0x01, 0xE502, 0x0100, 0x0001);
    hid_dev->hidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->inputReport(1);
    hid_dev->startServices();

    // --- Custom data service ---
    BLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
    if (param_fix_handle != CONN_HANDLE_NONE &&
        (int32_t)(millis() - param_fix_at_ms) >= 0) {
        uint16_t h = param_fix_handle;
        param_fix_handle = CONN_HANDLE_NONE;
        param_fix_spent  = h;
        if (server && server->getConnectedCount() > 0) {
            Serial.println("BLE: requesting 6s supervision timeout");
            server->updateConnParams(h, 12, 24, 0, DESIRED_TIMEOUT);
        }
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    ble_store_clear();
    clear_owner();
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED && current_conn_handle != CONN_HANDLE_NONE) {
        server->disconnect(current_conn_handle);
    }
    need_advertise = true;
}

bool ble_has_bonds(void) {
    ble_addr_t addrs[1];
    int n = 0;
    ble_store_util_bonded_peers(addrs, &n, 1);
    return n > 0;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_set_battery_level(int pct) {
    if (!hid_dev || pct < 0) return;
    if (pct > 100) pct = 100;
    hid_dev->setBatteryLevel((uint8_t)pct);
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}
