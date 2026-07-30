#include "airport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "airport";

#define URL_FMT "https://bitmorse.com/airports-api/LSZH/movements?days=%d"
#define CAP     12288          // response buffer (response is ~7 KB)

typedef struct { char *buf; int len; } resp_t;

// Null-safe integer field: 0 if missing or not a number (the API/response
// shape could change -- never dereference a NULL cJSON item).
static int jint(const cJSON *o, const char *key)
{
    const cJSON *i = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(i) ? i->valueint : 0;
}

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        resp_t *r = (resp_t *)e->user_data;
        if (r->len + e->data_len < CAP - 1) {
            memcpy(r->buf + r->len, e->data, e->data_len);
            r->len += e->data_len;
        }
    }
    return ESP_OK;
}

int airport_fetch_hourly(int days, int hourly[24], int daycount[24], int *total)
{
    for (int i = 0; i < 24; i++) { hourly[i] = 0; daycount[i] = 0; }
    if (total) *total = 0;

    char url[96];
    snprintf(url, sizeof(url), URL_FMT, days);

    resp_t resp = { .buf = malloc(CAP), .len = 0 };
    if (!resp.buf) return -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = &resp,
        .timeout_ms = 10000,
        .user_agent = "T10-ESP32/1.0",
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Accept", "application/json");
    esp_http_client_set_header(cli, "Referer", "https://bitmorse.com/airports/");

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP failed: err=%s status=%d", esp_err_to_name(err), status);
        free(resp.buf);
        return -1;
    }
    resp.buf[resp.len] = '\0';

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return -1; }

    int sum = 0;
    cJSON *ends = cJSON_GetObjectItem(root, "ends");
    cJSON *end;
    cJSON_ArrayForEach(end, ends) {
        cJSON *hours = cJSON_GetObjectItem(end, "hours");
        cJSON *h;
        cJSON_ArrayForEach(h, hours) {
            int hi = jint(h, "hour");
            int l  = jint(h, "landings");
            int t  = jint(h, "takeoffs");
            int d  = jint(h, "days");
            if (hi >= 0 && hi < 24) {
                hourly[hi] += l + t;
                if (d > daycount[hi]) daycount[hi] = d;
                sum += l + t;
            }
        }
    }
    cJSON_Delete(root);

    if (total) *total = sum;
    ESP_LOGI(TAG, "LSZH movements today: %d", sum);
    return 0;
}
