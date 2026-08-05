#include "uartrx.h"
#include "uartrx_sm.h"
#include "uartrx_ring.h"
#include "uartrx_rec.h"
#include "mcap.h"
#include "sdcard.h"
#include "provisioning.h"            // time_now_ns(), time_is_synced(), service name

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "uartrx";

#define RX_GPIO     GPIO_NUM_21
#define UART_PORT   UART_NUM_1        // UART0 is the USB console; leave it alone
#define UART_BAUD   9600
#define RX_BUF      1024              // driver RX ring (> HW FIFO 128)
#define POLL_MS     20                // line/UART poll cadence
#define DEBOUNCE_MS 60                // LOW held this long = tool holding the line (not UART)
#define HEARTBEAT_MS 1000             // periodic /state record + fflush

// MCAP channel ids (schemaless: schema_id 0).
#define CH_UART     1                 // /uart_rx : bytes as base64-JSON {"b64":".."}
#define CH_STATE    2                 // /state   : json, SM transitions + heartbeat
#define CH_META     3                 // /meta    : json, device info (one-shot)

// One monitor task drives the pure state machine (uartrx_sm): it polls the debounced
// GPIO level and, while a tool is docked, the UART, then performs the ATTACH/DETACH
// action the machine returns. uartrx_start()/stop() are non-blocking flag flips (safe
// from the LVGL callback); the task owns all hardware and exits back to REST on stop.
static uartrx_sm_t             s_sm;
static volatile bool           s_run;
static TaskHandle_t            s_task;
static bool                    s_uart_attached;    // monitor-task-local
static QueueHandle_t           s_uart_q;           // UART driver event queue
static volatile unsigned       s_bytes;

// UI-visible mirrors of the SM (written by the monitor task, read by the LVGL task).
static volatile uartrx_state_t s_state = UARTRX_REST;
static volatile int64_t        s_since_ms;

// Last-bytes ring: written by the monitor task, read by the LVGL task under a spinlock.
static uartrx_ring_t           s_ring;
static portMUX_TYPE            s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// MCAP recording (monitor-task-owned). /sdcard/uartNNNN.mcap; skipped if no SD.
static FILE                   *s_fp;
static mcap_writer_t           s_mcap;
static char                    s_rec_path[40];
static volatile uint64_t       s_rec_bytes;
static bool                    s_rec_walltime;      // latched at rec_open (see rec_now_ns)

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// Per-file latched MCAP timestamp. The clock base is decided ONCE, when the
// file is opened, and every message in that file uses it -- so a file's timeline
// never jumps. If the RTC was already synced at rec_open we stamp UTC (and it
// stays synced); otherwise we stamp monotonic since-boot for the whole file,
// even if SNTP lands mid-recording. Using time_now_ns() directly would flip a
// file from boot-relative to UTC mid-stream (a ~56-year jump Foxglove can't plot).
static uint64_t rec_now_ns(void)
{
    return s_rec_walltime ? time_now_ns()
                          : (uint64_t)esp_timer_get_time() * 1000ULL;
}

uartrx_state_t uartrx_state(void)         { return s_state; }
unsigned       uartrx_bytes(void)         { return s_bytes; }
int64_t        uartrx_state_elapsed_ms(void) { return now_ms() - s_since_ms; }
uint64_t       uartrx_rec_bytes(void)     { return s_rec_bytes; }
const char    *uartrx_rec_path(void)      { return s_fp ? s_rec_path : ""; }

int uartrx_last_hex(char *out, int cap)
{
    uartrx_ring_t snap;                        // snapshot under the lock, format after
    taskENTER_CRITICAL(&s_ring_mux);
    snap = s_ring;
    taskEXIT_CRITICAL(&s_ring_mux);
    return uartrx_ring_hex(&snap, out, cap);
}

