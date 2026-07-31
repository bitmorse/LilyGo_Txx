#include "filesrv.h"
#include "manifest.h"
#include "sdcard.h"
#include "provisioning.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "filesrv";

#define PORT 8080

static httpd_handle_t s_srv;
static char           s_token[33];       // 32 hex + NUL; empty = no valid session

const char *filesrv_token(void) { return s_token; }
bool filesrv_running(void)      { return s_srv != NULL; }

// --- helpers ----------------------------------------------------------------

static void new_token(void)
{
    // DEV: a fixed token so tools/sync_test.sh doesn't chase a new one each boot.
    // Swap back to a random per-session token (esp_fill_random) once the encrypted
    // BLE WIFI_HANDOFF delivers it (task #25).
    snprintf(s_token, sizeof(s_token), "t10devtoken");
}

// Static SoftAP SSID for this device: "Octanis-XXXX" (last two MAC bytes).
static void softap_ssid(char *out, int cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, cap, "Octanis-%02X%02X", mac[4], mac[5]);
}

static bool auth_ok(httpd_req_t *req)
{
    if (s_token[0] == '\0') return false;
    char h[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK)
        return false;
    return strncmp(h, "Bearer ", 7) == 0 && strcmp(h + 7, s_token) == 0;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "unauthorized");
    return ESP_OK;
}

static int id_from_uri(const char *uri)   // ".../file/7" -> 7
{
    const char *slash = strrchr(uri, '/');
    return slash ? atoi(slash + 1) : -1;
}

// --- handlers ---------------------------------------------------------------

// GET /info -- UNAUTHENTICATED reachability + capability probe (no file contents).
static esp_err_t h_info(httpd_req_t *req)
{
    char ssid[20];
    softap_ssid(ssid, sizeof(ssid));

    char ip[16] = "0.0.0.0";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));

    const esp_app_desc_t *app = esp_app_get_description();
    char body[320];
    // NOTE: "token" here is a DEV convenience so tools/sync_test.sh can run without
    // the serial dance. REMOVE it once the encrypted BLE WIFI_HANDOFF delivers the
    // token (task #25) -- an unauthenticated endpoint must never leak it in production.
    snprintf(body, sizeof(body),
        "{\"fw\":\"%s\",\"provisioned\":%s,\"station_ip\":\"%s\","
        "\"softap_ssid\":\"%s\",\"softap_capable\":true,\"token\":\"%s\"}",
        app->version, provisioning_is_connected() ? "true" : "false", ip, ssid, s_token);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

