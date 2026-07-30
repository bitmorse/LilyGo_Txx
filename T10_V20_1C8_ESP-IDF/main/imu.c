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
#define REG_ACCEL_CFG2  0x1D   // accel DLPF / FCHOICE (bit3 = A_FCHOICE_B)
#define REG_FIFO_EN     0x23
#define REG_INT_STATUS  0x3A   // bit4 = FIFO_OFLOW_INT
#define REG_INT_PIN_CFG 0x37
#define REG_ACCEL_XOUT  0x3B   // 14 bytes: accel(6) temp(2) gyro(6)
#define REG_TEMP_OUT    0x41   // 8 bytes: temp(2) gyro(6)
#define REG_USER_CTRL   0x6A   // bit6 FIFO_EN, bit2 FIFO_RST
#define REG_PWR_MGMT_1  0x6B
#define REG_PWR_MGMT_2  0x6C   // per-axis stby: bits5-3 accel, bits2-0 gyro
#define REG_FIFO_COUNTH 0x72   // 2 bytes, big-endian, valid entries
#define REG_FIFO_R_W    0x74   // FIFO read/write port
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
static i2c_master_dev_handle_t s_mpu_fast = NULL;  // 1 MHz handle for FIFO bursts
static uint8_t s_addr = 0;      // address the IMU answered on (0 = none)
static uint8_t s_whoami = 0;    // WHO_AM_I value read (for diagnostics)
static float   s_hires_lsb_per_g = 2048.0f;        // set by imu_hires_start()

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

static i2c_master_dev_handle_t add_dev_speed(i2c_master_bus_handle_t bus,
                                             uint8_t addr, uint32_t hz)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &h) != ESP_OK) return NULL;
    return h;
}

