#include "imu.h"
#include "i2c_bus.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "imu";

// --- MPU9250 registers ---
#define MPU_ADDR        0x68
#define REG_SMPLRT_DIV  0x19
#define REG_CONFIG      0x1A
#define REG_GYRO_CFG    0x1B
#define REG_ACCEL_CFG   0x1C
#define REG_INT_PIN_CFG 0x37
#define REG_ACCEL_XOUT  0x3B   // 14 bytes: accel(6) temp(2) gyro(6)
#define REG_PWR_MGMT_1  0x6B
#define REG_WHO_AM_I    0x75
#define MPU_WHOAMI      0x71

// --- AK8963 magnetometer (accessed via I2C bypass) ---
#define MAG_ADDR        0x0C
#define MAG_WIA         0x00   // -> 0x48
#define MAG_ST1         0x02
#define MAG_HXL         0x03   // 6 data bytes + ST2 at 0x09
#define MAG_CNTL1       0x0A
#define MAG_WHOAMI      0x48

// Scale factors for the default ranges (+/-2g, +/-250 dps).
#define ACC_LSB_PER_G   16384.0f
#define GYR_LSB_PER_DPS 131.0f

static i2c_master_dev_handle_t s_mpu = NULL;
static i2c_master_dev_handle_t s_mag = NULL;
static uint8_t s_addr = 0;      // address the IMU answered on (0 = none)
static uint8_t s_whoami = 0;    // WHO_AM_I value read (for diagnostics)

uint8_t imu_last_addr(void)   { return s_addr; }
uint8_t imu_last_whoami(void) { return s_whoami; }

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, 2, 100);
}

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(d, &reg, 1, buf, n, 100);
}

static i2c_master_dev_handle_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &h) != ESP_OK) return NULL;
    return h;
}

bool imu_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL) return false;

    // Log everything on the bus first (helps if the IMU is elsewhere).
    i2c_bus_scan(NULL, 0);

    // The IMU may sit at 0x68 or 0x69 (AD0 pin). Try both.
    static const uint8_t addrs[] = { 0x68, 0x69 };
    uint8_t who = 0;
    bool found = false;
    for (unsigned i = 0; i < sizeof(addrs); i++) {
        s_mpu = add_dev(bus, addrs[i]);
        if (s_mpu == NULL) continue;
        if (rd(s_mpu, REG_WHO_AM_I, &who, 1) == ESP_OK) {
            ESP_LOGI(TAG, "IMU @0x%02X WHO_AM_I=0x%02X", addrs[i], who);
            s_addr = addrs[i];
            s_whoami = who;
            // Accept MPU9250 (0x71) and close relatives that share the
            // accel/gyro register map: MPU9255 0x73, MPU6500 0x70, MPU6050 0x68.
            if (who == 0x71 || who == 0x73 || who == 0x70 || who == 0x68) {
                found = true;
                break;
            }
        }
        i2c_master_bus_rm_device(s_mpu);
        s_mpu = NULL;
    }
    if (!found) {
        ESP_LOGW(TAG, "no MPU-compatible IMU (last who=0x%02X @0x%02X)", who, s_addr);
        return false;
    }

    wr(s_mpu, REG_PWR_MGMT_1, 0x80);        // reset
    vTaskDelay(pdMS_TO_TICKS(100));
    wr(s_mpu, REG_PWR_MGMT_1, 0x01);        // wake, auto clock source
    vTaskDelay(pdMS_TO_TICKS(10));
    wr(s_mpu, REG_CONFIG, 0x03);            // DLPF ~41 Hz
    wr(s_mpu, REG_SMPLRT_DIV, 0x04);        // 200 Hz sample rate
    wr(s_mpu, REG_GYRO_CFG, 0x00);          // +/-250 dps
    wr(s_mpu, REG_ACCEL_CFG, 0x00);         // +/-2 g
    wr(s_mpu, REG_INT_PIN_CFG, 0x02);       // BYPASS_EN: expose AK8963 on the bus
    vTaskDelay(pdMS_TO_TICKS(10));

    // Optional magnetometer.
    s_mag = add_dev(bus, MAG_ADDR);
    if (s_mag) {
        uint8_t wia = 0;
        if (rd(s_mag, MAG_WIA, &wia, 1) == ESP_OK && wia == MAG_WHOAMI) {
            wr(s_mag, MAG_CNTL1, 0x00);     // power down
            vTaskDelay(pdMS_TO_TICKS(10));
            wr(s_mag, MAG_CNTL1, 0x16);     // 16-bit, continuous mode 2 (100 Hz)
            vTaskDelay(pdMS_TO_TICKS(10));
            ESP_LOGI(TAG, "AK8963 magnetometer online");
        } else {
            i2c_master_bus_rm_device(s_mag);
            s_mag = NULL;
            ESP_LOGW(TAG, "AK8963 WIA=0x%02X (expected 0x48)", wia);
        }
    }

    ESP_LOGI(TAG, "MPU9250 online%s", s_mag ? " + magnetometer" : "");
    return true;
}

static inline int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static inline int16_t le16(const uint8_t *p) { return (int16_t)((p[1] << 8) | p[0]); }

bool imu_read(imu_data_t *out)
{
    if (s_mpu == NULL || out == NULL) return false;

    uint8_t b[14];
    if (rd(s_mpu, REG_ACCEL_XOUT, b, sizeof(b)) != ESP_OK) return false;

    int16_t ax = be16(&b[0]),  ay = be16(&b[2]),  az = be16(&b[4]);
    int16_t t  = be16(&b[6]);
    int16_t gx = be16(&b[8]),  gy = be16(&b[10]), gz = be16(&b[12]);

    out->ax = ax / ACC_LSB_PER_G;
    out->ay = ay / ACC_LSB_PER_G;
    out->az = az / ACC_LSB_PER_G;
    out->gx = gx / GYR_LSB_PER_DPS;
    out->gy = gy / GYR_LSB_PER_DPS;
    out->gz = gz / GYR_LSB_PER_DPS;
    out->temp_c = t / 333.87f + 21.0f;      // per MPU9250 datasheet

    out->mag_ok = false;
    out->mx = out->my = out->mz = 0;
    if (s_mag) {
        uint8_t m[7];                        // HXL..HZH + ST2 (must read ST2)
        if (rd(s_mag, MAG_HXL, m, sizeof(m)) == ESP_OK && !(m[6] & 0x08)) {
            out->mx = le16(&m[0]);           // AK8963 is little-endian
            out->my = le16(&m[2]);
            out->mz = le16(&m[4]);
            out->mag_ok = true;
        }
    }

    out->ok = true;
    return true;
}
