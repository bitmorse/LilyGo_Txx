// Shared I2C master bus (SDA=21, SCL=22) used by both the IP5306 power chip and
// the MPU9250 IMU. Initialised once, on first use.
#pragma once

#include "driver/i2c_master.h"

// Returns the shared bus handle, or NULL if the bus could not be created.
i2c_master_bus_handle_t i2c_bus_get(void);

// Probe every 7-bit address (0x08..0x77), log each responder, and store up to
// `max` found addresses in `found`. Returns the number of devices found.
int i2c_bus_scan(uint8_t *found, int max);

// Diagnostic: try many candidate SDA/SCL pin pairs (using temporary buses),
// probing each for I2C devices. MUST run before any SPI/I2C peripheral claims
// those pins (i.e. before st7735_init). Writes a short human-readable summary
// into `report` and returns the total number of (pair,device) hits found.
int i2c_bus_sweep_pins(char *report, int report_sz);
