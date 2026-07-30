#include "ui_menu.h"
#include "lvgl_port.h"
#include "st7735.h"
#include "wifi_scan.h"
#include "imu.h"
#include "provisioning.h"
#include "airport.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

// Settings state (RAM for the demo).
static bool s_wifi_on = false;
static int  s_mode = 0;               // 0=Auto 1=Day 2=Night

// Main menu screen + its focusable widgets, so we can restore the encoder group
// when returning from a sub-page.
#define MAX_FOCUS 12
static lv_obj_t *s_main_scr;
static lv_obj_t *s_focus[MAX_FOCUS];
static int       s_focus_n;
static lv_obj_t *s_page;              // current sub-page, or NULL

static void group_add_main(lv_obj_t *o)
{
    lv_group_add_obj(lvgl_port_group(), o);
    if (s_focus_n < MAX_FOCUS) s_focus[s_focus_n++] = o;
}

// --- sub-page (info screen with a Back button) ------------------------------

static void back_cb(lv_event_t *e)
{
    (void)e;
    lv_group_t *g = lvgl_port_group();
    lv_group_remove_all_objs(g);
    for (int i = 0; i < s_focus_n; i++) lv_group_add_obj(g, s_focus[i]);
    if (s_focus_n) lv_group_focus_obj(s_focus[0]);

    lv_screen_load(s_main_scr);
    if (s_page) { lv_obj_delete_async(s_page); s_page = NULL; }
}

static lv_obj_t *page_button(lv_obj_t *page, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, text);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(lvgl_port_group(), b);
    return b;
}

// Fresh sub-page: clears the group, creates a styled screen with a title.
static lv_obj_t *page_shell(const char *title)
{
    lv_group_remove_all_objs(lvgl_port_group());

    lv_obj_t *page = lv_obj_create(NULL);
    s_page = page;
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0xE6EAEE), 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, 4, 0);
    lv_obj_set_style_pad_row(page, 4, 0);

    lv_obj_t *t = lv_label_create(page);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0x37C8B4), 0);
    return page;
}

static lv_obj_t *page_text(lv_obj_t *page, const char *text)
{
    lv_obj_t *c = lv_label_create(page);
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c, lv_pct(100));
    lv_label_set_text(c, text);
    return c;
}

// Simple info sub-page: title, wrapped text, optional action button, then Back.
static void open_page_ex(const char *title, const char *text,
                         const char *action_label, lv_event_cb_t action_cb)
{
    lv_obj_t *page = page_shell(title);
    page_text(page, text);
    if (action_label && action_cb) page_button(page, action_label, action_cb);
    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(back);
}

static void open_page(const char *title, const char *text)
{
    open_page_ex(title, text, NULL, NULL);
}

// --- widget / action handlers -----------------------------------------------

static void wifi_switch_cb(lv_event_t *e)
{
    s_wifi_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void backlight_cb(lv_event_t *e)
{
    st7735_set_brightness(lv_spinbox_get_value(lv_event_get_target(e)));
}

static void mode_cb(lv_event_t *e)
{
    s_mode = lv_dropdown_get_selected(lv_event_get_target(e));
}

static void wifi_scan_cb(lv_event_t *e)
{
    (void)e;
    ap_info_t aps[8];
    int n = wifi_scan_run(aps, 8);
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "%d networks:\n", n);
    for (int i = 0; i < n && off < (int)sizeof(buf) - 28; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s  %ddBm\n",
                        aps[i].ssid, aps[i].rssi);
    }
    if (n == 0) snprintf(buf, sizeof(buf), "No networks found");
    open_page("WiFi scan", buf);
}

static void sensors_cb(lv_event_t *e)
{
    (void)e;
    imu_data_t d;
    char buf[160];
    if (!imu_read(&d)) {
        snprintf(buf, sizeof(buf), "MPU9250 not detected");
    } else {
        snprintf(buf, sizeof(buf),
                 "Accel (mg)\n %d  %d  %d\nGyro (dps)\n %d  %d  %d\nTemp %d C",
                 (int)(d.ax * 1000), (int)(d.ay * 1000), (int)(d.az * 1000),
                 (int)d.gx, (int)d.gy, (int)d.gz, (int)d.temp_c);
    }
    open_page("Sensors", buf);
}

static void board_info_cb(lv_event_t *e)
{
    (void)e;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "ESP32 rev%d\n%d cores\nFlash %uMB\nFree heap %uKB",
             chip.revision, chip.cores, (unsigned)(flash >> 20),
             (unsigned)(esp_get_free_heap_size() / 1024));
    open_page("Board info", buf);
}

static void repair_cb(lv_event_t *e)
{
    (void)e;
    provisioning_reset_and_restart();   // erases creds + reboots into pairing
}

static void wifi_setup_cb(lv_event_t *e)
{
    (void)e;
    prov_state_t st = provisioning_state();
    lv_obj_t *page = page_shell("WiFi Setup");

    if (st == PROV_PAIRING || st == PROV_IDLE) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "App: ESP BLE\nProvisioning\n\nDevice: %s\nPoP: %s",
                 provisioning_service_name(), provisioning_pop());
        page_text(page, buf);
    } else {
        const char *msg = (st == PROV_CONNECTED)  ? "WiFi connected.\n\nUse Forget WiFi to\nchange network."
                        : (st == PROV_CONNECTING) ? "Connecting..."
                        :                           "Not connected.\n\nUse Forget WiFi to\nre-pair.";
        page_text(page, msg);
    }

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(back);
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);   // keep the QR visible at the top
}

// --- ZRH airport traffic: hourly bars, Today vs Usual ----------------------

