#include "filesrv.h"
#include "manifest.h"
#include "sdcard.h"
#include "provisioning.h"
#include "netmgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
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
static uint8_t        s_sendbuf[8192];   // shared send buffer (httpd = 1 req at a time)
static volatile int64_t s_last_activity_ms;

static void touch(void) { s_last_activity_ms = esp_timer_get_time() / 1000; }

// ms since the last HTTP request (0 if the server isn't running). For the watchdog.
int64_t filesrv_idle_ms(void)
{
    if (!s_srv) return 0;
    return esp_timer_get_time() / 1000 - s_last_activity_ms;
}

const char *filesrv_token(void) { return s_token; }
bool filesrv_running(void)      { return s_srv != NULL; }

static void new_token(void);

// Issue a fresh session token WITHOUT starting the server. netmgr calls this so it
// can put the token in the BLE handoff, tear BLE down, and only THEN start the heavy
// HTTP server (BLE + httpd don't fit in heap together). filesrv_start() reuses this
// token instead of minting a second one that would invalidate the handed-off value.
const char *filesrv_new_token(void) { new_token(); touch(); return s_token; }

// --- helpers ----------------------------------------------------------------

static void new_token(void)
{
    // Fresh random per-session token (128-bit hex). Delivered to the phone over the
    // encrypted BLE handoff; the HTTP API gates every request on it.
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) snprintf(s_token + i * 2, 3, "%02x", r[i]);
    s_token[32] = '\0';
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
    touch();                                       // any request counts as activity
    if (s_token[0] == '\0') return false;
    // "Authorization: Bearer <token>" header (the app uses this).
    char h[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) == ESP_OK
        && strncmp(h, "Bearer ", 7) == 0 && strcmp(h + 7, s_token) == 0)
        return true;
    // ...or "?token=<token>" query param, so a plain browser URL works for testing.
    char q[128], val[40];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "token", val, sizeof(val)) == ESP_OK
        && strcmp(val, s_token) == 0)
        return true;
    return false;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "unauthorized");
    return ESP_OK;
}

// One request per TCP connection: don't offer HTTP keep-alive. esp_http_server is
// persistent by default, but this single-worker server on a low-RAM ESP32 stalls
// the body of a *second* request served on a reused socket (curl reproduces it:
// /manifest then /file on one connection hangs after the file's headers; each on a
// fresh socket streams fine). iOS URLSession pools requests onto one socket and
// hits exactly that, so downloads time out. Fix server-side (a client Connection:
// close can't be trusted -- URLSession ignores its own): tell the client we'll
// close, and queue the socket close. trigger_close is async via the control socket,
// so it fires only after THIS response is fully flushed -- never truncates it, and
// covers every return path from one call at the top of the handler.
static void close_conn(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");   // literals: stored by pointer
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
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
    close_conn(req);
    touch();
    char ssid[20];
    softap_ssid(ssid, sizeof(ssid));

    char ip[16] = "0.0.0.0";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));

    const esp_app_desc_t *app = esp_app_get_description();
    char body[288];
    // Unauthenticated reachability probe -- never leaks the token (that comes only
    // over the encrypted BLE handoff).
    snprintf(body, sizeof(body),
        "{\"fw\":\"%s\",\"provisioned\":%s,\"station_ip\":\"%s\","
        "\"softap_ssid\":\"%s\",\"softap_capable\":true}",
        app->version, provisioning_is_connected() ? "true" : "false", ip, ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

// Sink for manifest_stream_json: forward each chunk as an HTTP chunked-transfer
// piece. Streaming keeps the (potentially large) file list off DRAM entirely.
static int manifest_chunk_sink(void *ctx, const char *data, int len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, len) == ESP_OK ? 0 : -1;
}

