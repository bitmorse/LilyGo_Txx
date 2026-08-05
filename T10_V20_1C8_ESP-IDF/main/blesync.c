#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"
#include "netmgr.h"                // netmgr_request_softap()/stop/provision
#include "provisioning.h"          // provisioning_is_connected()
#include "wifi_creds.h"            // wifi_creds_parse()

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_random.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Provided by the NimBLE store/config component (persists bonds to NVS).
extern void ble_store_config_init(void);

static const char *TAG = "blesync";

// --- 128-bit UUIDs (docs/DEVICE_FILE_SYNC.md §3, little-endian byte order) ----
// Base 6F4300xx-A1B2-4C3D-9E5F-0123456789AB; byte[12] selects the char.
#define SYNC_UUID(id) BLE_UUID128_INIT( \
    0xAB,0x89,0x67,0x45,0x23,0x01,0x5F,0x9E,0x3D,0x4C,0xB2,0xA1,(id),0x00,0x43,0x6F)

static const ble_uuid128_t s_svc_uuid    = SYNC_UUID(0x01);
static const ble_uuid128_t s_info_uuid   = SYNC_UUID(0x02);
static const ble_uuid128_t s_ctrl_uuid   = SYNC_UUID(0x03);
static const ble_uuid128_t s_status_uuid = SYNC_UUID(0x04);
static const ble_uuid128_t s_creds_uuid  = SYNC_UUID(0x05);   // WIFI_CREDS (WRITE)

// --- control opcodes ----------------------------------------------------------
#define OP_START_SOFTAP   0x11
#define OP_STOP_WIFI      0x12
#define OP_SET_MODE_WLAN  0x13
#define OP_SET_MODE_BLE   0x14
#define OP_UNPAIR         0x16

// --- state --------------------------------------------------------------------
static bool     s_active;
static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle;
static char     s_name[20];               // "Octanis-XXXX", also the SoftAP SSID
static volatile uint32_t s_passkey;       // non-zero = pairing code to show on the TFT

bool     blesync_active(void)  { return s_active; }
uint32_t blesync_passkey(void) { return s_passkey; }

bool blesync_is_paired(void)
{
    if (!s_active) return false;
    int count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    return count > 0;
}

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

// Notify the phone with the Wi-Fi handoff (SoftAP creds + server token). Public:
// netmgr calls this once the AP + file server are up and the phone is still on the
// BLE link, just before it tears BLE down for the transfer.
void blesync_notify_handoff(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    char json[192];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"softap\",\"ssid\":\"%s\",\"pass\":\"%s\","
        "\"ip\":\"192.168.4.1\",\"port\":8080,\"token\":\"%s\"}",
        apmode_ssid(), apmode_pass(), filesrv_token());
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    ESP_LOGI(TAG, "handoff sent: %s", json);
}

// WLAN-path handoff: the phone stays on its home network and pulls files from the
// device's STA IP -- no SoftAP join. netmgr calls this before tearing BLE down to
// serve over the LAN.
void blesync_notify_wlan_handoff(const char *ip, int port, const char *token)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    char json[160];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"wlan\",\"ip\":\"%s\",\"port\":%d,\"token\":\"%s\"}",
        ip, port, token);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    ESP_LOGI(TAG, "wlan handoff: %s", json);
}

// The control characteristic just forwards intent to netmgr, which owns the WiFi
// mode and BLE lifecycle (bring SoftAP up, hand off, free BLE / restore).
static int ctrl_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)arg;
    uint8_t op = 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= 1) os_mbuf_copydata(ctxt->om, 0, 1, &op);
    ESP_LOGI(TAG, "control opcode 0x%02X", op);
    switch (op) {
    case OP_START_SOFTAP:  netmgr_request_softap(); break;
    case OP_STOP_WIFI:     netmgr_request_stop_softap(); break;
    case OP_SET_MODE_WLAN: netmgr_request_set_mode(true);  break;
    case OP_SET_MODE_BLE:  netmgr_request_set_mode(false); break;
    case OP_UNPAIR:        ble_store_clear(); ESP_LOGI(TAG, "unpaired (bonds cleared)"); break;
    default: break;
    }
    return 0;
}

static int status_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)ctxt; (void)arg;
    return 0;                                  // notify-only; nothing to read
}

// WIFI_CREDS: the app writes "<ssid>\0<pass>" to provision. netmgr stores it as a
// candidate and verifies live (GOT_IP commits; failure discards + notifies).
static int creds_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr; (void)arg;
    uint8_t buf[128];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    os_mbuf_copydata(ctxt->om, 0, len, buf);
    char ssid[33], pass[65];
    if (!wifi_creds_parse(buf, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "WIFI_CREDS: bad payload");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    ESP_LOGI(TAG, "WIFI_CREDS write: ssid='%s'", ssid);
    netmgr_request_provision(ssid, pass);
    return 0;
}

// Notify the phone with the provisioning result of its last WIFI_CREDS write.
void blesync_notify_prov_result(bool ok, const char *err)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    char json[128];
    int n = ok ? snprintf(json, sizeof(json), "{\"prov\":\"ok\"}")
               : snprintf(json, sizeof(json), "{\"prov\":\"fail\",\"err\":\"%s\"}",
                          err ? err : "");
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    ESP_LOGI(TAG, "prov result: %s", json);
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &s_info_uuid.u,   .access_cb = info_access,   .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &s_ctrl_uuid.u,   .access_cb = ctrl_access,   .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_creds_uuid.u,  .access_cb = creds_access,  .flags = BLE_GATT_CHR_F_WRITE },
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
        s_passkey = 0;                         // dismiss any pairing code
        if (s_active) { ESP_LOGI(TAG, "disconnected, re-advertising"); advertise(); }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) advertise();
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // DISPLAY_ONLY: we generate a 6-digit code, show it on the TFT, and inject it;
        // the user types the same code into the app to complete MITM-protected pairing.
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP };
            io.passkey = esp_random() % 1000000u;
            s_passkey  = io.passkey ? io.passkey : 1;   // never 0 (0 == "none")
            ESP_LOGW(TAG, "PAIRING passkey: %06u", (unsigned)io.passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc) ESP_LOGE(TAG, "inject_io rc=%d", rc);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change: status=%d, paired=%d",
                 event->enc_change.status, blesync_is_paired());
        s_passkey = 0;                         // pairing finished -> dismiss the code
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Peer wants to pair but a bond already exists -> drop it and let it re-pair.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

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

    // MITM-protected bonding: the device shows a 6-digit passkey on its TFT
    // (io_cap = DISPLAY_ONLY) which the app user enters. Bonds persist in NVS.
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 1;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();                   // persist bonds to NVS

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