// GET /manifest -- token-protected file list.
static esp_err_t h_manifest(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    static char json[12288];
    int64_t t0 = esp_timer_get_time();
    int n = manifest_build_json(json, sizeof(json));
    ESP_LOGI(TAG, "manifest: %d bytes in %lld ms", n, (esp_timer_get_time() - t0) / 1000);
    if (n < 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}

// GET /file/<id> -- token-protected, Range-resumable, ETag = sha256.
static esp_err_t h_file(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    int id = id_from_uri(req->uri);
    char path[160];
    if (id < 0 || !manifest_path_for_id(id, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "open"); return ESP_FAIL; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);

    // Disable Nagle on this socket: without it, small chunked writes deadlock with
    // the peer's delayed ACKs -> ~single-digit KB/s. Biggest throughput win.
    int fd = httpd_req_to_sockfd(req), one = 1;
    if (fd >= 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    long start = 0, end = total - 1;
    bool partial = false;
    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long s = 0, e = -1;
        if (sscanf(range, "bytes=%ld-%ld", &s, &e) >= 1) {
            partial = true;
            start = s;
            if (e >= 0 && e < total) end = e;
            if (start < 0) start = 0;
            if (start > end) { fclose(f); httpd_resp_set_status(req, "416 Range Not Satisfiable"); httpd_resp_sendstr(req, ""); return ESP_OK; }
        }
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    char etag[70], hex[65];
    if (manifest_sha256_for_id(id, hex, sizeof(hex))) {
        snprintf(etag, sizeof(etag), "\"%s\"", hex);
        httpd_resp_set_hdr(req, "ETag", etag);
    }
    char crange[72];
    if (partial) {
        httpd_resp_set_status(req, "206 Partial Content");
        snprintf(crange, sizeof(crange), "bytes %ld-%ld/%ld", start, end, total);
        httpd_resp_set_hdr(req, "Content-Range", crange);
    }

    fseek(f, start, SEEK_SET);
    long remain = end - start + 1;
    // 8 KB static buffer: sector-aligned SD reads + fewer sends (research sweet
    // spot). Static (httpd serves one request at a time) so no heap cost — free
    // heap can be ~20 KB in unprovisioned AP+BLE mode where a big malloc fails.
    static uint8_t buf[8192];
    while (remain > 0) {
        size_t want = remain < (long)sizeof(buf) ? (size_t)remain : sizeof(buf);
        size_t r = fread(buf, 1, want, f);
        if (r == 0) break;
        if (httpd_resp_send_chunk(req, (const char *)buf, r) != ESP_OK) {
            ESP_LOGW(TAG, "file %d: send failed at offset %ld/%ld",
                     id, total - remain, total);
            fclose(f);
            return ESP_FAIL;
        }
        remain -= r;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);       // end of chunked response
    return ESP_OK;
}

// DELETE /file/<id> -- token-protected.
static esp_err_t h_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    int id = id_from_uri(req->uri);
    if (id < 0 || !manifest_delete_id(id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "deleted");
    return ESP_OK;
}

// POST /session/stop -- invalidate the token; SoftAP teardown wired in later.
static esp_err_t h_stop(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    ESP_LOGI(TAG, "session stop requested");
    s_token[0] = '\0';                          // invalidate
    httpd_resp_sendstr(req, "stopping");
    return ESP_OK;
}

// --- lifecycle --------------------------------------------------------------

static void precache_task(void *arg)
{
    (void)arg;
    manifest_precache();
    vTaskDelete(NULL);
}

bool filesrv_start(void)
{
    if (s_srv) return true;
    if (!sd_mount()) { ESP_LOGE(TAG, "no SD card"); return false; }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = PORT;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 8;
    cfg.lru_purge_enable  = true;
    cfg.send_wait_timeout = 30;   // tolerate a slow/high-latency link mid-transfer
    cfg.recv_wait_timeout = 30;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return false; }

    httpd_uri_t routes[] = {
        { .uri = "/info",         .method = HTTP_GET,    .handler = h_info },
        { .uri = "/manifest",     .method = HTTP_GET,    .handler = h_manifest },
        { .uri = "/file/*",       .method = HTTP_GET,    .handler = h_file },
        { .uri = "/file/*",       .method = HTTP_DELETE, .handler = h_delete },
        { .uri = "/session/stop", .method = HTTP_POST,   .handler = h_stop },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_srv, &routes[i]);

    new_token();

    // A sleeping STA drops inbound unicast (the server becomes unreachable while
    // mDNS multicast still works). Keep the radio awake while serving files.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // mDNS so the file server is reachable at http://t10.local:8080 for testing.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("t10");
        mdns_service_add(NULL, "_http", "_tcp", PORT, NULL, 0);
    }

    // Precompute sha256 sidecars in the background so the manifest never blocks
    // on hashing (reading multi-MB files off the SPI SD is slow).
    xTaskCreate(precache_task, "sha_cache", 6144, NULL, 3, NULL);

    ESP_LOGI(TAG, "file server on :%d  (token=%s)  http://t10.local:%d/info",
             PORT, s_token, PORT);
    return true;
}

void filesrv_stop(void)
{
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
    mdns_free();
    s_token[0] = '\0';
    ESP_LOGI(TAG, "file server stopped");
}
