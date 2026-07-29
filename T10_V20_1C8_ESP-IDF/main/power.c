#include "power.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "power";

// IP5306 on the shared I2C bus (SDA=21, SCL=22).
#define I2C_SDA          21
#define I2C_SCL          22
#define IP5306_ADDR      0x75
#define IP5306_SYS_CTL0  0x00
#define IP5306_KEEP_ON   0x37   // bit1 set = boost keep-on

void power_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed; skipping IP5306 setup");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IP5306_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "IP5306 not addressable; skipping");
        return;
    }

    uint8_t cmd[2] = { IP5306_SYS_CTL0, IP5306_KEEP_ON };
    if (i2c_master_transmit(dev, cmd, sizeof(cmd), 100) == ESP_OK) {
        ESP_LOGI(TAG, "IP5306 boost keep-on enabled");
    } else {
        ESP_LOGW(TAG, "IP5306 write failed (fine if USB-powered)");
    }
}
