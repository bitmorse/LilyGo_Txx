#include "ui_menu.h"
#include "lvgl_port.h"
#include "wifi_scan.h"
#include "imu.h"
#include "provisioning.h"
#include "netmgr.h"
#include "airport.h"
#include "audio.h"
#include "radio.h"
#include "viblog.h"
#include "apmode.h"
#include "filesrv.h"
#include "uartrx.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

// Main menu screen + its focusable widgets, so we can restore the encoder group
// when returning from a sub-page.
#define MAX_FOCUS 16
static lv_obj_t *s_main_scr;
static lv_obj_t *s_focus[MAX_FOCUS];
static int       s_focus_n;
static lv_obj_t *s_page;              // current sub-page, or NULL
static lv_obj_t *s_wifi_status;       // live WiFi status line on the home screen
static lv_obj_t *s_wifi_bars;         // signal-strength bar container
static lv_obj_t *s_bar[4];            // the 4 signal bars

#define BAR_ON  0x37C8B4
#define BAR_OFF 0x333A42

static void page_focus_stop(lv_obj_t *o);   // fwd decl (defined below)

static void group_add_main(lv_obj_t *o)
{
    page_focus_stop(o);               // same scroll behaviour as sub-pages
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

// Register `o` as an encoder focus stop on a scrollable sub-page. Reusable on
// every page: it fixes the recurring encoder-scroll pitfalls in one place --
//   * the object must not be its own scroll container (else the encoder gets
//     stuck scrolling inside it),
//   * focusing it should scroll the page to bring it into view, and
//   * focusing the FIRST stop scrolls to the very top so the title/header shows.
#define PAGE_FOCUS_FLAG LV_OBJ_FLAG_USER_1

static void page_focus_scroll_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(o);
    uint32_t idx = lv_obj_get_index(o);
    for (uint32_t i = 0; i < idx; i++)
        if (lv_obj_has_flag(lv_obj_get_child(page, i), PAGE_FOCUS_FLAG))
            return;                     // not the first stop: default scroll is fine
    lv_obj_scroll_to_y(page, 0, LV_ANIM_ON);   // first stop -> reveal the header
}

static void page_focus_stop(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS | PAGE_FOCUS_FLAG);
    lv_obj_add_event_cb(o, page_focus_scroll_cb, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(lvgl_port_group(), o);
}

static lv_obj_t *page_button(lv_obj_t *page, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, text);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    page_focus_stop(b);
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

// Four signal bars of increasing height, aligned to the bottom.
static lv_obj_t *make_wifi_bars(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 20, 14);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);   // bars sit on a common baseline
    lv_obj_set_style_pad_column(c, 1, 0);
    for (int i = 0; i < 4; i++) {
        s_bar[i] = lv_obj_create(c);
        lv_obj_remove_style_all(s_bar[i]);
        lv_obj_set_size(s_bar[i], 3, 4 + i * 3);   // 4, 7, 10, 13 px tall
        lv_obj_set_style_radius(s_bar[i], 1, 0);
        lv_obj_set_style_bg_opa(s_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_bar[i], lv_color_hex(BAR_OFF), 0);
    }
    return c;
}

// Map RSSI (dBm) to how many of the 4 bars are lit (1..4 when connected).
static int rssi_to_bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -73) return 2;
    return 1;
}

// Live WiFi status line + signal bars on the home screen, refreshed by a timer.
static void wifi_status_update(lv_timer_t *t)
{
    (void)t;
    if (!s_wifi_status) return;
    if (provisioning_is_connected()) {
        char ssid[33];
        provisioning_ssid(ssid, sizeof(ssid));
        lv_label_set_text_fmt(s_wifi_status, LV_SYMBOL_WIFI " %s",
                              ssid[0] ? ssid : "connected");
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(0x37C8B4), 0);
        int lit = rssi_to_bars(provisioning_rssi());
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_bg_color(s_bar[i],
                lv_color_hex(i < lit ? BAR_ON : BAR_OFF), 0);
        lv_obj_remove_flag(s_wifi_bars, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_wifi_status, LV_SYMBOL_WIFI " offline");
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(0x8A93A0), 0);
        lv_obj_add_flag(s_wifi_bars, LV_OBJ_FLAG_HIDDEN);
    }
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

