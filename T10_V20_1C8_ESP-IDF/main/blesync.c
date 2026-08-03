#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"
#include "provisioning.h"          // provisioning_is_connected()

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "blesync";

// --- 128-bit UUIDs (docs/DEVICE_FILE_SYNC.md §3, little-endian byte order) ----
// Base 6F4300xx-A1B2-4C3D-9E5F-0123456789AB; byte[12] selects the char.
#define SYNC_UUID(id) BLE_UUID128_INIT( \
    0xAB,0x89,0x67,0x45,0x23,0x01,0x5F,0x9E,0x3D,0x4C,0xB2,0xA1,(id),0x00,0x43,0x6F)

static const ble_uuid128_t s_svc_uuid    = SYNC_UUID(0x01);
static const ble_uuid128_t s_info_uuid   = SYNC_UUID(0x02);
static const ble_uuid128_t s_ctrl_uuid   = SYNC_UUID(0x03);
static const ble_uuid128_t s_status_uuid = SYNC_UUID(0x04);

// --- control opcodes ----------------------------------------------------------
#define OP_START_SOFTAP 0x11
#define OP_STOP_WIFI    0x12

// --- state --------------------------------------------------------------------
static bool     s_active;
static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle;
static char     s_name[20];               // "Octanis-XXXX", also the SoftAP SSID
static char     s_ap_ssid[20], s_ap_pass[16];

bool blesync_active(void) { return s_active; }

static void advertise(void);

// --- GATT access callbacks ----------------------------------------------------

static int info_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)arg;
    char json[160];
    int n = snprintf(json, sizeof(json),
        "{\"fw\":\"t10\",\"softap_ssid\":\"%s\",\"provisioned\":%s}",
        s_name, provisioning_is_connected() ? "true" : "false");
    return os_mbuf_append(ctxt->om, json, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Notify the phone with the Wi-Fi handoff (SoftAP creds + server token).
static void notify_handoff(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    char json[192];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"softap\",\"ssid\":\"%s\",\"pass\":\"%s\","
        "\"ip\":\"192.168.4.1\",\"port\":8080,\"token\":\"%s\"}",
        s_ap_ssid, s_ap_pass, filesrv_token());
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    ESP_LOGI(TAG, "handoff sent: %s", json);
}

// Bringing the AP up blocks (~1.5 s) -- do it off the NimBLE host task.
static void softap_task(void *arg)
{
    (void)arg;
    if (apmode_start(s_ap_ssid, sizeof(s_ap_ssid), s_ap_pass, sizeof(s_ap_pass)))
        filesrv_start();
    notify_handoff();
    // CRITICAL (no-PSRAM ESP32): BLE + SoftAP + LWIP together starve the heap to
    // ~2 KB, so LWIP can't allocate TX buffers and the file body stalls after the
    // headers. Once the handoff notification has reached the phone, tear the whole
    // BLE stack down -- nimble_port_deinit() disables + deinits the controller and
    // returns ~tens of KB to the heap for the Wi-Fi transfer. BLE is restarted on
    // session teardown (stop_task) for the next sync. Give the notify ~1 s to flush.
    vTaskDelay(pdMS_TO_TICKS(1000));
    blesync_stop();
    vTaskDelete(NULL);
}

static void stop_task(void *arg)
{
    (void)arg;
    filesrv_stop();
    apmode_stop();
    blesync_start();                           // BLE back up for the next session
    vTaskDelete(NULL);
}

// Public: tear down the Wi-Fi session and return to BLE-advertising idle. Called
// from the HTTP /session/stop handler and the app_main watchdog. Runs the teardown
// on its own task (filesrv_stop() must not run on the httpd task).
void blesync_teardown_wifi(void)
{
    xTaskCreate(stop_task, "ap_stop", 4096, NULL, 5, NULL);
}

static int ctrl_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)arg;
    uint8_t op = 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= 1) os_mbuf_copydata(ctxt->om, 0, 1, &op);
    ESP_LOGI(TAG, "control opcode 0x%02X", op);
    switch (op) {
    case OP_START_SOFTAP: xTaskCreate(softap_task, "ap_start", 4096, NULL, 5, NULL); break;
    case OP_STOP_WIFI:    xTaskCreate(stop_task,   "ap_stop",  4096, NULL, 5, NULL); break;
    default: break;
    }
    return 0;
}

static int status_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)ctxt; (void)arg;
    return 0;                                  // notify-only; nothing to read
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &s_info_uuid.u,   .access_cb = info_access,   .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &s_ctrl_uuid.u,   .access_cb = ctrl_access,   .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_status_uuid.u, .access_cb = status_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_status_val_handle },
            { 0 },
        },
    },
    { 0 },
};

// --- GAP / advertising --------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else if (s_active) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_active) { ESP_LOGI(TAG, "disconnected, re-advertising"); advertise(); }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    // The 128-bit service UUID (18 B) + name (14 B) + flags (3 B) overflows the
    // 31-byte adv packet -> put the name in the adv, the UUID in the scan response.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as %s (rc=0)", s_name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();                         // returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// --- public -------------------------------------------------------------------

bool blesync_start(void)
{
    if (s_active) return true;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_name, sizeof(s_name), "Octanis-%02X%02X", mac[4], mac[5]);

    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return false; }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_gatt_svcs) != 0 || ble_gatts_add_svcs(s_gatt_svcs) != 0) {
        ESP_LOGE(TAG, "gatt register failed");
        return false;
    }
    ble_svc_gap_device_name_set(s_name);

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    s_active = true;
    ESP_LOGI(TAG, "BLE sync service started");
    return true;
}

// Tear the BLE stack all the way down (host + controller), returning its RAM to the
// heap. Symmetric with blesync_start(), so BLE can be brought back for a later sync.
void blesync_stop(void)
{
    if (!s_active) return;
    s_active = false;                          // stop gap_event from re-advertising

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();                        // best-effort; ignore rc if not adv

    int rc = nimble_port_stop();               // unblocks host_task's nimble_port_run()
    if (rc == 0) {
        nimble_port_deinit();                  // disable+deinit controller -> heap freed
    } else {
        ESP_LOGE(TAG, "nimble_port_stop rc=%d (BLE not fully freed)", rc);
    }
    ESP_LOGI(TAG, "BLE stopped; heap now %u", (unsigned)esp_get_free_heap_size());
}
