#include <string.h>

#include "core_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

// Settings live in NVS namespace "cfg" as one blob; "try"/"prev" implement the network trial.

home_cfg_t g_cfg;
static const char *TAG = "cfg";
static esp_timer_handle_t s_trial_timer;

static void defaults(home_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->ip_mode = HP_IP_MODE_DHCP;
    c->server_port = HP_TCP_PORT;
    c->ble_mode = HP_BLE_MODE_ALWAYS;
    c->log_level = HP_LOG_LEVEL_WARN;
}

void cfg_load(void)
{
    defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof g_cfg;
    home_cfg_t tmp;
    if (nvs_get_blob(h, "main", &tmp, &n) == ESP_OK && n == sizeof tmp) g_cfg = tmp;
    else ESP_LOGW(TAG, "no stored settings, using defaults");
    nvs_close(h);
}

static esp_err_t save_blob(const char *key, const home_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, c, sizeof *c);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t cfg_save(void) { return save_blob("main", &g_cfg); }

bool cfg_has_wifi(void) { return g_cfg.ssid[0] != 0; }

esp_err_t cfg_save_trial(const home_cfg_t *prev)
{
    esp_err_t err = cfg_save();
    if (err != ESP_OK || prev->ssid[0] == 0) return err;  // nothing to roll back to
    nvs_handle_t h;
    err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "prev", prev, sizeof *prev);
    if (err == ESP_OK) err = nvs_set_u8(h, "try", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void trial_expired(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    home_cfg_t prev;
    size_t n = sizeof prev;
    if (nvs_get_blob(h, "prev", &prev, &n) == ESP_OK && n == sizeof prev) {
        ESP_LOGW(TAG, "server not reached with the new network settings: restoring the previous ones");
        nvs_set_blob(h, "main", &prev, sizeof prev);
    }
    nvs_erase_key(h, "try");
    nvs_erase_key(h, "prev");
    nvs_commit(h);
    nvs_close(h);
    home_restart("network settings rolled back");
}

void cfg_trial_check_at_boot(void)
{
    nvs_handle_t h;
    uint8_t trying = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "try", &trying);
        nvs_close(h);
    }
    if (!trying) return;
    ESP_LOGI(TAG, "new network settings on trial: 2 minutes to reach the server");
    const esp_timer_create_args_t a = {.callback = trial_expired, .name = "net_trial"};
    esp_timer_create(&a, &s_trial_timer);
    esp_timer_start_once(s_trial_timer, 120ULL * 1000 * 1000);
}

void cfg_trial_commit(void)
{
    if (!s_trial_timer) return;
    esp_timer_stop(s_trial_timer);
    esp_timer_delete(s_trial_timer);
    s_trial_timer = NULL;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "try");
        nvs_erase_key(h, "prev");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "new network settings confirmed");
}
