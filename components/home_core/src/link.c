#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "core_internal.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

// TCP link to the server: discovery, HELLO handshake, framed messages, keepalive, reconnects.

static const char *TAG = "link";
static int s_sock = -1;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_online;
static volatile bool s_reconnect;
static uint8_t s_txframe[MSG_MAX + HP_FRAME_OVERHEAD];
static uint8_t s_rx[MSG_MAX * 2 + HP_FRAME_OVERHEAD];
static size_t s_rx_len;
static uint16_t s_req_id;
char g_server_addr[80];

bool link_online(void) { return s_online; }

void link_reconnect(void) { s_reconnect = true; }

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

esp_err_t link_send(const uint8_t *msg, size_t len)
{
    if (s_sock < 0 || !s_tx_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t res = ESP_OK;
    size_t n = hp_frame_encode(msg, len, s_txframe, sizeof s_txframe);
    size_t off = 0;
    while (n && off < n && s_sock >= 0) {
        int r = send(s_sock, s_txframe + off, n - off, 0);
        if (r <= 0) {
            res = ESP_FAIL;
            s_reconnect = true;
            break;
        }
        off += (size_t)r;
    }
    if (!n) res = ESP_ERR_INVALID_SIZE;
    xSemaphoreGive(s_tx_lock);
    return res;
}

// UDP broadcast "HOME?" → "HOME <port>" from the server.
static bool discover(char *host, size_t cap, uint16_t *port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct timeval tv = {.tv_sec = 2};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(HP_DISCOVER_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        sendto(s, "HOME?", 5, 0, (struct sockaddr *)&to, sizeof to);
        char buf[32];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n > 5 && memcmp(buf, "HOME ", 5) == 0) {
            buf[n] = 0;
            *port = (uint16_t)atoi(buf + 5);
            inet_ntoa_r(from.sin_addr, host, cap);
            ok = *port != 0;
        }
    }
    close(s);
    return ok;
}