// Live IMU readout: an lv_timer re-reads the MPU9250 and updates the label.
static lv_obj_t   *s_imu_label;
static lv_timer_t *s_imu_timer;

static void imu_update(lv_timer_t *t)
{
    (void)t;
    if (!s_imu_label) return;
    imu_data_t d;
    if (!imu_read(&d)) {
        lv_label_set_text(s_imu_label, "MPU9250\nnot detected");
        return;
    }
    lv_label_set_text_fmt(s_imu_label,
        "Accel mg\n%d  %d  %d\nGyro dps\n%d  %d  %d\nTemp %d C",
        (int)(d.ax * 1000), (int)(d.ay * 1000), (int)(d.az * 1000),
        (int)d.gx, (int)d.gy, (int)d.gz, (int)d.temp_c);
}

static void sensors_back_cb(lv_event_t *e)
{
    if (s_imu_timer) { lv_timer_delete(s_imu_timer); s_imu_timer = NULL; }
    s_imu_label = NULL;
    back_cb(e);
}

static void sensors_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("Sensors");
    s_imu_label = page_text(page, "Reading...");
    imu_update(NULL);                                  // first sample now
    s_imu_timer = lv_timer_create(imu_update, 150, NULL);  // then ~7 Hz

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", sensors_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(back);
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
    netmgr_request_forget_wifi();        // erase creds + reboot into sync mode
}

// Kick off BLE WiFi provisioning. The manager reboots into a dedicated
// provisioning mode (single-owner NimBLE), so show the QR the "ESP BLE
// Provisioning" app scans, then the reboot happens.
static void wifi_setup_start_cb(lv_event_t *e)
{
    (void)e;
    netmgr_request_provisioning();       // sets flag + reboots into provisioning
}

static void wifi_setup_cb(lv_event_t *e)
{
    (void)e;
    net_state_t st = netmgr_state();
    lv_obj_t *page = page_shell("WiFi Setup");

    if (st == NET_PROVISIONING) {
        // Already provisioning: show the QR to pair with.
        lv_obj_t *qr = lv_qrcode_create(page);
        lv_qrcode_set_size(qr, 88);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        char payload[96];
        snprintf(payload, sizeof(payload),
                 "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                 provisioning_service_name(), provisioning_pop());
        lv_qrcode_update(qr, payload, strlen(payload));
        lv_obj_set_style_border_color(qr, lv_color_white(), 0);
        lv_obj_set_style_border_width(qr, 3, 0);   // quiet zone for scanning

        char buf[64];
        snprintf(buf, sizeof(buf), "%s  PoP %s",
                 provisioning_service_name(), provisioning_pop());
        page_text(page, buf);
    } else if (st == NET_STA_CONNECTED) {
        page_text(page, "WiFi connected.\n\nUse Forget WiFi to\nchange network.");
    } else {
        page_text(page, "Start pairing to add\nhome WiFi. The device\nreboots into setup.");
        page_button(page, LV_SYMBOL_WIFI " Start pairing", wifi_setup_start_cb);
    }

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(back);
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
    page_focus_stop(c);                          // encoder scroll stop (reusable)
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

// --- Audio clips (WAV playback via the DAC) ---------------------------------

static lv_obj_t *s_audio_status;

static void clip_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    audio_play(idx);
    if (s_audio_status)
        lv_label_set_text_fmt(s_audio_status, LV_SYMBOL_AUDIO " %s", audio_clip_name(idx));
}

static void audio_clips_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("Audio Clips");
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    int n = audio_clip_count();
    lv_obj_t *first = NULL;
    if (n == 0) {
        page_text(page, "No clips.\nAdd .wav files to\naudio_clips/ then\n'make audio'.");
    } else {
        s_audio_status = page_text(page, "Press to play");
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = lv_button_create(page);
            lv_obj_set_width(b, lv_pct(100));
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, audio_clip_name(i));
            lv_obj_center(l);
            lv_obj_add_event_cb(b, clip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            page_focus_stop(b);
            if (!first) first = b;
        }
    }

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(first ? first : back);
}