static i2c_master_dev_handle_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr)
{
    return add_dev_speed(bus, addr, 400000);
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

// --- high-rate accelerometer FIFO mode (industrial vibration logging) --------
//
// Reconfigures the accelerometer for its maximum 4 kHz output data rate (DLPF
// bypassed, ~1 kHz bandwidth) at +/-16 g and streams samples through the on-chip
// 512-byte FIFO. A dedicated 1 MHz I2C handle drains the FIFO fast enough to keep
// up with the 24 KB/s the accelerometer produces. Gyro/temp/mag are powered down.
//
// This is a *different* configuration than imu_read()'s 200 Hz mode; call
// imu_hires_stop() (or re-run imu_init()) to return to the live-Sensors mode.

float imu_hires_lsb_per_g(void) { return s_hires_lsb_per_g; }

bool imu_hires_start(void)
{
    if (s_mpu == NULL) return false;

    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (s_mpu_fast == NULL && bus)
        s_mpu_fast = add_dev_speed(bus, s_addr, 1000000);  // fast-mode-plus
    // If the 1 MHz handle can't be created, fall back to the 400 kHz handle;
    // the achieved rate will be lower (see docs) but logging still works.
    i2c_master_dev_handle_t dev = s_mpu_fast ? s_mpu_fast : s_mpu;

    wr(dev, REG_PWR_MGMT_1, 0x01);      // wake, best available clock
    vTaskDelay(pdMS_TO_TICKS(5));
    wr(dev, REG_PWR_MGMT_2, 0x00);      // accel + gyro on (gyro read on aux chan)
    wr(dev, REG_ACCEL_CFG,  0x18);      // ACCEL_FS_SEL = 3 -> +/-16 g
    wr(dev, REG_ACCEL_CFG2, 0x08);      // A_FCHOICE_B=1: DLPF off -> 4 kHz ODR
    s_hires_lsb_per_g = 2048.0f;        // +/-16 g full-scale

    // Reset then enable the FIFO, routing only the accelerometer into it.
    wr(dev, REG_USER_CTRL, 0x04);       // FIFO_RST
    vTaskDelay(pdMS_TO_TICKS(2));
    wr(dev, REG_FIFO_EN,   0x08);       // ACCEL -> FIFO
    wr(dev, REG_USER_CTRL, 0x40);       // FIFO_EN
    ESP_LOGI(TAG, "hi-rate FIFO started (+/-16 g, 4 kHz, %s)",
             s_mpu_fast ? "I2C 1 MHz" : "I2C 400 kHz fallback");
    return true;
}

// Drain the FIFO into `dst` (int16 x,y,z triples). `max_samples` is the capacity
// of dst in *samples* (dst must hold max_samples*3 int16). Returns the number of
// samples read. If the FIFO overflowed since the last call, *overflow is set true
// and the FIFO is reset (the gap in the stream is the caller's to account for).
int imu_hires_read(int16_t *dst, int max_samples, bool *overflow)
{
    i2c_master_dev_handle_t dev = s_mpu_fast ? s_mpu_fast : s_mpu;
    if (dev == NULL || dst == NULL || max_samples <= 0) return 0;

    if (overflow) *overflow = false;

    uint8_t st = 0;
    if (rd(dev, REG_INT_STATUS, &st, 1) == ESP_OK && (st & 0x10)) {
        // FIFO overflowed: samples were lost and 6-byte framing may be broken.
        // Reset it and report the gap rather than logging misaligned garbage.
        wr(dev, REG_USER_CTRL, 0x04);   // FIFO_RST
        wr(dev, REG_USER_CTRL, 0x40);   // FIFO_EN
        if (overflow) *overflow = true;
        return 0;
    }

    uint8_t cnt[2];
    if (rd(dev, REG_FIFO_COUNTH, cnt, 2) != ESP_OK) return 0;
    int bytes = (cnt[0] << 8) | cnt[1];
    int samples = bytes / 6;                        // 6 bytes per accel sample
    if (samples <= 0) return 0;
    if (samples > max_samples) samples = max_samples;

    // Burst-read in bounded chunks (I2C transfer + our stack buffer).
    uint8_t buf[60];                                // 10 samples per burst
    int done = 0;
    while (done < samples) {
        int chunk = samples - done;
        if (chunk > 10) chunk = 10;
        if (rd(dev, REG_FIFO_R_W, buf, chunk * 6) != ESP_OK) break;
        for (int i = 0; i < chunk; i++) {
            const uint8_t *p = &buf[i * 6];
            int16_t *o = &dst[(done + i) * 3];
            o[0] = be16(&p[0]);
            o[1] = be16(&p[2]);
            o[2] = be16(&p[4]);
        }
        done += chunk;
    }
    return done;
}

// Read the slow auxiliary channels once (gyro deg/s, mag uT, temp degC) via
// direct register reads -- NOT the FIFO (the FIFO carries accel only). Safe to
// call concurrently with imu_hires_read: the shared I2C bus serializes access.
// Any output pointer may be NULL. Returns false only if no IMU is present.
bool imu_hires_read_aux(float *gx, float *gy, float *gz,
                        float *mx, float *my, float *mz, float *temp_c)
{
    i2c_master_dev_handle_t dev = s_mpu_fast ? s_mpu_fast : s_mpu;
    if (dev == NULL) return false;

    uint8_t b[8];                            // TEMP(2) + GYRO(6), contiguous
    if (rd(dev, REG_TEMP_OUT, b, sizeof(b)) != ESP_OK) return false;
    if (temp_c) *temp_c = be16(&b[0]) / 333.87f + 21.0f;
    if (gx) *gx = be16(&b[2]) / GYR_LSB_PER_DPS;   // gyro FS +/-250 dps (default)
    if (gy) *gy = be16(&b[4]) / GYR_LSB_PER_DPS;
    if (gz) *gz = be16(&b[6]) / GYR_LSB_PER_DPS;

    // Magnetometer (AK8963), if present. 16-bit mode: 0.15 uT / LSB.
    float lx = 0, ly = 0, lz = 0;
    if (s_mag) {
        uint8_t m[7];                        // HXL..HZH + ST2 (ST2 read unlatches)
        if (rd(s_mag, MAG_HXL, m, sizeof(m)) == ESP_OK && !(m[6] & 0x08)) {
            lx = le16(&m[0]) * 0.15f;        // AK8963 is little-endian
            ly = le16(&m[2]) * 0.15f;
            lz = le16(&m[4]) * 0.15f;
        }
    }
    if (mx) *mx = lx;
    if (my) *my = ly;
    if (mz) *mz = lz;
    return true;
}

void imu_hires_stop(void)
{
    i2c_master_dev_handle_t dev = s_mpu_fast ? s_mpu_fast : s_mpu;
    if (dev == NULL) return;
    wr(dev, REG_FIFO_EN,   0x00);       // stop feeding the FIFO
    wr(dev, REG_USER_CTRL, 0x04);       // FIFO_RST (flush)
    // Restore the gentle live-Sensors configuration (200 Hz, +/-2 g, gyro on).
    wr(dev, REG_PWR_MGMT_2, 0x00);
    wr(dev, REG_ACCEL_CFG2, 0x03);      // DLPF ~41 Hz
    wr(dev, REG_ACCEL_CFG,  0x00);      // +/-2 g
    ESP_LOGI(TAG, "hi-rate FIFO stopped");
}