// GPIO21 as a plain input, internal pulls DISABLED. Reset the pin first so any UART
// matrix routing is cleared (gpio_reset_pin turns the internal pull-up on, so we must
// disable pulls afterwards).
static void set_rest_pin(void)
{
    gpio_reset_pin(RX_GPIO);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void uartrx_init(void)
{
    set_rest_pin();
    uartrx_sm_init(&s_sm, now_ms());
    s_state = UARTRX_REST;
    s_since_ms = now_ms();
    ESP_LOGI(TAG, "REST: GPIO%d input, no pull", RX_GPIO);
}

static bool attach_uart(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // RX-only, with an EVENT QUEUE so we let the UART hardware tell real frames
    // (UART_DATA) from the tool holding the line low while charging (UART_BREAK /
    // framing errors). RX routed onto GPIO21 via the matrix.
    if (uart_driver_install(UART_PORT, RX_BUF, 0, 20, &s_uart_q, 0) != ESP_OK ||
        uart_param_config(UART_PORT, &cfg) != ESP_OK ||
        uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART attach failed");
        uart_driver_delete(UART_PORT);
        s_uart_q = NULL;
        s_uart_attached = false;
        return false;
    }
    s_uart_attached = true;
    ESP_LOGI(TAG, "UART%d RX <- GPIO%d @ %d 8N1", UART_PORT, RX_GPIO, UART_BAUD);
    return true;
}

static void detach_uart(void)
{
    uart_driver_delete(UART_PORT);     // also frees the event queue
    s_uart_q = NULL;
    s_uart_attached = false;
    set_rest_pin();                    // back to a plain input for line polling
}

// --- MCAP recording (all on the monitor task; no-ops if SD/open failed) ------

static bool next_path(char *out, int n)
{
    for (int i = 0; i < 10000; i++) {
        snprintf(out, n, "/sdcard/uart%04d.mcap", i);
        struct stat st;
        if (stat(out, &st) != 0) return true;       // doesn't exist -> use it
    }
    return false;
}

static void rec_write_state(uartrx_state_t st)
{
    if (!s_fp) return;
    char json[80];
    int n = uartrx_rec_state_json(json, sizeof(json), uartrx_state_str(st),
                                  s_bytes, (long long)(now_ms() - s_since_ms));
    uint64_t t = rec_now_ns();
    if (mcap_write_message(&s_mcap, CH_STATE, t, t, (const uint8_t *)json, n))
        s_rec_bytes += (uint64_t)n + 22;
}

static void rec_open(void)
{
    if (!sd_mount()) { ESP_LOGW(TAG, "no SD -> not recording"); return; }
    if (!next_path(s_rec_path, sizeof(s_rec_path))) return;
    s_fp = fopen(s_rec_path, "wb");
    if (!s_fp) { ESP_LOGW(TAG, "rec open failed"); return; }
    setvbuf(s_fp, NULL, _IOFBF, 8 * 1024);         // fewer, larger SD writes

    if (!mcap_begin(&s_mcap, s_fp)) { fclose(s_fp); s_fp = NULL; return; }
    mcap_add_channel(&s_mcap, CH_UART,  0, "/uart_rx", "json");
    mcap_add_channel(&s_mcap, CH_STATE, 0, "/state",   "json");
    mcap_add_channel(&s_mcap, CH_META,  0, "/meta",    "json");
    s_rec_bytes = 0;
    // Latch the clock base for the whole file (see rec_now_ns). The /meta
    // "time_synced" flag records which base this file uses: true = UTC, false =
    // monotonic since-boot.
    s_rec_walltime = time_is_synced();

    char meta[128];
    const esp_app_desc_t *app = esp_app_get_description();
    int mn = uartrx_rec_meta_json(meta, sizeof(meta), app->version,
                                  provisioning_service_name(), UART_BAUD, RX_GPIO,
                                  s_rec_walltime);
    uint64_t t = rec_now_ns();
    mcap_write_message(&s_mcap, CH_META, t, t, (const uint8_t *)meta, mn);
    rec_write_state(s_sm.state);
    ESP_LOGI(TAG, "recording -> %s", s_rec_path);
}

static void rec_write_uart(const uint8_t *buf, int n)
{
    if (!s_fp || n <= 0) return;
    // Wrap the raw bytes as {"b64":"..."} so the channel is well-known "json"
    // (Foxglove rejects application/octet-stream). buf is at most sizeof(monitor
    // buf) = 256 bytes -> 344 base64 chars + 10 wrapper + NUL.
    char json[368];
    int jn = uartrx_rec_uart_b64(json, sizeof(json), buf, n);
    if (jn <= 0) return;                            // refused (won't fit) -- skip
    uint64_t t = rec_now_ns();
    if (mcap_write_message(&s_mcap, CH_UART, t, t, (const uint8_t *)json, (uint32_t)jn))
        s_rec_bytes += (uint64_t)jn + 22;
}

static void rec_close(void)
{
    if (!s_fp) return;
    mcap_close(&s_mcap);
    fclose(s_fp);
    s_fp = NULL;
    ESP_LOGI(TAG, "recording closed (%llu bytes)", (unsigned long long)s_rec_bytes);
}

static void monitor_task(void *arg)
{
    (void)arg;
    set_rest_pin();                    // WAIT polls the line as a plain input
    rec_open();                        // open uartNNNN.mcap (skipped if no SD)
    int64_t low_streak = 0, last_hb = now_ms();
    uartrx_state_t prev = s_sm.state;
    uint8_t buf[256];

    while (s_run) {
        int64_t now = now_ms();

        // Debounced line for tool PRESENCE only (WAIT->CHARGING on insertion, and the
        // removal grace). LOW held >= DEBOUNCE_MS = tool holding the line low; a 9600
        // frame can't hold LOW that long, so a released line clears this immediately.
        int lvl = gpio_get_level(RX_GPIO);
        low_streak = (lvl == 0) ? low_streak + POLL_MS : 0;
        bool line_low = low_streak >= DEBOUNCE_MS;

        // DATA detection is done by the UART hardware, NOT the GPIO level: drain the
        // event queue and treat only UART_DATA (validly-framed bytes) as real data.
        // The tool holding the line low while charging shows up as UART_BREAK /
        // framing errors, which we flush and ignore -- so LOW-heavy real traffic is
        // no longer mistaken for "still charging".
        bool uart_byte = false, saw_break = false;
        if (s_uart_attached && s_uart_q) {
            uart_event_t ev;
            while (xQueueReceive(s_uart_q, &ev, 0) == pdTRUE) {
                if (ev.type == UART_DATA) {
                    size_t want = ev.size > sizeof(buf) ? sizeof(buf) : ev.size;
                    int n = uart_read_bytes(UART_PORT, buf, want, 0);
                    if (n > 0) {
                        uart_byte = true;
                        s_bytes += (unsigned)n;
                        taskENTER_CRITICAL(&s_ring_mux);
                        uartrx_ring_push(&s_ring, buf, n);
                        taskEXIT_CRITICAL(&s_ring_mux);
                        rec_write_uart(buf, n);        // raw bytes -> /uart_rx
                    }
                } else {
                    saw_break = true;              // break / framing / overflow (charging low)
                }
            }
            // Clear break junk only if the round produced no real data -- otherwise a
            // break event ordered before the data event would wipe the just-read frames.
            if (saw_break && !uart_byte) uart_flush_input(UART_PORT);
        }

        uartrx_in_t in = { .line_low = line_low, .uart_byte = uart_byte, .now_ms = now };
        uartrx_act_t act = uartrx_sm_step(&s_sm, in);
        if      (act == UARTRX_ACT_ATTACH_UART) attach_uart();
        else if (act == UARTRX_ACT_DETACH_UART) detach_uart();

        s_state = s_sm.state;
        s_since_ms = s_sm.since_ms;
        if (s_sm.state != prev) {
            ESP_LOGI(TAG, "%s -> %s", uartrx_state_str(prev), uartrx_state_str(s_sm.state));
            rec_write_state(s_sm.state);   // transition -> /state
            prev = s_sm.state;
        }
        if (now - last_hb >= HEARTBEAT_MS) {  // periodic /state + flush to bound loss
            last_hb = now;
            rec_write_state(s_sm.state);
            if (s_fp) fflush(s_fp);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    // Stop requested -> REST: close the recording, detach UART, restore GPIO21.
    uartrx_sm_stop(&s_sm, now_ms());
    rec_close();
    if (s_uart_attached) detach_uart();
    else                 set_rest_pin();
    s_state = UARTRX_REST;
    s_since_ms = now_ms();
    ESP_LOGI(TAG, "stopped -> REST (%u bytes)", s_bytes);
    s_task = NULL;
    vTaskDelete(NULL);
}

void uartrx_start(void)
{
    if (s_state != UARTRX_REST || s_task) return;   // already armed/entering
    s_bytes = 0;
    taskENTER_CRITICAL(&s_ring_mux);
    uartrx_ring_reset(&s_ring);
    taskEXIT_CRITICAL(&s_ring_mux);
    uartrx_sm_init(&s_sm, now_ms());
    uartrx_sm_start(&s_sm, now_ms());               // REST -> WAIT
    s_state = s_sm.state;
    s_since_ms = s_sm.since_ms;
    s_run = true;
    // 8192: matches viblog's writer. The monitor task writes MCAP to SD (fwrite ->
    // FATFS -> sdspi, a deep chain) on top of the 256 B read buffer, and -- if the
    // card was not already mounted at boot -- runs the even deeper esp_vfs_fat_sdspi
    // mount itself. 6144 overflowed on that mount path; 8192 gives the same headroom
    // as viblog. (The boot-time mount in app_main normally makes this the early-out.)
    xTaskCreate(monitor_task, "uartrx", 8192, NULL, 5, &s_task);
}

void uartrx_stop(void)
{
    if (s_state == UARTRX_REST) return;
    s_run = false;                    // monitor_task tears down + returns to REST
}
