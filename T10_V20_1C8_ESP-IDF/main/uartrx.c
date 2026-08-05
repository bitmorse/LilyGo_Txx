#include "uartrx.h"
#include "uartrx_ring.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "uartrx";

#define RX_GPIO     GPIO_NUM_21
#define UART_PORT   UART_NUM_1        // UART0 is the USB console; leave it alone
#define UART_BAUD   9600
#define RX_BUF      1024              // driver RX ring (> HW FIFO 128)

// The reader task owns the whole DATA-state lifecycle: it installs the UART, routes
// RX <- GPIO21, drains bytes while s_run is set, then on stop tears the UART down and
// restores GPIO21 to REST. So uartrx_start()/stop() are just non-blocking flag flips
// -- safe to call straight from an LVGL button callback.
static volatile uartrx_state_t s_state = UARTRX_REST;
static volatile bool           s_run;
static TaskHandle_t            s_task;
static volatile unsigned       s_bytes;

// The last-bytes ring is written by the reader task and read by the LVGL task; a
// spinlock keeps the UI from seeing a half-updated buffer (garbled hex).
static uartrx_ring_t           s_ring;
static portMUX_TYPE            s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

uartrx_state_t uartrx_state(void) { return s_state; }
unsigned       uartrx_bytes(void) { return s_bytes; }

const char *uartrx_state_str(uartrx_state_t s)
{
    return s == UARTRX_DATA ? "DATA" : "REST";
}

// GPIO21 as a plain input, internal pulls DISABLED. Reset the pin first so any UART
// matrix routing is cleared and the pin is a GPIO again (gpio_reset_pin turns the
// internal pull-up on, so we must explicitly disable pulls afterwards).
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
    s_state = UARTRX_REST;
    ESP_LOGI(TAG, "REST: GPIO%d input, no pull", RX_GPIO);
}

int uartrx_last_hex(char *out, int cap)
{
    uartrx_ring_t snap;                        // snapshot under the lock, format after
    taskENTER_CRITICAL(&s_ring_mux);
    snap = s_ring;                             // small (<=12 B) struct copy
    taskEXIT_CRITICAL(&s_ring_mux);
    return uartrx_ring_hex(&snap, out, cap);
}

static void reader_task(void *arg)
{
    (void)arg;
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // RX-only: no TX ring, TX/RTS/CTS pins unmapped, RX routed onto GPIO21.
    if (uart_driver_install(UART_PORT, RX_BUF, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_PORT, &cfg) != ESP_OK ||
        uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART attach failed");
        uart_driver_delete(UART_PORT);
        set_rest_pin();
        s_state = UARTRX_REST;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DATA: UART%d RX <- GPIO%d @ %d 8N1", UART_PORT, RX_GPIO, UART_BAUD);

    uint8_t buf[128];
    while (s_run) {
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            s_bytes += (unsigned)n;
            taskENTER_CRITICAL(&s_ring_mux);
            uartrx_ring_push(&s_ring, buf, n);
            taskEXIT_CRITICAL(&s_ring_mux);
        }
    }

    // Stop requested -> back to REST. Detach UART, restore GPIO21 to input/no-pull.
    uart_driver_delete(UART_PORT);
    set_rest_pin();
    s_state = UARTRX_REST;
    ESP_LOGI(TAG, "REST: UART detached, GPIO%d input/no-pull (%u bytes total)",
             RX_GPIO, s_bytes);
    s_task = NULL;
    vTaskDelete(NULL);
}

void uartrx_start(void)
{
    if (s_state != UARTRX_REST || s_task) return;   // already in/entering DATA
    s_bytes = 0;
    taskENTER_CRITICAL(&s_ring_mux);
    uartrx_ring_reset(&s_ring);
    taskEXIT_CRITICAL(&s_ring_mux);
    s_run = true;
    s_state = UARTRX_DATA;
    xTaskCreate(reader_task, "uartrx", 3072, NULL, 5, &s_task);
}

void uartrx_stop(void)
{
    if (s_state != UARTRX_DATA) return;
    s_run = false;                    // reader_task tears down + returns to REST
}
