#include "radio.h"
#include "netmgr.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mp3dec.h"

static const char *TAG = "radio";

#define STATION_URL "http://mp3.radiox.ch/standard.mp3"
#define INBUF_SZ    8192          // MP3 input ring (max frame ~1440 B @192k)
#define PCM_MAX     2304          // 1152 samples * 2 channels (MPEG-1)

static volatile bool s_run;
static volatile const char *s_status = "Off";
static TaskHandle_t  s_task;

bool        radio_is_playing(void) { return s_run; }
const char *radio_status(void)     { return (const char *)s_status; }

static dac_continuous_handle_t make_dac(int rate)
{
    dac_continuous_handle_t dac = NULL;
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,     // GPIO25
        .desc_num  = 6,
        .buf_size  = 2048,
        .freq_hz   = rate,
        .offset    = 0,
        .clk_src   = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    if (dac_continuous_new_channels(&cfg, &dac) != ESP_OK) {
        ESP_LOGW(TAG, "DAC init failed @ %d Hz", rate);
        return NULL;
    }
    dac_continuous_enable(dac);
    return dac;
}

static void radio_task(void *arg)
{
    (void)arg;
    gpio_reset_pin(GPIO_NUM_25);   // release LEDC (boot melody) from the pin

    uint8_t *in  = malloc(INBUF_SZ);
    short   *pcm = malloc(PCM_MAX * sizeof(short));
    uint8_t *out = malloc(PCM_MAX);                 // mono 8-bit, <= PCM_MAX
    HMP3Decoder dec = MP3InitDecoder();
    esp_http_client_handle_t cli = NULL;
    dac_continuous_handle_t  dac = NULL;
    int inlen = 0;

    bool wifi_held = false;
    if (!in || !pcm || !out || !dec) { ESP_LOGE(TAG, "no memory"); s_status = "Error"; goto done; }

    // On-demand Wi-Fi: hold it up for the whole stream (BLE stays up, §1.3), released
    // in the cleanup below when the user stops.
    wifi_held = true;
    if (!netmgr_wifi_hold(15000)) { ESP_LOGW(TAG, "no wifi"); s_status = "No WiFi"; goto done; }

    esp_http_client_config_t hc = {
        .url = STATION_URL, .timeout_ms = 8000, .buffer_size = 1024,
        .user_agent = "T10-radio/1.0",
    };
    cli = esp_http_client_init(&hc);
    if (esp_http_client_open(cli, 0) != ESP_OK) { ESP_LOGW(TAG, "open failed"); s_status = "Error"; goto done; }
    esp_http_client_fetch_headers(cli);
    ESP_LOGI(TAG, "connected: %s", STATION_URL);
    s_status = "Buffering";

    while (s_run) {
        // Refill the input buffer.
        if (inlen < INBUF_SZ / 2) {
            int n = esp_http_client_read(cli, (char *)in + inlen, INBUF_SZ - inlen);
            if (n <= 0) { ESP_LOGW(TAG, "stream read %d", n); break; }
            inlen += n;
        }
        // Sync to an MP3 frame.
        int off = MP3FindSyncWord(in, inlen);
        if (off < 0) { inlen = 0; continue; }        // no frame; discard, refill
        if (off > 0) { memmove(in, in + off, inlen - off); inlen -= off; }

        uint8_t *p = in;
        int left = inlen;
        int err = MP3Decode(dec, &p, &left, pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue;                                 // need more data; keep buffer
        }
        int consumed = inlen - left;
        if (consumed > 0) { memmove(in, in + consumed, left); inlen = left; }
        if (err != ERR_MP3_NONE) { continue; }        // skip bad frame

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (!dac) {
            ESP_LOGI(TAG, "%d Hz, %d ch, %d kbps", fi.samprate, fi.nChans, fi.bitrate / 1000);
            dac = make_dac(fi.samprate);
            if (!dac) { s_status = "Error"; break; }
            s_status = "Playing";
        }
        // Downmix to mono + 16-bit -> 8-bit unsigned for the DAC.
        int frames = fi.outputSamps / (fi.nChans ? fi.nChans : 1);
        for (int i = 0; i < frames; i++) {
            int s = (fi.nChans == 2) ? (pcm[2 * i] + pcm[2 * i + 1]) / 2 : pcm[i];
            out[i] = (uint8_t)((s >> 8) + 128);
        }
        dac_continuous_write(dac, out, frames, NULL, 300);  // paces to real time
    }

done:
    if (dac) { dac_continuous_disable(dac); dac_continuous_del_channels(dac); }
    if (cli) { esp_http_client_close(cli); esp_http_client_cleanup(cli); }
    if (dec) MP3FreeDecoder(dec);
    free(in); free(pcm); free(out);
    if (wifi_held) netmgr_wifi_release();          // drop the Wi-Fi hold (§2.3)
    ESP_LOGI(TAG, "stopped, free heap %u", (unsigned)esp_get_free_heap_size());
    s_run = false;
    // keep a failure status ("Error"/"No WiFi") visible; otherwise back to Off
    if (s_status[0] == 'B' || s_status[0] == 'P') s_status = "Off";
    s_task = NULL;
    vTaskDelete(NULL);
}

void radio_toggle(void)
{
    if (s_run) { s_run = false; return; }             // task stops itself
    if (s_task) return;                               // still tearing down
    s_run = true;
    s_status = "Buffering";
    // Decode on core 1 so WiFi (core 0) can't starve it.
    xTaskCreatePinnedToCore(radio_task, "radio", 8192, NULL, 5, &s_task, 1);
}
