#include <string.h>

#include "core_internal.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// BLE GATT service for setup from the Android app. Same messages as over TCP, fragmented to the MTU.
// Access control: a 4-digit code shown on the screen (PAIR_CODE), or a 5-minute window after
// power-on / a button press for devices without a screen. Read-only requests are always allowed.

static const char *TAG = "ble";
static ble_uuid128_t s_svc_uuid, s_rx_uuid, s_tx_uuid, s_info_uuid;
static uint16_t s_tx_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static bool s_unlocked;
static uint16_t s_code;
static int64_t s_window_until_ms;
static uint8_t s_own_addr_type;
static bool s_synced;
static hp_ble_rx_t s_rx;
static uint8_t s_rxbuf[MSG_MAX];
static SemaphoreHandle_t s_tx_lock;
static char s_name[24];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void parse_uuid(const char *s, ble_uuid128_t *out)
{
    // "6e6f6d65-0001-4c9e-9a6e-686f6d650001" → 16 bytes, little-endian as NimBLE expects.
    uint8_t be[16];
    int k = 0;
    for (const char *p = s; *p && k < 16; p++) {
        if (*p == '-') continue;
        unsigned v;
        sscanf(p, "%2x", &v);
        be[k++] = (uint8_t)v;
        p++;
    }
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) out->value[i] = be[15 - i];
}

bool ble_is_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

bool ble_unlocked(void) { return s_unlocked || (!g_dev->has_display && now_ms() < s_window_until_ms); }

bool ble_check_code(uint16_t code)
{
    if (g_dev->has_display && s_code && code == s_code) {
        s_unlocked = true;
        core_ui(HOME_UI_PAIR_CODE, 0);
        ESP_LOGI(TAG, "phone unlocked with the code");
        return true;
    }
    if (!g_dev->has_display && now_ms() < s_window_until_ms) return true;
    return false;
}

void ble_open_window(void)
{
    s_window_until_ms = now_ms() + 5 * 60 * 1000;
    ble_update_advertising();
}

static bool may_notify_fragment(const uint8_t *frag, size_t n, void *ctx)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frag, n);
    if (!om) return false;
    for (int attempt = 0; attempt < 50; attempt++) {
        int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc == 0) return true;
        if (rc != BLE_HS_ENOMEM) return false;  // om is consumed on other errors
        vTaskDelay(pdMS_TO_TICKS(10));        // wait for free buffers
        om = ble_hs_mbuf_from_flat(frag, n);
        if (!om) return false;
    }
    return false;
}

