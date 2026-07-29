// Minimal MPU9250 9-axis IMU driver (accel + gyro + temp + AK8963 magnetometer)
// over the shared I2C bus. Enough to read and display live values.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    ok;          // MPU9250 responded and data is valid
    float   ax, ay, az;  // acceleration, g
    float   gx, gy, gz;  // angular rate, deg/s
    float   temp_c;      // die temperature, deg C
    bool    mag_ok;      // magnetometer present and read this sample
    int16_t mx, my, mz;  // magnetometer, raw counts
} imu_data_t;

// Probe and configure the MPU9250. Returns true if detected (WHO_AM_I == 0x71).
bool imu_init(void);

// Read one sample. Returns false if the IMU isn't present / read failed.
bool imu_read(imu_data_t *out);

// Diagnostics from the last imu_init() probe.
uint8_t imu_last_addr(void);    // I2C address the IMU answered on (0 = none)
uint8_t imu_last_whoami(void);  // WHO_AM_I value read