// --- Internet radio (prototype) ---------------------------------------------

static lv_obj_t   *s_radio_label;
static lv_timer_t *s_radio_timer;

static void radio_update(lv_timer_t *t)
{
    (void)t;
    if (s_radio_label)
        lv_label_set_text_fmt(s_radio_label, "Radio X\n\n%s\n\n" LV_SYMBOL_AUDIO " play/stop",
                              radio_status());
}

static void radio_toggle_cb(lv_event_t *e) { (void)e; radio_toggle(); }

static void radio_back_cb(lv_event_t *e)
{
    if (s_radio_timer) { lv_timer_delete(s_radio_timer); s_radio_timer = NULL; }
    s_radio_label = NULL;                 // radio keeps playing in the background
    back_cb(e);
}

static void radio_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("Radio");
    s_radio_label = page_text(page, "Radio X");
    radio_update(NULL);
    s_radio_timer = lv_timer_create(radio_update, 500, NULL);

    lv_obj_t *play = page_button(page, LV_SYMBOL_AUDIO " Play / Stop", radio_toggle_cb);
    page_button(page, LV_SYMBOL_LEFT " Back", radio_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(play);
}

// --- Vibration logging (high-rate accel -> SD as MCAP) ----------------------

static lv_obj_t     *s_vib_label;
static lv_obj_t     *s_vib_btn_label;
static lv_timer_t   *s_vib_timer;
static volatile bool s_vib_busy;

static void vib_update(lv_timer_t *t)
{
    (void)t;
    if (!s_vib_label) return;
    viblog_status_t s;
    viblog_get_status(&s);

    char buf[176];
    if (s.running) {
        const char *f = strrchr(s.path, '/');
        f = f ? f + 1 : s.path;
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_AUDIO " REC  %s\n%us   %uk smp\ndrop %u/%u  buf %u%%\n"
                 "%u KB   clock %s\naccel + gyro/mag/temp",
                 f, (unsigned)s.elapsed_s, (unsigned)(s.samples / 1000),
                 (unsigned)s.drops, (unsigned)s.aux_drops, (unsigned)s.buf_pct,
                 (unsigned)(s.bytes / 1024), s.time_synced ? "UTC" : "mono");
    } else if (s.err[0]) {
        snprintf(buf, sizeof(buf), "Idle\n\nError:\n%s", s.err);
    } else {
        snprintf(buf, sizeof(buf),
                 "Idle\n\n4kHz accel +/-16g\n+ gyro/mag/temp\n@100Hz, to SD as\n"
                 "MCAP. Card, Start.");
    }
    lv_label_set_text(s_vib_label, buf);

    if (s_vib_btn_label)
        lv_label_set_text(s_vib_btn_label,
            s.running ? LV_SYMBOL_STOP " Stop" : LV_SYMBOL_PLAY " Start");
}

// Start/stop can block (SD mount, flushing/closing the file), so run it off the
// LVGL task -- never block the UI thread (it holds the LVGL mutex).
static void vib_toggle_task(void *arg)
{
    (void)arg;
    if (viblog_is_running()) viblog_stop();
    else                     viblog_start();
    s_vib_busy = false;
    vTaskDelete(NULL);
}

static void vib_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (s_vib_busy) return;                 // ignore presses while (un)mounting
    s_vib_busy = true;
    xTaskCreate(vib_toggle_task, "vibtog", 5120, NULL, 5, NULL);
}

static void vib_back_cb(lv_event_t *e)
{
    if (s_vib_timer) { lv_timer_delete(s_vib_timer); s_vib_timer = NULL; }
    s_vib_label = s_vib_btn_label = NULL;   // logging keeps running in background
    back_cb(e);
}

