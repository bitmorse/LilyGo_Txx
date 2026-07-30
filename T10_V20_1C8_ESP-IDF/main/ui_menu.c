#include "ui_menu.h"
#include "lvgl_port.h"
#include "st7735.h"
#include "wifi_scan.h"
#include "imu.h"

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

// Persisted-ish settings (in RAM for the demo).
static bool s_wifi_on = false;
static int  s_mode = 0;               // 0=Auto 1=Day 2=Night

static lv_obj_t *s_info;              // shared status/output label

static void set_info(const char *txt)
{
    if (s_info) lv_label_set_text(s_info, txt);
}

// --- widget event handlers --------------------------------------------------

static void wifi_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_wifi_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    set_info(s_wifi_on ? "WiFi: ON" : "WiFi: OFF");
}

static void backlight_cb(lv_event_t *e)
{
    lv_obj_t *sb = lv_event_get_target(e);
    int v = lv_spinbox_get_value(sb);
    st7735_set_brightness(v);          // live brightness
}

static void mode_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_mode = lv_dropdown_get_selected(dd);
}

static void wifi_scan_cb(lv_event_t *e)
{
    (void)e;
    set_info("Scanning...");
    lv_refr_now(NULL);                 // paint the message before the blocking scan

    ap_info_t aps[6];
    int n = wifi_scan_run(aps, 6);

    char buf[256];
    int off = snprintf(buf, sizeof(buf), "Found %d:\n", n);
    for (int i = 0; i < n && off < (int)sizeof(buf) - 24; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s %ddBm\n",
                        aps[i].ssid, aps[i].rssi);
    }
    set_info(buf);
}

static void board_info_cb(lv_event_t *e)
{
    (void)e;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "ESP32 rev%d %dc\nFlash %uMB\nHeap %uKB",
             chip.revision, chip.cores, (unsigned)(flash >> 20),
             (unsigned)(esp_get_free_heap_size() / 1024));
    set_info(buf);
}

static void sensors_cb(lv_event_t *e)
{
    (void)e;
    imu_data_t d;
    if (!imu_read(&d)) { set_info("IMU: not found"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "A %d %d %d mg\nG %d %d %d dps\nT %d C",
             (int)(d.ax * 1000), (int)(d.ay * 1000), (int)(d.az * 1000),
             (int)d.gx, (int)d.gy, (int)d.gz, (int)d.temp_c);
    set_info(buf);
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    set_info("Rebooting...");
    lv_refr_now(NULL);
    esp_restart();
}

// --- builders ---------------------------------------------------------------

static lv_obj_t *make_row(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    return row;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(lvgl_port_group(), btn);
    return btn;
}

void ui_menu_start(void)
{
    lv_group_t *g = lvgl_port_group();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xE6EAEE), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 4, 0);
    lv_obj_set_style_pad_row(scr, 4, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0x37C8B4), 0);

    // WiFi switch
    lv_obj_t *row = make_row(scr, "WiFi");
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_add_event_cb(sw, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(g, sw);

    // Backlight spinbox (0..100, live)
    row = make_row(scr, "Light");
    lv_obj_t *sb = lv_spinbox_create(row);
    lv_spinbox_set_range(sb, 0, 100);
    lv_spinbox_set_digit_format(sb, 3, 0);
    lv_spinbox_set_step(sb, 5);
    lv_spinbox_set_value(sb, 100);
    lv_obj_set_width(sb, 54);
    lv_obj_add_event_cb(sb, backlight_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(g, sb);

    // Mode dropdown
    row = make_row(scr, "Mode");
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, "Auto\nDay\nNight");
    lv_obj_set_width(dd, 70);
    lv_obj_add_event_cb(dd, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(g, dd);

    // Actions
    make_button(scr, "WiFi scan", wifi_scan_cb);
    make_button(scr, "Sensors", sensors_cb);
    make_button(scr, "Board info", board_info_cb);
    make_button(scr, "Reboot", reboot_cb);

    // Shared output label
    s_info = lv_label_create(scr);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info, lv_pct(100));
    lv_label_set_text(s_info, "BTN1/2: move\nBTN3: select");
    lv_obj_set_style_text_color(s_info, lv_color_hex(0x8A93A0), 0);
}
