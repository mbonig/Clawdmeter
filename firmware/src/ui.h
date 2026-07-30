#pragma once
#include "data.h"
#include "ble.h"

// One screen only — the usage view, which picks between its own sub-views
// (pairing hint / idle / live numbers) from connection + data freshness.
void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