static void vib_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("Vibration Log");
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    s_vib_label = page_text(page, "...");

    // Start/Stop button (built inline so we can update its label live).
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_width(b, lv_pct(100));
    s_vib_btn_label = lv_label_create(b);
    lv_label_set_text(s_vib_btn_label, LV_SYMBOL_PLAY " Start");
    lv_obj_center(s_vib_btn_label);
    lv_obj_add_event_cb(b, vib_toggle_cb, LV_EVENT_CLICKED, NULL);
    page_focus_stop(b);

    vib_update(NULL);
    s_vib_timer = lv_timer_create(vib_update, 500, NULL);

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", vib_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(b);
    (void)back;
}

// --- File Sync (SoftAP direct transfer) -------------------------------------

static lv_obj_t     *s_ap_label;
static lv_timer_t   *s_ap_timer;

// The manager owns the SoftAP lifecycle now; the UI just requests start/stop and
// reflects state. Creds are read from apmode once the session is up.
static void ap_update(lv_timer_t *t)
{
    (void)t;
    if (!s_ap_label) return;
    if (netmgr_state() != NET_SOFTAP || !apmode_active() || apmode_ssid()[0] == '\0') {
        lv_label_set_text(s_ap_label, "Starting SoftAP...\n(a few seconds)");
        return;
    }
    lv_label_set_text_fmt(s_ap_label,
        "Join WiFi:\n%s\npass: %s\n\nhttp://192.168.4.1:8080\n\nclients: %d",
        apmode_ssid(), apmode_pass(), apmode_clients());
}

static void filesync_back_cb(lv_event_t *e)
{
    if (s_ap_timer) { lv_timer_delete(s_ap_timer); s_ap_timer = NULL; }
    s_ap_label = NULL;
    netmgr_request_stop_softap();        // manager tears down + restores base mode
    back_cb(e);
}

static void filesync_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("File Sync");
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    s_ap_label = page_text(page, "Starting SoftAP...");
    s_ap_timer = lv_timer_create(ap_update, 500, NULL);
    netmgr_request_softap();             // manager brings up SoftAP + file server

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Stop & Back", filesync_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(back);
}

// --- Settings ---------------------------------------------------------------

static lv_obj_t *s_bootsnd_lbl;

