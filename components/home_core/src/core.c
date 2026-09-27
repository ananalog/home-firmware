#include <string.h>

#include "core_internal.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = TAG_CORE;

const home_device_t *g_dev;
char g_device_id[13];
volatile uint8_t g_link_state = HP_LINK_STATE_NO_CONFIG;
volatile bool g_adopted;
volatile bool g_pending_verify;
static int64_t s_time_offset_ms;  // server time - uptime
static bool s_time_known;
static esp_timer_handle_t s_verify_timer;

hp_value_t home_str(const char *s)
{
    hp_value_t v = {0};
    v.has_s = true;
    strlcpy(v.s, s ? s : "", sizeof v.s);
    return v;
}

float home_num(const hp_value_t *v)
{
    if (!v) return NAN;
    if (v->has_f) return v->f;
    if (v->has_i) return (float)v->i;
    if (v->has_b) return v->b ? 1.0f : 0.0f;
    return NAN;
}

uint8_t home_state(void) { return g_link_state; }
bool home_adopted(void) { return g_adopted; }
const char *home_device_id(void) { return g_device_id; }
const char *home_device_name(void) { return g_cfg.name[0] ? g_cfg.name : g_dev->short_name; }
bool home_ble_connected(void) { return ble_is_connected(); }

uint64_t home_time_ms(void) { return s_time_known ? (uint64_t)(s_time_offset_ms + esp_timer_get_time() / 1000) : 0; }

void core_set_time(uint64_t server_ms)
{
    s_time_offset_ms = (int64_t)server_ms - esp_timer_get_time() / 1000;
    s_time_known = server_ms > 0;
}

void core_ui(home_ui_event_t ev, int arg)
{
    if (g_dev && g_dev->on_ui) g_dev->on_ui(ev, arg);
}

void core_set_state(uint8_t s)
{
    if (g_link_state == s) return;
    g_link_state = s;
    core_ui(HOME_UI_STATE, s);
    ble_update_advertising();
}

uint8_t core_boot_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return HP_BOOT_PARTITION_UNKNOWN;
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY: return HP_BOOT_PARTITION_FACTORY;
    case ESP_PARTITION_SUBTYPE_APP_OTA_0: return HP_BOOT_PARTITION_OTA0;
    case ESP_PARTITION_SUBTYPE_APP_OTA_1: return HP_BOOT_PARTITION_OTA1;
    default: return HP_BOOT_PARTITION_UNKNOWN;
    }
}

static void verify_expired(void *arg)
{
    ESP_LOGE(TAG, "new firmware was not confirmed in time: rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void core_confirm_firmware(void)
{
    if (!g_pending_verify) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        g_pending_verify = false;
        if (s_verify_timer) esp_timer_stop(s_verify_timer);
        ESP_LOGI(TAG, "firmware %s confirmed", esp_app_get_description()->version);
    }
}

void home_restart(const char *why)
{
    ESP_LOGW(TAG, "restart: %s", why);
    core_ui(HOME_UI_REBOOTING, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void home_factory_reset(uint8_t mode)
{
    ESP_LOGW(TAG, "factory reset, mode %u", mode);
    if (mode == HP_RESET_MODE_SETTINGS || mode == HP_RESET_MODE_ALL) {
        nvs_flash_deinit();
        nvs_flash_erase_partition("nvs");
    }
    if (mode == HP_RESET_MODE_FIRMWARE || mode == HP_RESET_MODE_ALL) {
        const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (f) esp_ota_set_boot_partition(f);
        else ESP_LOGE(TAG, "no factory partition");
    }
    home_restart("factory reset");
}

esp_err_t home_start(const home_device_t *dev)
{
    g_dev = dev;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(g_device_id, sizeof g_device_id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s, device %s, model %u, boot %u", app->project_name, app->version, g_device_id, dev->model, core_boot_partition());

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        g_pending_verify = true;
        const esp_timer_create_args_t a = {.callback = verify_expired, .name = "verify"};
        esp_timer_create(&a, &s_verify_timer);
        esp_timer_start_once(s_verify_timer, 5ULL * 60 * 1000 * 1000);
        ESP_LOGW(TAG, "firmware pending verification (5 min to reach the server or the phone)");
    }

    cfg_load();
    if (!g_cfg.hostname[0])
        snprintf(g_cfg.hostname, sizeof g_cfg.hostname, "home-%s-%s", dev->short_name, g_device_id + 8);
    cfg_trial_check_at_boot();

    points_init();
    ota_init();
    logfwd_start();
    dispatch_start();
    net_start();
    ble_start();
    link_start();
    if (dev->button_gpio >= 0) button_start();
    core_set_state(cfg_has_wifi() ? HP_LINK_STATE_WIFI_CONNECTING : HP_LINK_STATE_NO_CONFIG);
    core_ui(HOME_UI_STATE, g_link_state);
    return ESP_OK;
}
