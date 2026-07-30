#include "i2c_bus.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

// This board wires I2C to GPIO19 (SDA) / GPIO18 (SCL) -- the MPU9250 responds at
// 0x68 there (confirmed by the boot sweep). The repo's header claims 21/22.
#define I2C_SDA 19
#define I2C_SCL 18

static const char *TAG = "i2c";
static i2c_master_bus_handle_t s_bus = NULL;
static bool s_tried = false;

i2c_master_bus_handle_t i2c_bus_get(void)
{
    if (s_tried) return s_bus;
    s_tried = true;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        s_bus = NULL;
    }
    return s_bus;
}

int i2c_bus_scan(uint8_t *found, int max)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL) return 0;

    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "device found @ 0x%02X", addr);
            if (found && n < max) found[n] = addr;
            n++;
        }
    }
    if (n == 0) ESP_LOGW(TAG, "no I2C devices responded on SDA21/SCL22");
    return n;
}

int i2c_bus_sweep_pins(char *report, int report_sz)
{
    // Candidate (SDA, SCL) pairs to try. Excludes: input-only pins (34-39, can't
    // drive I2C), flash pins (6-11), and pins already used by on-board
    // peripherals we don't want to disturb -- TFT (5/16/17/23/27), SD
    // (2/13/14/15), speaker (25). Probing those left them mis-configured and
    // corrupted the display, so they're intentionally omitted here.
    static const uint8_t pairs[][2] = {
        {21, 22}, {22, 21},
        {19, 18}, {18, 19},
        {32, 33}, {33, 32},
        { 0,  4}, { 4,  0},
        {26, 12}, {12, 26},
    };
    const int npairs = sizeof(pairs) / sizeof(pairs[0]);

    int off = 0;
    if (report && report_sz) report[0] = '\0';
    int total = 0;

    for (int i = 0; i < npairs; i++) {
        uint8_t sda = pairs[i][0], scl = pairs[i][1];
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus = NULL;
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK || bus == NULL) {
            continue;   // pins unusable for I2C
        }
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (i2c_master_probe(bus, addr, 30) == ESP_OK) {
                ESP_LOGW(TAG, "HIT: SDA%d SCL%d -> 0x%02X", sda, scl, addr);
                total++;
                if (report && off < report_sz - 1) {
                    off += snprintf(report + off, report_sz - off,
                                    "SDA%d SCL%d:%02X\n", sda, scl, addr);
                }
            }
        }
        i2c_del_master_bus(bus);
    }

    if (total == 0) {
        ESP_LOGW(TAG, "sweep: no I2C devices on any candidate pins");
        if (report && report_sz) snprintf(report, report_sz, "no devices\non any pins");
    }
    return total;
}
