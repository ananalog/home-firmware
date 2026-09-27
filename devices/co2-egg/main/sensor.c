#include <stdlib.h>
#include <string.h>

#include "acd1200.h"
#include "app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Polls the ACD1200 every 5 s, filters (median of 3), reports on change/interval, raises threshold events.

static const char *TAG = "sensor";
static acd1200_handle_t s_acd;
static SemaphoreHandle_t s_lock;
static volatile int s_ppm = -1;
static volatile int s_status = STATUS_PREHEAT;
static int s_hist3[3], s_hist_n;
static uint16_t s_graph[72];  // one value per minute, 72 = screen width
static int s_graph_n;
static int s_trend;
static bool s_want_auto_cal = true;
static int64_t s_start_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

int sensor_ppm(void) { return s_ppm; }
int sensor_status(void) { return s_status; }
int sensor_trend(void) { return s_trend; }

const uint16_t *sensor_graph(int *n)
{
    *n = s_graph_n;
    return s_graph;
}

int sensor_preheat_left_s(void)
{
    int64_t left = ACD1200_PREHEAT_MS - (now_ms() - s_start_ms);
    return left > 0 ? (int)(left / 1000) : 0;
}

static esp_err_t with_sensor(esp_err_t (*fn)(acd1200_handle_t, void *), void *arg)
{
    if (!s_acd) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = fn(s_acd, arg);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t do_cal(acd1200_handle_t h, void *a) { return acd1200_calibrate(h, *(uint16_t *)a); }
static esp_err_t do_reset(acd1200_handle_t h, void *a) { return acd1200_factory_reset(h); }
static esp_err_t do_abc(acd1200_handle_t h, void *a) { return acd1200_set_auto_calibration(h, *(bool *)a); }

esp_err_t sensor_calibrate(uint16_t ppm) { return with_sensor(do_cal, &ppm); }
esp_err_t sensor_factory_reset(void) { return with_sensor(do_reset, NULL); }

esp_err_t sensor_set_auto_calibration(bool on)
{
    s_want_auto_cal = on;
    return s_acd ? with_sensor(do_abc, &on) : ESP_OK;  // applied after init otherwise
}

void sensor_settings_changed(void) {}

static int median3(int a, int b, int c)
{
    if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
    if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
    return c;
}

static bool try_init(void)
{
    if (acd1200_init(g_i2c, &s_acd) != ESP_OK) return false;
    char ver[11] = "", sn[11] = "";
    acd1200_version(s_acd, ver);
    acd1200_serial(s_acd, sn);
    char model[64];
    snprintf(model, sizeof model, "ACD1200 %s %s", ver, sn);
    home_report(P_SENSOR_MODEL, home_str(model));
    acd1200_set_auto_calibration(s_acd, s_want_auto_cal);
    ESP_LOGI(TAG, "%s", model);
    return true;
}

static void set_status(int st)
{
    if (s_status == st) return;
    s_status = st;
    home_report(P_STATUS, home_i32(st));
}

static void task(void *arg)
{
    s_start_ms = now_ms();
    int last_sent = -1000;
    int64_t last_sent_ms = 0, last_graph_ms = 0, last_init_ms = 0;
    int errors = 0, level = 0;
    home_report(P_STATUS, home_i32(STATUS_PREHEAT));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t t = now_ms();
        if (!s_acd) {
            if (t - last_init_ms > 10000) {
                last_init_ms = t;
                if (!try_init()) {
                    set_status(STATUS_ERROR);
                    continue;
                }
            } else continue;
        }
        acd1200_reading_t r;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = acd1200_read(s_acd, &r);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK || r.ppm < 300 || r.ppm > 10000) {
            errors++;
            ESP_LOGW(TAG, "read failed (%s), %d in a row", esp_err_to_name(err), errors);
            if (errors == 10) {
                set_status(STATUS_ERROR);
                home_event(HP_EVENT_KIND_SENSOR_ERROR, P_CO2, NULL, "CO2 sensor does not answer");
            }
            continue;
        }
        errors = 0;
        s_hist3[s_hist_n % 3] = (int)r.ppm;
        s_hist_n++;
        int ppm = s_hist_n >= 3 ? median3(s_hist3[0], s_hist3[1], s_hist3[2]) : (int)r.ppm;
        s_ppm = ppm;
        bool preheat = sensor_preheat_left_s() > 0;
        set_status(preheat ? STATUS_PREHEAT : STATUS_OK);

        if (t - last_graph_ms >= 60000 || s_graph_n == 0) {
            last_graph_ms = t;
            if (s_graph_n == 72) memmove(s_graph, s_graph + 1, sizeof s_graph - sizeof s_graph[0]);
            else s_graph_n++;
            s_graph[s_graph_n - 1] = (uint16_t)ppm;
            int ref = s_graph_n > 10 ? s_graph[s_graph_n - 11] : s_graph[0];
            s_trend = ppm > ref + 30 ? 1 : ppm < ref - 30 ? -1 : 0;
        }

        if (abs(ppm - last_sent) >= g_set.report_delta || t - last_sent_ms >= g_set.report_interval_s * 1000LL) {
            last_sent = ppm;
            last_sent_ms = t;
            home_report(P_CO2, home_f32((float)ppm));
            home_report(P_SENSOR_TEMP, home_i32(r.temp_raw));
        }

        if (!preheat) {  // threshold events; stepping down needs 50 ppm hysteresis
            int raw = ppm >= g_set.thr_alarm ? 2 : ppm >= g_set.thr_warn ? 1 : 0;
            int nl = level;
            if (raw > level) nl = raw;
            else if (raw < level) nl = ppm < (level == 2 ? g_set.thr_alarm : g_set.thr_warn) - 50 ? raw : level;
            if (nl != level) {
                char text[64];
                if (nl == 0) snprintf(text, sizeof text, "CO2 снова в норме");
                else if (nl > level) snprintf(text, sizeof text, "CO2 выше %d ppm", nl == 2 ? g_set.thr_alarm : g_set.thr_warn);
                else snprintf(text, sizeof text, "CO2 ниже %d ppm", g_set.thr_alarm);
                hp_value_t v = home_f32((float)ppm);
                home_event(HP_EVENT_KIND_THRESHOLD, P_CO2, &v, text);
                level = nl;
            }
        }
    }
}

void sensor_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    try_init();
    xTaskCreate(task, "co2_sensor", 4096, NULL, 5, NULL);
}
