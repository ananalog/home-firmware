// Internal interfaces between the parts of home_core.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home.h"

#define TAG_CORE "home"
#define MSG_MAX HP_MAX_MESSAGE

// ---------------------------------------------------------------- message building
#define MSG_BUILD(buf, cap, outlen, type_, flags_, reqid_, writer, body)          \
    do {                                                                          \
        htlv_writer_t w_;                                                         \
        htlv_w_init(&w_, (buf), (cap));                                           \
        hp_header_t h_ = {.ver = HP_HEADER_VERSION, .type = (type_), .flags = (flags_), .req_id = (reqid_)}; \
        hp_header_write(&w_, &h_);                                                \
        writer(&w_, (body));                                                      \
        *(outlen) = w_.overflow ? 0 : w_.len;                                     \
    } while (0)

typedef enum { TR_TCP = 0, TR_BLE = 1 } home_transport_t;

// ---------------------------------------------------------------- config (config.c)
typedef struct {
    char name[49];
    char room[49];
    char ssid[33];
    char pass[65];
    uint8_t ip_mode;       // HP_IP_MODE_*
    uint32_t ip, mask, gw, dns1, dns2;
    char hostname[33];
    char server_host[65];  // empty = discover
    uint16_t server_port;
    uint8_t ble_mode;      // HP_BLE_MODE_*
    uint8_t log_level;     // HP_LOG_LEVEL_* forwarded to the server
} home_cfg_t;

extern home_cfg_t g_cfg;
void cfg_load(void);
esp_err_t cfg_save(void);
bool cfg_has_wifi(void);
// Saves new network settings as a trial: the previous ones come back if the server
// is not reached within 2 minutes after the reboot.
esp_err_t cfg_save_trial(const home_cfg_t *prev);
void cfg_trial_check_at_boot(void);
void cfg_trial_commit(void);

// ---------------------------------------------------------------- runtime (core.c)
extern const home_device_t *g_dev;
extern char g_device_id[13];
extern volatile uint8_t g_link_state;
extern volatile bool g_adopted;
extern volatile bool g_pending_verify;
void core_set_state(uint8_t s);
void core_set_time(uint64_t server_ms);
void core_ui(home_ui_event_t ev, int arg);
void core_confirm_firmware(void);
uint8_t core_boot_partition(void);

// ---------------------------------------------------------------- Wi-Fi (net.c)
void net_start(void);
bool net_wait_ip(TickType_t timeout);
bool net_has_ip(void);
esp_err_t net_scan(hp_wifi_scan_resp_t *out);
void net_status(hp_net_status_t *out);

// ---------------------------------------------------------------- server link (link.c)
void link_start(void);
esp_err_t link_send(const uint8_t *msg, size_t len);
bool link_online(void);
void link_reconnect(void);

// ---------------------------------------------------------------- BLE (ble.c)
void ble_start(void);
esp_err_t ble_send(const uint8_t *msg, size_t len);
bool ble_is_connected(void);
bool ble_unlocked(void);
bool ble_check_code(uint16_t code);
void ble_open_window(void);   // button press: allow setup without a screen code for 5 minutes
void ble_update_advertising(void);

// ---------------------------------------------------------------- dispatch (dispatch.c)
void dispatch_start(void);
void dispatch_post(home_transport_t tr, const uint8_t *msg, size_t len);
esp_err_t transport_send(home_transport_t tr, const uint8_t *msg, size_t len);

// ---------------------------------------------------------------- points (points.c)
void points_init(void);
size_t points_count(void);
const home_point_t *points_find(uint8_t id);
void points_describe(htlv_writer_t *w);            // writes DESCRIBE resp body
void points_write_samples(htlv_writer_t *w, const uint8_t *ids, size_t n);  // GET/STATE samples (n=0: all)
esp_err_t points_set(uint8_t id, const hp_value_t *in, hp_value_t *applied, uint8_t *err_code);
void points_send_state(void);
void points_flush_offline(void);

// ---------------------------------------------------------------- OTA (ota.c)
void ota_init(void);
void ota_handle(home_transport_t tr, const hp_header_t *h, const uint8_t *body, size_t n);
bool ota_busy(void);

// ---------------------------------------------------------------- misc
void button_start(void);
void logfwd_start(void);
void logfwd_drain(void);  // called by the link task
