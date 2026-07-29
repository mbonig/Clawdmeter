#include "../../hal/power_hal.h"
#include "board.h"

// No PMU and no fuel gauge disclosed on this board (an MX1.25 3.7V battery
// connector exists, but Waveshare's docs don't name a battery-monitoring
// chip). Rather than guess at an unconfirmed ADC pin, this is a deliberate
// "no battery" stub — matches BoardCaps.has_battery = false in caps.cpp.
// No PWR button either (see board.h): only BOOT + a hardware RST line.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void)  { return -1; }
bool power_hal_is_charging(void)  { return false; }
bool power_hal_is_vbus_in(void)   { return false; }

bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
