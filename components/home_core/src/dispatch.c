#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// One worker handles requests from both transports (TCP and BLE), one at a time.
// Long operations (OTA erase, Wi-Fi scan) never block the network or BLE host tasks.

static const char *TAG = "dispatch";
static QueueHandle_t s_queue;
static uint8_t s_out[MSG_MAX];
extern char g_server_addr[80];

typedef struct {
    home_transport_t tr;
    uint16_t len;
    uint8_t data[];
} inmsg_t;

esp_err_t transport_send(home_transport_t tr, const uint8_t *msg, size_t len)
{
    return tr == TR_TCP ? link_send(msg, len) : ble_send(msg, len);
}

void dispatch_post(home_transport_t tr, const uint8_t *msg, size_t len)
{
    inmsg_t *m = malloc(sizeof *m + len);
    if (!m) return;
    m->tr = tr;
    m->len = (uint16_t)len;
    memcpy(m->data, msg, len);
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping message");
        free(m);
    }
}

static void reply_error(home_transport_t tr, const hp_header_t *h, uint8_t code, const char *text)
{
    hp_error_body_t e = {.has_code = true, .code = code, .has_text = text != NULL};
    if (text) strlcpy(e.text, text, sizeof e.text);
    size_t n;
    MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP | HP_FLAGS_ERR, h->req_id, hp_error_body_write, &e);
    transport_send(tr, s_out, n);
}

static void reply_empty(home_transport_t tr, const hp_header_t *h)
{
    uint8_t b[HP_HEADER_SIZE];
    htlv_writer_t w;
    htlv_w_init(&w, b, sizeof b);
    hp_header_t r = {.ver = HP_HEADER_VERSION, .type = h->type, .flags = HP_FLAGS_RESP, .req_id = h->req_id};
    hp_header_write(&w, &r);
    transport_send(tr, b, w.len);
}

static uint8_t decode_error(hp_decode_result_t r)
{
    return r == HP_DECODE_UNSUPPORTED ? HP_ERROR_CODE_UNSUPPORTED : HP_ERROR_CODE_BAD_REQUEST;
}

// Requests allowed before the device is adopted (TCP) or the phone entered the code (BLE).
static bool read_only(uint8_t type)
{
    switch (type) {
    case HP_MSG_PING:
    case HP_MSG_DESCRIBE:
    case HP_MSG_GET:
    case HP_MSG_IDENTIFY:
    case HP_MSG_NET_STATUS:
    case HP_MSG_OTA_STATUS:
    case HP_MSG_PAIR_CODE:
        return true;
    default:
        return false;
    }
}

static void handle_cfg_set(home_transport_t tr, const hp_header_t *h, const hp_cfg_set_req_t *req)
{
    const hp_device_config_t *c = &req->config;
    home_cfg_t prev = g_cfg;
    bool net_changed = false;
    if (c->has_name) strlcpy(g_cfg.name, c->name, sizeof g_cfg.name);
    if (c->has_room) strlcpy(g_cfg.room, c->room, sizeof g_cfg.room);
    if (c->has_wifi_ssid && strcmp(c->wifi_ssid, g_cfg.ssid) != 0) {
        strlcpy(g_cfg.ssid, c->wifi_ssid, sizeof g_cfg.ssid);
        net_changed = true;
    }
    if (c->has_wifi_pass) {
        strlcpy(g_cfg.pass, c->wifi_pass, sizeof g_cfg.pass);
        net_changed = true;
    }
    if (c->has_net) {
        const hp_net_config_t *n = &c->net;
        if (n->has_ip_mode) g_cfg.ip_mode = n->ip_mode;
        if (g_cfg.ip_mode == HP_IP_MODE_STATIC) {
            if (!n->has_ip || !n->has_mask || !n->has_gw || !n->ip || !n->mask) {
                g_cfg = prev;
                reply_error(tr, h, HP_ERROR_CODE_INVALID_VALUE, "static mode needs ip, mask and gateway");
                return;
            }
            g_cfg.ip = n->ip;
            g_cfg.mask = n->mask;
            g_cfg.gw = n->gw;
            g_cfg.dns1 = n->has_dns1 ? n->dns1 : n->gw;
            g_cfg.dns2 = n->has_dns2 ? n->dns2 : 0;
        }
        if (n->has_hostname && n->hostname[0]) strlcpy(g_cfg.hostname, n->hostname, sizeof g_cfg.hostname);
        net_changed = true;
    }
    if (c->has_server_host) {
        strlcpy(g_cfg.server_host, c->server_host, sizeof g_cfg.server_host);
        net_changed = true;
    }
    if (c->has_server_port) {
        g_cfg.server_port = c->server_port;
        net_changed = true;
    }
    if (c->has_ble_mode) g_cfg.ble_mode = c->ble_mode;
    if (c->has_log_level) g_cfg.log_level = c->log_level;

    esp_err_t err = net_changed ? cfg_save_trial(&prev) : cfg_save();
    if (err != ESP_OK) {
        reply_error(tr, h, HP_ERROR_CODE_STORAGE, esp_err_to_name(err));
        return;
    }
    reply_empty(tr, h);
    ble_update_advertising();
    if (net_changed) {
        vTaskDelay(pdMS_TO_TICKS(500));  // let the answer leave
        home_restart("network settings changed");
    }
}