static int connect_to(const char *host, uint16_t port)
{
    char ports[8];
    snprintf(ports, sizeof ports, "%u", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve %s", host);
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {.tv_sec = 5};
        int err = 0;
        socklen_t el = sizeof err;
        if (select(s + 1, NULL, &w, NULL, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(s);
            return -1;
        }
    } else if (r < 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    int yes = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    struct timeval stv = {.tv_sec = 5};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    return s;
}

static void send_hello(void)
{
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
    h.has_reset_reason = true;
    h.reset_reason = (uint8_t)esp_reset_reason();
    h.has_uptime_s = true;
    h.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    h.has_name = g_cfg.name[0] != 0;
    strlcpy(h.name, g_cfg.name, sizeof h.name);
    h.has_idf_version = true;
    strlcpy(h.idf_version, app->idf_ver, sizeof h.idf_version);
    h.has_pending_verify = true;
    h.pending_verify = g_pending_verify;
    static uint8_t buf[256];
    size_t n;
    MSG_BUILD(buf, sizeof buf, &n, HP_MSG_HELLO, 0, ++s_req_id, hp_hello_req_write, &h);
    link_send(buf, n);
}

static void send_ping(void)
{
    hp_ping_req_t p = {.has_time_ms = true, .time_ms = home_time_ms()};
    uint8_t buf[32];
    size_t n;
    MSG_BUILD(buf, sizeof buf, &n, HP_MSG_PING, 0, ++s_req_id, hp_ping_req_write, &p);
    link_send(buf, n);
}

// Handles one message from the server. Returns false to drop the connection.
static bool on_message(const uint8_t *msg, size_t len, bool *hello_done)
{
    hp_header_t h;
    if (!hp_header_read(msg, len, &h)) return true;
    if (h.flags & HP_FLAGS_RESP) {
        if (h.type == HP_MSG_HELLO && !(h.flags & HP_FLAGS_ERR)) {
            hp_hello_resp_t r;
            if (hp_hello_resp_read(&r, msg + HP_HEADER_SIZE, len - HP_HEADER_SIZE) != HP_DECODE_OK) return false;
            if (r.has_time_ms) core_set_time(r.time_ms);
            if (r.has_log_level) g_cfg.log_level = r.log_level;
            uint8_t state = r.has_state ? r.state : HP_ADOPT_STATE_NEW;
            if (state == HP_ADOPT_STATE_BLOCKED) {
                ESP_LOGW(TAG, "the server blocked this device");
                return false;
            }
            g_adopted = state == HP_ADOPT_STATE_ADOPTED;
            *hello_done = true;
            s_online = true;
            core_confirm_firmware();
            cfg_trial_commit();
            core_set_state(HP_LINK_STATE_ONLINE);
            ESP_LOGI(TAG, "online, %s", g_adopted ? "adopted" : "waiting for adoption");
            points_send_state();
            if (g_adopted) points_flush_offline();
        }
        return true;  // PING answers etc.
    }
    if (!*hello_done) return true;
    dispatch_post(TR_TCP, msg, len);
    return true;
}

static void session(const char *host, uint16_t port)
{
    core_set_state(HP_LINK_STATE_SERVER_CONNECTING);
    int s = connect_to(host, port);
    if (s < 0) {
        ESP_LOGW(TAG, "cannot connect to %s:%u", host, port);
        return;
    }
    snprintf(g_server_addr, sizeof g_server_addr, "%s:%u", host, port);
    ESP_LOGI(TAG, "connected to %s", g_server_addr);
    s_sock = s;
    s_rx_len = 0;
    s_reconnect = false;
    bool hello_done = false;
    int64_t last_rx = now_ms(), last_ping = now_ms(), hello_at = now_ms();
    send_hello();

    while (!s_reconnect) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        struct timeval tv = {.tv_sec = 1};
        int sel = select(s + 1, &r, NULL, NULL, &tv);
        if (sel < 0) break;
        if (sel > 0) {
            int n = recv(s, s_rx + s_rx_len, sizeof s_rx - s_rx_len, 0);
            if (n <= 0) break;
            s_rx_len += (size_t)n;
            last_rx = now_ms();
            size_t off = 0;
            for (;;) {
                const uint8_t *m;
                size_t ml, used;
                hp_frame_result_t fr = hp_frame_decode(s_rx + off, s_rx_len - off, MSG_MAX, &m, &ml, &used);
                if (fr == HP_FRAME_NEED_MORE) break;
                off += used;
                if (fr == HP_FRAME_READY && !on_message(m, ml, &hello_done)) {
                    s_reconnect = true;
                    break;
                }
            }
            memmove(s_rx, s_rx + off, s_rx_len - off);
            s_rx_len -= off;
            if (s_rx_len == sizeof s_rx) s_rx_len = 0;  // garbage overflow
        }
        int64_t t = now_ms();
        if (!hello_done && t - hello_at > 10000) {
            ESP_LOGW(TAG, "no HELLO answer");
            break;
        }
        if (hello_done) {
            logfwd_drain();
            if (t - last_rx > 30000 && t - last_ping > 30000) {
                send_ping();
                last_ping = t;
            }
            if (t - last_rx > 75000) {
                ESP_LOGW(TAG, "server silent, reconnecting");
                break;
            }
        }
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_online = false;
    close(s);
    s_sock = -1;
    xSemaphoreGive(s_tx_lock);
    ESP_LOGW(TAG, "disconnected");
}

static void link_task(void *arg)
{
    int backoff = 1;
    for (;;) {
        net_wait_ip(portMAX_DELAY);
        char host[65];
        uint16_t port = g_cfg.server_port ? g_cfg.server_port : HP_TCP_PORT;
        bool found = true;
        if (g_cfg.server_host[0]) strlcpy(host, g_cfg.server_host, sizeof host);
        else {
            core_set_state(HP_LINK_STATE_SERVER_CONNECTING);
            found = discover(host, sizeof host, &port);
            if (!found) ESP_LOGW(TAG, "server not found by discovery");
        }
        int64_t started = now_ms();
        if (found) session(host, port);
        if (net_has_ip()) core_set_state(HP_LINK_STATE_IP_OK);
        if (now_ms() - started > 60000) backoff = 1;  // was connected for a while
        int wait = backoff + (int)(esp_random() % 1000) / 1000;
        vTaskDelay(pdMS_TO_TICKS(wait * 1000 + (esp_random() % 1000)));
        backoff = backoff < 60 ? backoff * 2 : 60;
        if (!g_adopted && !found) backoff = backoff > 10 ? 10 : backoff;
    }
}

void link_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    xTaskCreate(link_task, "home_link", 6144, NULL, 5, NULL);
}