esp_err_t ble_send(const uint8_t *msg, size_t len)
{
    if (!ble_is_connected()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    size_t payload = s_mtu > 3 ? s_mtu - 3 : 20;
    bool ok = hp_ble_tx(msg, len, payload, may_notify_fragment, NULL);
    xSemaphoreGive(s_tx_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

static int info_access(struct ble_gatt_access_ctxt *ctxt)
{
    // The info characteristic is a HELLO body: model, firmware, id, name (for the scan list).
    const esp_app_desc_t *app = esp_app_get_description();
    hp_hello_req_t h = {0};
    h.has_device_id = true;
    strlcpy(h.device_id, g_device_id, sizeof h.device_id);
    h.has_model = true;
    h.model = g_dev->model;
    h.has_hw_rev = true;
    h.hw_rev = g_dev->hw_rev;
    h.has_fw_version = true;
    strlcpy(h.fw_version, app->version, sizeof h.fw_version);
    h.has_proto_major = h.has_proto_minor = true;
    h.proto_major = HP_PROTO_MAJOR;
    h.proto_minor = HP_PROTO_MINOR;
    h.has_boot_partition = true;
    h.boot_partition = core_boot_partition();
    h.has_name = g_cfg.name[0] != 0;
    strlcpy(h.name, g_cfg.name, sizeof h.name);
    h.has_pending_verify = true;
    h.pending_verify = g_pending_verify;
    uint8_t buf[200];
    htlv_writer_t w;
    htlv_w_init(&w, buf, sizeof buf);
    hp_hello_req_write(&w, &h);
    return os_mbuf_append(ctxt->om, buf, w.len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return info_access(ctxt);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t frag[256];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, frag, sizeof frag, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (hp_ble_rx_feed(&s_rx, frag, n)) dispatch_post(TR_BLE, s_rxbuf, s_rx.len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_chr_def s_chrs[4];
static struct ble_gatt_svc_def s_svcs[2];

static int gap_event(struct ble_gap_event *e, void *arg);

static bool should_advertise(void)
{
    switch (g_cfg.ble_mode) {
    case HP_BLE_MODE_OFF: return !link_online();  // never lose the device: on when the server is unreachable
    case HP_BLE_MODE_BUTTON: return now_ms() < s_window_until_ms || !cfg_has_wifi();
    default: return true;
    }
}

static void advertise(void)
{
    if (!s_synced || ble_is_connected() || ble_gap_adv_active()) return;
    if (!should_advertise()) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;

    struct ble_hs_adv_fields sr = {0};
    sr.name = (uint8_t *)s_name;
    sr.name_len = strlen(s_name);
    sr.name_is_complete = 1;
    // Manufacturer data: company 0xFFFF, model u16, flags (bit0 configured), protocol major.
    static uint8_t mfg[6];
    mfg[0] = HP_BLE_MANUFACTURER_ID & 0xFF;
    mfg[1] = HP_BLE_MANUFACTURER_ID >> 8;
    mfg[2] = g_dev->model & 0xFF;
    mfg[3] = g_dev->model >> 8;
    mfg[4] = cfg_has_wifi() ? 1 : 0;
    mfg[5] = HP_PROTO_MAJOR;
    sr.mfg_data = mfg;
    sr.mfg_data_len = sizeof mfg;
    ble_gap_adv_rsp_set_fields(&sr);

    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = BLE_GAP_ADV_ITVL_MS(200), .itvl_max = BLE_GAP_ADV_ITVL_MS(300)};
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "adv start: %d", rc);
}

void ble_update_advertising(void)
{
    if (!s_synced) return;
    if (ble_gap_adv_active() && !should_advertise()) ble_gap_adv_stop();
    else advertise();
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            s_unlocked = false;
            hp_ble_rx_init(&s_rx, s_rxbuf, sizeof s_rxbuf);
            if (g_dev->has_display) {
                s_code = (uint16_t)(1000 + esp_random() % 9000);
                core_ui(HOME_UI_PAIR_CODE, s_code);
            }
            ESP_LOGI(TAG, "phone connected");
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_unlocked = false;
        s_code = 0;
        s_mtu = 23;
        core_ui(HOME_UI_PAIR_CODE, 0);
        ESP_LOGI(TAG, "phone disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = e->mtu.value;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    advertise();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void adv_timer(void *arg) { ble_update_advertising(); }

void ble_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    s_window_until_ms = now_ms() + 5 * 60 * 1000;  // power-on window
    parse_uuid(HP_BLE_SERVICE_UUID, &s_svc_uuid);
    parse_uuid(HP_BLE_RX_UUID, &s_rx_uuid);
    parse_uuid(HP_BLE_TX_UUID, &s_tx_uuid);
    parse_uuid(HP_BLE_INFO_UUID, &s_info_uuid);
    s_chrs[0] = (struct ble_gatt_chr_def){.uuid = &s_rx_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP};
    s_chrs[1] = (struct ble_gatt_chr_def){.uuid = &s_tx_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_tx_handle};
    s_chrs[2] = (struct ble_gatt_chr_def){.uuid = &s_info_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ};
    s_svcs[0] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_svc_uuid.u, .characteristics = s_chrs};

    char upper[8];
    snprintf(upper, sizeof upper, "%s", g_dev->short_name);
    for (char *p = upper; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    snprintf(s_name, sizeof s_name, "Home-%s-%s", upper, g_device_id + 8);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_svc_gap_device_name_set(s_name);
    ble_att_set_preferred_mtu(247);
    nimble_port_freertos_init(host_task);

    // Re-evaluate advertising periodically (button window expiry, link state).
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = adv_timer, .name = "ble_adv"};
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, 10ULL * 1000 * 1000);
    ESP_LOGI(TAG, "advertising as %s", s_name);
}