static void boot_sound_cb(lv_event_t *e)
{
    (void)e;
    bool on = !settings_boot_sound();
    settings_set_boot_sound(on);
    if (s_bootsnd_lbl)
        lv_label_set_text_fmt(s_bootsnd_lbl, "Boot sound: %s", on ? "ON" : "OFF");
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("Settings");
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    lv_obj_t *b = lv_button_create(page);          // toggle button (press to flip)
    lv_obj_set_width(b, lv_pct(100));
    s_bootsnd_lbl = lv_label_create(b);
    lv_label_set_text_fmt(s_bootsnd_lbl, "Boot sound: %s",
                          settings_boot_sound() ? "ON" : "OFF");
    lv_obj_center(s_bootsnd_lbl);
    lv_obj_add_event_cb(b, boot_sound_cb, LV_EVENT_CLICKED, NULL);
    page_focus_stop(b);

    lv_obj_t *back = page_button(page, LV_SYMBOL_LEFT " Back", back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(b);
    (void)back;
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

// --- UART RX on GPIO21 (9600 8N1) -------------------------------------------

static lv_obj_t   *s_uart_label;
static lv_obj_t   *s_uart_btn_label;
static lv_timer_t *s_uart_timer;

static void uart_update(lv_timer_t *t)
{
    (void)t;
    if (!s_uart_label) return;
    uartrx_state_t st = uartrx_state();
    switch (st) {
    case UARTRX_WAIT:
        lv_label_set_text(s_uart_label,
            "WAIT\n\nWaiting for tool…\nGPIO21 HIGH (no tool)");
        break;
    case UARTRX_CHARGING: {
        long s = (long)(uartrx_state_elapsed_ms() / 1000);
        lv_label_set_text_fmt(s_uart_label,
            "CHARGING\n\nTool inserted (LOW)\ncharging %lds / 600s", s);
        break;
    }
    case UARTRX_DATA: {
        char hex[3 * 8 + 1];
        uartrx_last_hex(hex, sizeof(hex));
        lv_label_set_text_fmt(s_uart_label,
            "DATA\n\nbytes: %u\nlast: %s", uartrx_bytes(), hex[0] ? hex : "-");
        break;
    }
    case UARTRX_FAULT:
        lv_label_set_text(s_uart_label,
            "FAULT\n\nNo UART after 10 min\ncheck / replace tool");
        break;
    case UARTRX_REST:
    default:
        lv_label_set_text(s_uart_label,
            "REST\n\nGPIO21 @ 9600 8N1\nPress Start to arm");
        break;
    }
    if (s_uart_btn_label)
        lv_label_set_text(s_uart_btn_label,
            st == UARTRX_REST ? LV_SYMBOL_PLAY " Start"
                              : LV_SYMBOL_STOP " Stop");
}

static void uart_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (uartrx_state() == UARTRX_REST) uartrx_start();
    else                               uartrx_stop();
    uart_update(NULL);
}

static void uart_back_cb(lv_event_t *e)
{
    if (s_uart_timer) { lv_timer_delete(s_uart_timer); s_uart_timer = NULL; }
    s_uart_label = s_uart_btn_label = NULL;
    uartrx_stop();                       // leave the page in the REST state
    back_cb(e);
}

static void uart_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *page = page_shell("UART RX");
    s_uart_label = page_text(page, "State: REST");

    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_width(b, lv_pct(100));
    s_uart_btn_label = lv_label_create(b);
    lv_obj_center(s_uart_btn_label);
    lv_obj_add_event_cb(b, uart_toggle_cb, LV_EVENT_CLICKED, NULL);
    page_focus_stop(b);

    uart_update(NULL);
    s_uart_timer = lv_timer_create(uart_update, 300, NULL);

    page_button(page, LV_SYMBOL_LEFT " Back", uart_back_cb);
    lv_screen_load(page);
    lv_group_focus_obj(b);
}

// --- main menu builders -----------------------------------------------------

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
    lv_obj_set_style_pad_row(scr, 3, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "T10 V2.0");
    lv_obj_set_style_text_color(title, lv_color_hex(0x37C8B4), 0);

    // Live WiFi status + signal bars (replaces the old no-op WiFi switch).
    lv_obj_t *wrow = lv_obj_create(scr);
    lv_obj_remove_style_all(wrow);
    lv_obj_set_size(wrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(wrow, 5, 0);
    s_wifi_status = lv_label_create(wrow);
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_14, 0);
    s_wifi_bars = make_wifi_bars(wrow);
    wifi_status_update(NULL);
    lv_timer_create(wifi_status_update, 2000, NULL);

    // Actions -> sub-pages.
    make_button(scr, "ZRH Traffic " LV_SYMBOL_RIGHT, airport_cb);
    make_button(scr, "Radio " LV_SYMBOL_RIGHT, radio_cb);
    make_button(scr, "Audio Clips " LV_SYMBOL_RIGHT, audio_clips_cb);
    make_button(scr, "Sensors " LV_SYMBOL_RIGHT, sensors_cb);
    make_button(scr, "Vibration Log " LV_SYMBOL_RIGHT, vib_cb);
    make_button(scr, "File Sync " LV_SYMBOL_RIGHT, filesync_cb);
    make_button(scr, "UART RX " LV_SYMBOL_RIGHT, uart_cb);
    make_button(scr, "WiFi scan " LV_SYMBOL_RIGHT, wifi_scan_cb);
    make_button(scr, "Board info " LV_SYMBOL_RIGHT, board_info_cb);
    make_button(scr, "Setup WiFi " LV_SYMBOL_RIGHT, wifi_setup_cb);
    make_button(scr, "Settings " LV_SYMBOL_RIGHT, settings_cb);
    make_button(scr, "Forget WiFi", repair_cb);
    make_button(scr, "Reboot", reboot_cb);

    if (s_focus_n) lv_group_focus_obj(s_focus[0]);
}