static int                s_air_gen;        // invalidated when leaving the page
static lv_obj_t          *s_air_status;
static lv_obj_t          *s_chart_today,  *s_chart_usual;
static lv_chart_series_t *s_ser_today,    *s_ser_usual;

static lv_obj_t *make_hour_chart(lv_obj_t *parent, uint32_t color,
                                 lv_chart_series_t **ser_out)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_size(c, 118, 62);
    lv_chart_set_type(c, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(c, 24);            // one bar per hour of day
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_column(c, 1, 0);       // thin gaps between bars
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    // The chart must NOT be its own scroll container, or the encoder gets stuck
    // trying to scroll inside it. Make it a plain focus stop that scrolls the page.
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(lvgl_port_group(), c);
    *ser_out = lv_chart_add_series(c, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    return c;
}

// LVGL 9 dropped the built-in chart axis labels, so add our own tiny X-axis
// hour row (approximately aligned to the 0..23 bars).
static void add_hour_axis(lv_obj_t *parent)
{
    lv_obj_t *x = page_text(parent, "0   6   12   18   23h");
    lv_obj_set_width(x, 118);
    lv_obj_set_style_text_font(x, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(0x8A93A0), 0);
}

static void fill_chart(lv_obj_t *c, lv_chart_series_t *s, const int v[24])
{
    int mx = 1;
    for (int i = 0; i < 24; i++) if (v[i] > mx) mx = v[i];
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    for (int i = 0; i < 24; i++) lv_chart_set_value_by_id(c, s, i, v[i]);
    lv_chart_refresh(c);
}

static void air_fetch_task(void *arg)
{
    int gen = (int)(intptr_t)arg;
    int today[24], tdc[24], ttot = 0;
    int usual[24], udc[24], utot = 0;

    int ok_t = airport_fetch_hourly(1, today, tdc, &ttot);
    int ok_u = airport_fetch_hourly(14, usual, udc, &utot);

    // Usual = average movements per active day, per hour.
    int usual_avg[24];
    for (int i = 0; i < 24; i++)
        usual_avg[i] = usual[i] / (udc[i] > 0 ? udc[i] : 1);

    lvgl_port_lock();
    if (gen == s_air_gen && s_chart_today) {          // page still open?
        if (ok_t == 0) {
            int peak = 0;
            for (int i = 0; i < 24; i++) if (today[i] > peak) peak = today[i];
            fill_chart(s_chart_today, s_ser_today, today);
            lv_label_set_text_fmt(s_air_status, "Today %d  peak %d/h", ttot, peak);
        } else {
            lv_label_set_text(s_air_status, "Fetch failed.\nWiFi connected?");
        }
        if (ok_u == 0) fill_chart(s_chart_usual, s_ser_usual, usual_avg);
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void airport_back_cb(lv_event_t *e)
{
    s_air_gen++;              // invalidate any in-flight fetch task
    s_chart_today = NULL;
    back_cb(e);
}

static void airport_cb(lv_event_t *e)
{
    (void)e;
    int gen = ++s_air_gen;

    lv_obj_t *page = page_shell("ZRH Traffic");
    lv_obj_set_style_pad_row(page, 2, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);   // vertical only (no horiz scrollbar)

    // teal = today, gray = usual (legend is the status line)
    s_air_status = page_text(page, "Loading LSZH...");
    s_chart_today = make_hour_chart(page, 0x37C8B4, &s_ser_today);
    s_chart_usual = make_hour_chart(page, 0x8A93A0, &s_ser_usual);
    add_hour_axis(page);

    page_button(page, LV_SYMBOL_LEFT " Back", airport_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(s_chart_today);   // start at the top; rotate down to Back

    xTaskCreate(air_fetch_task, "air", 8192, (void *)(intptr_t)gen, 5, NULL);
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

// --- main menu builders -----------------------------------------------------

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

static void make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    group_add_main(btn);
}

void ui_menu_start(void)
{
    s_main_scr = lv_screen_active();
    lv_obj_t *scr = s_main_scr;

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
    group_add_main(sw);

    // Backlight spinbox (0..100, live PWM)
    row = make_row(scr, "Light");
    lv_obj_t *sb = lv_spinbox_create(row);
    lv_spinbox_set_range(sb, 0, 100);
    lv_spinbox_set_digit_format(sb, 3, 0);
    lv_spinbox_set_step(sb, 5);
    lv_spinbox_set_value(sb, 100);
    lv_obj_set_width(sb, 54);
    lv_obj_add_event_cb(sb, backlight_cb, LV_EVENT_VALUE_CHANGED, NULL);
    group_add_main(sb);

    // Mode dropdown
    row = make_row(scr, "Mode");
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, "Auto\nDay\nNight");
    lv_obj_set_width(dd, 70);
    lv_obj_add_event_cb(dd, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    group_add_main(dd);

    // Action buttons -> open sub-pages
    make_button(scr, "Setup WiFi " LV_SYMBOL_RIGHT, wifi_setup_cb);
    make_button(scr, "Forget WiFi", repair_cb);
    make_button(scr, "ZRH Traffic " LV_SYMBOL_RIGHT, airport_cb);
    make_button(scr, "WiFi scan " LV_SYMBOL_RIGHT, wifi_scan_cb);
    make_button(scr, "Sensors " LV_SYMBOL_RIGHT, sensors_cb);
    make_button(scr, "Board info " LV_SYMBOL_RIGHT, board_info_cb);
    make_button(scr, "Reboot", reboot_cb);

    if (s_focus_n) lv_group_focus_obj(s_focus[0]);
}
