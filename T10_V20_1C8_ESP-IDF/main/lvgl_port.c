#include "lvgl_port.h"
#include "st7735.h"
#include "buttons.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "lvgl";

#define LV_TASK_STACK  6144
#define LV_TASK_PRIO   4
#define DISP_HOR       ST7735_WIDTH
#define DISP_VER       ST7735_HEIGHT

// Partial render buffers: 1/4 screen each, double-buffered.
#define BUF_LINES  40
static lv_color_t s_buf1[DISP_HOR * BUF_LINES];
static lv_color_t s_buf2[DISP_HOR * BUF_LINES];

static lv_display_t   *s_disp;
static lv_indev_t     *s_indev;
static lv_group_t     *s_group;
static SemaphoreHandle_t s_mutex;

// --- display flush ----------------------------------------------------------

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    // LVGL renders RGB565 little-endian; the ST7735 wants big-endian.
    lv_draw_sw_rgb565_swap(px_map, w * h);
    st7735_blit(area->x1, area->y1, area->x2, area->y2, px_map);

    lv_display_flush_ready(disp);
}

// --- 3-button encoder input -------------------------------------------------

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int prev_prev = 0, prev_next = 0;   // edge state for BTN1/BTN2
    (void)indev;

    int b_prev  = buttons_level(BTN_1);   // left   -> rotate back
    int b_next  = buttons_level(BTN_2);   // middle -> rotate forward
    int b_enter = buttons_level(BTN_3);   // right  -> push (enter/edit)

    int diff = 0;
    if (b_prev && !prev_prev) diff -= 1;
    if (b_next && !prev_next) diff += 1;
    prev_prev = b_prev;
    prev_next = b_next;

    data->enc_diff = diff;
    data->state = b_enter ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// --- tick + task ------------------------------------------------------------

static uint32_t tick_ms_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        lvgl_port_lock();
        uint32_t next_ms = lv_timer_handler();
        lvgl_port_unlock();
        if (next_ms > 20) next_ms = 20;      // keep input latency snappy
        vTaskDelay(pdMS_TO_TICKS(next_ms < 5 ? 5 : next_ms));
    }
}

// --- public -----------------------------------------------------------------

void lvgl_port_lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void lvgl_port_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

lv_group_t *lvgl_port_group(void) { return s_group; }

void lvgl_port_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();

    lv_init();
    lv_tick_set_cb(tick_ms_cb);

    s_disp = lv_display_create(DISP_HOR, DISP_VER);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, sizeof(s_buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev, encoder_read_cb);

    s_group = lv_group_create();
    lv_group_set_default(s_group);
    lv_indev_set_group(s_indev, s_group);

    xTaskCreate(lvgl_task, "lvgl", LV_TASK_STACK, NULL, LV_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "LVGL %d.%d up (%dx%d, 3-button encoder)",
             lv_version_major(), lv_version_minor(), DISP_HOR, DISP_VER);
}