static void handle_cfg_get(home_transport_t tr, const hp_header_t *h)
{
    static hp_cfg_get_resp_t r;
    memset(&r, 0, sizeof r);
    hp_device_config_t *c = &r.config;
    r.has_config = true;
    c->has_name = c->has_room = c->has_wifi_ssid = true;
    strlcpy(c->name, g_cfg.name, sizeof c->name);
    strlcpy(c->room, g_cfg.room, sizeof c->room);
    strlcpy(c->wifi_ssid, g_cfg.ssid, sizeof c->wifi_ssid);
    c->has_net = true;
    c->net.has_ip_mode = true;
    c->net.ip_mode = g_cfg.ip_mode;
    if (g_cfg.ip_mode == HP_IP_MODE_STATIC) {
        c->net.has_ip = c->net.has_mask = c->net.has_gw = c->net.has_dns1 = true;
        c->net.ip = g_cfg.ip;
        c->net.mask = g_cfg.mask;
        c->net.gw = g_cfg.gw;
        c->net.dns1 = g_cfg.dns1;
        c->net.has_dns2 = g_cfg.dns2 != 0;
        c->net.dns2 = g_cfg.dns2;
    }
    c->net.has_hostname = true;
    strlcpy(c->net.hostname, g_cfg.hostname, sizeof c->net.hostname);
    c->has_server_host = c->has_server_port = c->has_ble_mode = c->has_log_level = true;
    strlcpy(c->server_host, g_cfg.server_host, sizeof c->server_host);
    c->server_port = g_cfg.server_port;
    c->ble_mode = g_cfg.ble_mode;
    c->log_level = g_cfg.log_level;
    size_t n;
    MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP, h->req_id, hp_cfg_get_resp_write, &r);
    transport_send(tr, s_out, n);
}