// GET /manifest -- token-protected file list (chunked; no size ceiling).
static esp_err_t h_manifest(httpd_req_t *req)
{
    close_conn(req);
    if (!auth_ok(req)) return deny(req);
    int64_t t0 = esp_timer_get_time();
    httpd_resp_set_type(req, "application/json");
    manifest_precache_hold(true);              // full SD bus for the scan
    int rc = manifest_stream_json(manifest_chunk_sink, req);
    manifest_precache_hold(false);
    httpd_resp_send_chunk(req, NULL, 0);       // terminate the chunked response
    ESP_LOGI(TAG, "manifest streamed in %lld ms (rc=%d)", (esp_timer_get_time() - t0) / 1000, rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// GET /file/<id> -- token-protected, Range-resumable, ETag = sha256.
static esp_err_t h_file(httpd_req_t *req)
{
    close_conn(req);
    if (!auth_ok(req)) return deny(req);
    int id = id_from_uri(req->uri);
    char path[160];
    if (id < 0 || !manifest_path_for_id(id, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }
    // POSIX open/read, NOT stdio fopen/fread: newlib's small default file buffer
    // turns each read into a swarm of tiny SD transactions -> 6 KB/s vs 92 KB/s.
    int ffd = open(path, O_RDONLY);
    if (ffd < 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "open"); return ESP_FAIL; }
    long total = (long)lseek(ffd, 0, SEEK_END);

    // Disable Nagle on this socket (harmless; helps some clients).
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
            if (start > end) { close(ffd); httpd_resp_set_status(req, "416 Range Not Satisfiable"); httpd_resp_sendstr(req, ""); return ESP_OK; }
        }
    }

    // Pause the background sha256 precache for the whole transfer so this download
    // owns the SD bus (else the two contend and the client times out mid-body).
    manifest_precache_hold(true);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    char etag[70], hex[65];
    if (manifest_sha256_for_id(id, hex, sizeof(hex))) {   // sidecar-only, never computes
        snprintf(etag, sizeof(etag), "\"%s\"", hex);
        httpd_resp_set_hdr(req, "ETag", etag);
    }
    char crange[72];
    if (partial) {
        httpd_resp_set_status(req, "206 Partial Content");
        snprintf(crange, sizeof(crange), "bytes %ld-%ld/%ld", start, end, total);
        httpd_resp_set_hdr(req, "Content-Range", crange);
    }

    ESP_LOGI(TAG, "file %d: serving bytes %ld-%ld/%ld (%s)",
             id, start, end, total, partial ? "206" : "200");
    lseek(ffd, start, SEEK_SET);
    long remain = end - start + 1, sent = 0;
    int64_t t0 = esp_timer_get_time();
    bool first = true;
    while (remain > 0) {
        size_t want = remain < (long)sizeof(s_sendbuf) ? (size_t)remain : sizeof(s_sendbuf);
        int64_t tr = esp_timer_get_time();
        ssize_t r = read(ffd, s_sendbuf, want);
        if (r <= 0) {
            ESP_LOGW(TAG, "file %d: read()=%d at offset %ld (%lld ms) errno=%d",
                     id, (int)r, start + sent, (esp_timer_get_time() - tr) / 1000, errno);
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)s_sendbuf, r) != ESP_OK) {
            ESP_LOGW(TAG, "file %d: send failed at offset %ld/%ld", id, start + sent, total);
            close(ffd);
            manifest_precache_hold(false);
            return ESP_FAIL;
        }
        if (first) { ESP_LOGI(TAG, "file %d: first %d B out in %lld ms",
                              id, (int)r, (esp_timer_get_time() - t0) / 1000); first = false; }
        sent   += r;
        remain -= r;
    }
    close(ffd);
    manifest_precache_hold(false);
    httpd_resp_send_chunk(req, NULL, 0);       // end of chunked response
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "file %d: sent %ld B in %lld ms (%ld KB/s)",
             id, sent, ms, (long)(ms > 0 ? sent / ms : 0));
    return ESP_OK;
}

// DELETE /file/<id> -- token-protected.
static esp_err_t h_delete(httpd_req_t *req)
{
    close_conn(req);
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
    close_conn(req);
    if (!auth_ok(req)) return deny(req);
    ESP_LOGI(TAG, "session stop requested");
    s_token[0] = '\0';                          // invalidate
    httpd_resp_sendstr(req, "stopping");
    netmgr_request_stop_softap();               // manager stops SoftAP+server, restores base mode
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
    if (s_token[0] == '\0') new_token();        // reuse the token filesrv_new_token()
    touch();                                    // already minted for the BLE handoff;
    if (s_srv) return true;                     // only mint one if none was issued yet
    manifest_init();                            // create the scan lock (single-threaded here)
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

    // A sleeping STA drops inbound unicast (the server becomes unreachable while
    // mDNS multicast still works). Keep the radio awake while serving files.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // mDNS: per-device hostname t10-XXXX.local (last 2 MAC bytes) so multiple
    // devices coexist on one LAN. This is the WLAN-path discovery name.
    if (mdns_init() == ESP_OK) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char host[16];
        snprintf(host, sizeof(host), "t10-%02x%02x", mac[4], mac[5]);
        mdns_hostname_set(host);
        mdns_service_add(NULL, "_http", "_tcp", PORT, NULL, 0);
        ESP_LOGI(TAG, "mDNS: http://%s.local:%d", host, PORT);
    }

    // Precompute sha256 sidecars in the background so the manifest never blocks
    // on hashing (reading multi-MB files off the SPI SD is slow).
    // 16384: manifest_precache() holds a names[MAX_FILES][NAME_MAX_] array on-stack
    // (96*64 = 6 KB at MAX_FILES=96) and calls compute_sha256() (2 KB read buffer +
    // mbedtls + FATFS). Grows with MAX_FILES -- 8192 overflowed even at 40 files.
    xTaskCreate(precache_task, "sha_cache", 16384, NULL, 3, NULL);

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
