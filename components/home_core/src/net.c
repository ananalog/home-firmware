#include <string.h>

#include "core_internal.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "net";
static esp_netif_t *s_netif;
static EventGroupHandle_t s_events;
static TimerHandle_t s_retry;
static int s_attempt;
static volatile bool s_scanning;
#define BIT_IP BIT0

static void retry_cb(TimerHandle_t t)
{
    if (cfg_has_wifi() && !s_scanning) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (cfg_has_wifi()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_IP);
        if (!cfg_has_wifi()) {
            core_set_state(HP_LINK_STATE_NO_CONFIG);
            return;
        }
        core_set_state(HP_LINK_STATE_WIFI_CONNECTING);
        wifi_event_sta_disconnected_t *d = data;
        s_attempt++;
        int delay_s = s_attempt < 5 ? 2 : s_attempt < 20 ? 10 : 30;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %d s", d->reason, delay_s);
        xTimerChangePeriod(s_retry, pdMS_TO_TICKS(delay_s * 1000), 0);
        xTimerStart(s_retry, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_attempt = 0;
        xEventGroupSetBits(s_events, BIT_IP);
        core_set_state(HP_LINK_STATE_IP_OK);
    }
}

static void apply_ip_config(void)
{
    esp_netif_set_hostname(s_netif, g_cfg.hostname);
    if (g_cfg.ip_mode != HP_IP_MODE_STATIC) {
        esp_netif_dhcpc_start(s_netif);
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = {.ip.addr = g_cfg.ip, .netmask.addr = g_cfg.mask, .gw.addr = g_cfg.gw};
    esp_netif_set_ip_info(s_netif, &ip);
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
    dns.ip.u_addr.ip4.addr = g_cfg.dns1 ? g_cfg.dns1 : g_cfg.gw;
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (g_cfg.dns2) {
        dns.ip.u_addr.ip4.addr = g_cfg.dns2;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns);
    }
    ESP_LOGI(TAG, "static ip " IPSTR, IP2STR(&ip.ip));
}

void net_start(void)
{
    s_events = xEventGroupCreate();
    s_retry = xTimerCreate("wifi_retry", pdMS_TO_TICKS(2000), pdFALSE, NULL, retry_cb);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, g_cfg.pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    apply_ip_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // required for Wi-Fi/BLE coexistence
    if (!cfg_has_wifi()) ESP_LOGW(TAG, "no Wi-Fi settings: set them up over BLE");
}

bool net_wait_ip(TickType_t timeout) { return xEventGroupWaitBits(s_events, BIT_IP, pdFALSE, pdTRUE, timeout) & BIT_IP; }

bool net_has_ip(void) { return xEventGroupGetBits(s_events) & BIT_IP; }

static uint8_t auth_of(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return HP_WIFI_AUTH_OPEN;
    case WIFI_AUTH_WEP: return HP_WIFI_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK: return HP_WIFI_AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK: return HP_WIFI_AUTH_WPA2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK: return HP_WIFI_AUTH_WPA3;
    default: return HP_WIFI_AUTH_OTHER;
    }
}

esp_err_t net_scan(hp_wifi_scan_resp_t *out)
{
    memset(out, 0, sizeof *out);
    s_scanning = true;
    xTimerStop(s_retry, 0);
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_ERR_WIFI_STATE) {  // busy connecting: pause and scan
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(NULL, true);
    }
    s_scanning = false;
    if (err != ESP_OK) {
        if (cfg_has_wifi() && !net_has_ip()) esp_wifi_connect();
        return err;
    }
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    for (uint16_t i = 0; i < n && out->networks_count < 20; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;  // one entry per SSID (strongest first)
        for (size_t j = 0; j < out->networks_count; j++)
            if (strcmp(out->networks[j].ssid, (char *)recs[i].ssid) == 0) dup = true;
        if (dup) continue;
        hp_wifi_net_t *w = &out->networks[out->networks_count++];
        w->has_ssid = true;
        strlcpy(w->ssid, (char *)recs[i].ssid, sizeof w->ssid);
        w->has_rssi = true;
        w->rssi = recs[i].rssi;
        w->has_auth = true;
        w->auth = auth_of(recs[i].authmode);
        w->has_channel = true;
        w->channel = recs[i].primary;
    }
    if (cfg_has_wifi() && !net_has_ip()) esp_wifi_connect();
    return ESP_OK;
}

void net_status(hp_net_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->has_state = true;
    out->state = g_link_state;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        hp_net_config_t *c = &out->current;
        out->has_current = true;
        c->has_ip_mode = true;
        c->ip_mode = g_cfg.ip_mode;
        c->has_ip = c->has_mask = c->has_gw = true;
        c->ip = ip.ip.addr;
        c->mask = ip.netmask.addr;
        c->gw = ip.gw.addr;
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            c->has_dns1 = true;
            c->dns1 = dns.ip.u_addr.ip4.addr;
        }
        c->has_hostname = true;
        strlcpy(c->hostname, g_cfg.hostname, sizeof c->hostname);
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->has_rssi = true;
        out->rssi = ap.rssi;
        out->has_ssid = true;
        strlcpy(out->ssid, (char *)ap.ssid, sizeof out->ssid);
    } else if (g_cfg.ssid[0]) {
        out->has_ssid = true;
        strlcpy(out->ssid, g_cfg.ssid, sizeof out->ssid);
    }
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    out->has_mac = true;
    snprintf(out->mac, sizeof out->mac, MACSTR, MAC2STR(mac));
    for (char *p = out->mac; *p; p++)
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
}