static void handle(home_transport_t tr, const uint8_t *msg, size_t len)
{
    hp_header_t h;
    if (!hp_header_read(msg, len, &h) || (h.flags & HP_FLAGS_RESP)) return;
    const uint8_t *body = msg + HP_HEADER_SIZE;
    size_t bn = len - HP_HEADER_SIZE;
    bool noack = h.flags & HP_FLAGS_NOACK;

    if (!read_only(h.type)) {
        if (tr == TR_TCP && !g_adopted) {
            reply_error(tr, &h, HP_ERROR_CODE_FORBIDDEN, "not adopted");
            return;
        }
        if (tr == TR_BLE && !ble_unlocked()) {
            reply_error(tr, &h, HP_ERROR_CODE_FORBIDDEN, "enter the code shown on the device");
            return;
        }
    }

    size_t n = 0;
    switch (h.type) {
    case HP_MSG_PING: {
        hp_ping_req_t p;
        hp_ping_req_read(&p, body, bn);
        hp_ping_resp_t r = {.has_time_ms = p.has_time_ms, .time_ms = p.time_ms};
        MSG_BUILD(s_out, sizeof s_out, &n, h.type, HP_FLAGS_RESP, h.req_id, hp_ping_resp_write, &r);
        break;
    }
    case HP_MSG_DESCRIBE: {
        htlv_writer_t w;
        htlv_w_init(&w, s_out, sizeof s_out);
        hp_header_t rh = {.ver = HP_HEADER_VERSION, .type = h.type, .flags = HP_FLAGS_RESP, .req_id = h.req_id};
        hp_header_write(&w, &rh);
        points_describe(&w);
        n = w.overflow ? 0 : w.len;
        if (!n) {
            reply_error(tr, &h, HP_ERROR_CODE_INTERNAL, "description too large");
            return;
        }
        break;
    }
    case HP_MSG_GET: {
        hp_get_req_t *g = calloc(1, sizeof *g);
        if (!g) return;
        hp_decode_result_t dr = hp_get_req_read(g, body, bn);
        if (dr != HP_DECODE_OK) {
            free(g);
            reply_error(tr, &h, decode_error(dr), NULL);
            return;
        }
        htlv_writer_t w;
        htlv_w_init(&w, s_out, sizeof s_out);
        hp_header_t rh = {.ver = HP_HEADER_VERSION, .type = h.type, .flags = HP_FLAGS_RESP, .req_id = h.req_id};
        hp_header_write(&w, &rh);
        points_write_samples(&w, g->points, g->points_count);
        free(g);
        n = w.overflow ? 0 : w.len;
        break;
    }
    case HP_MSG_SET: {
        hp_set_req_t s;
        hp_decode_result_t dr = hp_set_req_read(&s, body, bn);
        if (dr != HP_DECODE_OK || !s.has_point) {
            reply_error(tr, &h, decode_error(dr), "point required");
            return;
        }
        hp_value_t applied;
        uint8_t code = 0;
        if (points_set(s.point, &s.value, &applied, &code) != ESP_OK) {
            reply_error(tr, &h, code ? code : HP_ERROR_CODE_INVALID_VALUE, NULL);
            return;
        }
        hp_set_resp_t r = {.has_value = true, .value = applied};
        MSG_BUILD(s_out, sizeof s_out, &n, h.type, HP_FLAGS_RESP, h.req_id, hp_set_resp_write, &r);
        if (tr == TR_BLE) home_report(s.point, applied);  // keep the server in sync
        break;
    }
    case HP_MSG_INVOKE: {
        hp_invoke_req_t iv;
        if (hp_invoke_req_read(&iv, body, bn) != HP_DECODE_OK || !iv.has_point) {
            reply_error(tr, &h, HP_ERROR_CODE_BAD_REQUEST, NULL);
            return;
        }
        const home_point_t *p = points_find(iv.point);
        if (!p || p->kind != HP_POINT_KIND_ACTION || !g_dev->on_invoke) {
            reply_error(tr, &h, HP_ERROR_CODE_NOT_FOUND, "no such action");
            return;
        }
        hp_invoke_resp_t r = {0};
        esp_err_t err = g_dev->on_invoke(iv.point, iv.has_arg ? iv.arg : p->arg_default, r.text, sizeof r.text);
        if (err != ESP_OK) {
            reply_error(tr, &h, err == ESP_ERR_INVALID_ARG ? HP_ERROR_CODE_INVALID_VALUE : HP_ERROR_CODE_SENSOR, r.text[0] ? r.text : esp_err_to_name(err));
            return;
        }
        r.has_text = r.text[0] != 0;
        MSG_BUILD(s_out, sizeof s_out, &n, h.type, HP_FLAGS_RESP, h.req_id, hp_invoke_resp_write, &r);
        break;
    }
    case HP_MSG_CFG_GET:
        handle_cfg_get(tr, &h);
        return;
    case HP_MSG_CFG_SET: {
        hp_cfg_set_req_t *c = calloc(1, sizeof *c);
        if (!c) return;
        hp_decode_result_t dr = hp_cfg_set_req_read(c, body, bn);
        if (dr != HP_DECODE_OK) reply_error(tr, &h, decode_error(dr), NULL);
        else handle_cfg_set(tr, &h, c);
        free(c);
        return;
    }
    case HP_MSG_WIFI_SCAN: {
        hp_wifi_scan_resp_t *r = calloc(1, sizeof *r);
        if (!r) return;
        esp_err_t err = net_scan(r);
        if (err != ESP_OK) reply_error(tr, &h, HP_ERROR_CODE_BUSY, esp_err_to_name(err));
        else {
            MSG_BUILD(s_out, sizeof s_out, &n, h.type, HP_FLAGS_RESP, h.req_id, hp_wifi_scan_resp_write, r);
            transport_send(tr, s_out, n);
        }
        free(r);
        return;
    }
    case HP_MSG_NET_STATUS: {
        static hp_net_status_resp_t r;
        memset(&r, 0, sizeof r);
        r.has_status = true;
        net_status(&r.status);
        if (link_online()) {
            r.status.has_server = true;
            strlcpy(r.status.server, g_server_addr, sizeof r.status.server);
        }
        MSG_BUILD(s_out, sizeof s_out, &n, h.type, HP_FLAGS_RESP, h.req_id, hp_net_status_resp_write, &r);
        break;
    }
    case HP_MSG_OTA_BEGIN:
    case HP_MSG_OTA_DATA:
    case HP_MSG_OTA_END:
    case HP_MSG_OTA_ABORT:
    case HP_MSG_OTA_STATUS:
        ota_handle(tr, &h, body, bn);
        return;
    case HP_MSG_OTA_CONFIRM:
        core_confirm_firmware();
        reply_empty(tr, &h);
        return;
    case HP_MSG_REBOOT:
        reply_empty(tr, &h);
        vTaskDelay(pdMS_TO_TICKS(300));
        home_restart("requested");
        return;
    case HP_MSG_FACTORY_RESET: {
        hp_factory_reset_req_t f;
        if (hp_factory_reset_req_read(&f, body, bn) != HP_DECODE_OK || !f.has_mode || f.mode < 1 || f.mode > 3) {
            reply_error(tr, &h, HP_ERROR_CODE_BAD_REQUEST, "mode 1..3");
            return;
        }
        reply_empty(tr, &h);
        vTaskDelay(pdMS_TO_TICKS(300));
        home_factory_reset(f.mode);
        return;
    }
    case HP_MSG_IDENTIFY: {
        hp_identify_req_t id;
        hp_identify_req_read(&id, body, bn);
        core_ui(HOME_UI_IDENTIFY, id.has_seconds && id.seconds ? id.seconds : 10);
        reply_empty(tr, &h);
        return;
    }
    case HP_MSG_PAIR_CODE: {
        hp_pair_code_req_t pc;
        hp_pair_code_req_read(&pc, body, bn);
        if (tr != TR_BLE || !pc.has_code || !ble_check_code(pc.code)) {
            reply_error(tr, &h, HP_ERROR_CODE_FORBIDDEN, "wrong code");
            return;
        }
        reply_empty(tr, &h);
        return;
    }
    default:
        if (!noack) reply_error(tr, &h, HP_ERROR_CODE_UNSUPPORTED, hp_msg_name(h.type));
        return;
    }
    if (n) transport_send(tr, s_out, n);
    else reply_error(tr, &h, HP_ERROR_CODE_INTERNAL, "answer does not fit");
}

static void worker(void *arg)
{
    for (;;) {
        inmsg_t *m;
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) continue;
        handle(m->tr, m->data, m->len);
        free(m);
    }
}

void dispatch_start(void)
{
    s_queue = xQueueCreate(12, sizeof(inmsg_t *));
    xTaskCreate(worker, "home_dispatch", 8192, NULL, 4, NULL);
}
