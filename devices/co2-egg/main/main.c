// CO2 sensor: ESP32-C3 "Egg" (0.42" OLED) + ASAIR ACD1200.
#include <math.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "esp_log.h"

#pragma GCC diagnostic ignored "-Woverride-init"  // P() sets defaults that entries override

HOME_IMAGE_DESC(HP_MODEL_CO2_EGG, 0xFFFF);

static const char *TAG = "co2";
settings_t g_set = {30, 25, true, DISPLAY_CO2, 60, 800, 1200, true};
i2c_master_bus_handle_t g_i2c;

static const char *const status_opts[] = {"preheat", "ok", "sensor_error"};
static const char *const display_opts[] = {"co2", "co2_graph", "night", "off"};

#define NONE NAN
#define P(...) {.min = NONE, .max = NONE, .step = NONE, .thr_warn = NONE, .thr_alarm = NONE, .arg_default = NONE, __VA_ARGS__}

// Not const: thresholds follow the thr_warn/thr_alarm settings (sent with DESCRIBE).
static home_point_t s_points[] = {
    P(.id = P_CO2, .key = "co2", .title = "CO2", .kind = HP_POINT_KIND_SENSOR, .type = HP_POINT_TYPE_F32, .unit = "ppm",
      .min = 400, .max = 5000, .step = 1, .ui = HP_UI_HINT_GAUGE, .flags = HP_POINT_FLAGS_HISTORY, .thr_warn = 800, .thr_alarm = 1200),
    P(.id = P_SENSOR_TEMP, .key = "sensor_temp_raw", .title = "Датчик: температура (сырое)", .kind = HP_POINT_KIND_SENSOR,
      .type = HP_POINT_TYPE_I32, .ui = HP_UI_HINT_TEXT, .flags = HP_POINT_FLAGS_ADVANCED),
    P(.id = P_STATUS, .key = "status", .title = "Состояние", .kind = HP_POINT_KIND_SENSOR, .type = HP_POINT_TYPE_ENUM,
      .options = status_opts, .n_options = 3, .ui = HP_UI_HINT_TEXT),
    P(.id = P_SENSOR_MODEL, .key = "sensor_model", .title = "Датчик", .kind = HP_POINT_KIND_SENSOR, .type = HP_POINT_TYPE_STR,
      .ui = HP_UI_HINT_TEXT, .flags = HP_POINT_FLAGS_ADVANCED),
    P(.id = P_REPORT_INTERVAL, .key = "report_interval", .title = "Интервал отправки", .kind = HP_POINT_KIND_SETTING,
      .type = HP_POINT_TYPE_I32, .unit = "s", .min = 5, .max = 600, .step = 5, .ui = HP_UI_HINT_SLIDER,
      .flags = HP_POINT_FLAGS_PERSIST | HP_POINT_FLAGS_ADVANCED, .def = {.has_i = true, .i = 30}),
    P(.id = P_REPORT_DELTA, .key = "report_delta", .title = "Порог изменения", .kind = HP_POINT_KIND_SETTING,
      .type = HP_POINT_TYPE_I32, .unit = "ppm", .min = 5, .max = 500, .step = 5, .ui = HP_UI_HINT_SLIDER,
      .flags = HP_POINT_FLAGS_PERSIST | HP_POINT_FLAGS_ADVANCED, .def = {.has_i = true, .i = 25}),
    P(.id = P_AUTO_CAL, .key = "auto_calibration", .title = "Автокалибровка", .kind = HP_POINT_KIND_SETTING,
      .type = HP_POINT_TYPE_BOOL, .ui = HP_UI_HINT_SWITCH, .flags = HP_POINT_FLAGS_PERSIST, .def = {.has_b = true, .b = true}),
    P(.id = P_DISPLAY_MODE, .key = "display_mode", .title = "Экран", .kind = HP_POINT_KIND_SETTING, .type = HP_POINT_TYPE_ENUM,
      .options = display_opts, .n_options = 4, .ui = HP_UI_HINT_SELECT, .flags = HP_POINT_FLAGS_PERSIST, .def = {.has_i = true, .i = 0}),
    P(.id = P_BRIGHTNESS, .key = "display_brightness", .title = "Яркость экрана", .kind = HP_POINT_KIND_SETTING,
      .type = HP_POINT_TYPE_I32, .unit = "%", .min = 0, .max = 100, .step = 5, .ui = HP_UI_HINT_SLIDER,
      .flags = HP_POINT_FLAGS_PERSIST, .def = {.has_i = true, .i = 60}),
    P(.id = P_THR_WARN, .key = "thr_warn", .title = "Порог «внимание»", .kind = HP_POINT_KIND_SETTING, .type = HP_POINT_TYPE_I32,
      .unit = "ppm", .min = 400, .max = 5000, .step = 50, .ui = HP_UI_HINT_SLIDER,
      .flags = HP_POINT_FLAGS_PERSIST | HP_POINT_FLAGS_ADVANCED, .def = {.has_i = true, .i = 800}),
    P(.id = P_THR_ALARM, .key = "thr_alarm", .title = "Порог «тревога»", .kind = HP_POINT_KIND_SETTING, .type = HP_POINT_TYPE_I32,
      .unit = "ppm", .min = 400, .max = 5000, .step = 50, .ui = HP_UI_HINT_SLIDER,
      .flags = HP_POINT_FLAGS_PERSIST | HP_POINT_FLAGS_ADVANCED, .def = {.has_i = true, .i = 1200}),
    P(.id = P_LED_ALARM, .key = "led_alarm", .title = "Мигать при тревоге", .kind = HP_POINT_KIND_SETTING,
      .type = HP_POINT_TYPE_BOOL, .ui = HP_UI_HINT_SWITCH, .flags = HP_POINT_FLAGS_PERSIST, .def = {.has_b = true, .b = true}),
    P(.id = P_CALIBRATE, .key = "calibrate", .title = "Калибровка (на свежем воздухе 20 мин)", .kind = HP_POINT_KIND_ACTION,
      .type = HP_POINT_TYPE_F32, .unit = "ppm", .min = 400, .max = 5000, .step = 1, .ui = HP_UI_HINT_BUTTON, .has_arg = true,
      .arg_default = 420),
    P(.id = P_SENSOR_RESET, .key = "sensor_reset", .title = "Сброс датчика к заводским", .kind = HP_POINT_KIND_ACTION,
      .type = HP_POINT_TYPE_BOOL, .ui = HP_UI_HINT_BUTTON, .flags = HP_POINT_FLAGS_ADVANCED),
};

static home_point_t *point(uint8_t id)
{
    for (size_t i = 0; i < sizeof s_points / sizeof s_points[0]; i++)
        if (s_points[i].id == id) return &s_points[i];
    return NULL;
}

static esp_err_t on_set(uint8_t id, hp_value_t *v)
{
    int n = (int)home_num(v);
    switch (id) {
    case P_REPORT_INTERVAL: g_set.report_interval_s = n; break;
    case P_REPORT_DELTA: g_set.report_delta = n; break;
    case P_AUTO_CAL:
        g_set.auto_cal = v->b;
        sensor_set_auto_calibration(v->b);
        break;
    case P_DISPLAY_MODE: g_set.display_mode = n; break;
    case P_BRIGHTNESS: g_set.brightness = n; break;
    case P_THR_WARN:
        if (n >= g_set.thr_alarm) return ESP_ERR_INVALID_ARG;
        g_set.thr_warn = n;
        point(P_CO2)->thr_warn = (float)n;
        break;
    case P_THR_ALARM:
        if (n <= g_set.thr_warn) return ESP_ERR_INVALID_ARG;
        g_set.thr_alarm = n;
        point(P_CO2)->thr_alarm = (float)n;
        break;
    case P_LED_ALARM: g_set.led_alarm = v->b; break;
    default: return ESP_ERR_NOT_FOUND;
    }
    sensor_settings_changed();
    ui_settings_changed();
    return ESP_OK;
}

static esp_err_t on_invoke(uint8_t id, float arg, char *text, size_t cap)
{
    esp_err_t err;
    switch (id) {
    case P_CALIBRATE:
        if (arg < 400 || arg > 5000) return ESP_ERR_INVALID_ARG;
        err = sensor_calibrate((uint16_t)arg);
        if (err == ESP_OK) snprintf(text, cap, "calibrated to %d ppm", (int)arg);
        return err;
    case P_SENSOR_RESET:
        err = sensor_factory_reset();
        if (err == ESP_OK) snprintf(text, cap, "sensor reset");
        return err;
    default: return ESP_ERR_NOT_FOUND;
    }
}

static const home_device_t s_device = {
    .model = HP_MODEL_CO2_EGG,
    .hw_rev = 1,
    .short_name = "co2",
    .points = s_points,
    .n_points = sizeof s_points / sizeof s_points[0],
    .on_set = on_set,
    .on_invoke = on_invoke,
    .on_ui = ui_event,
    .has_display = true,
    .button_gpio = BOARD_BUTTON_GPIO,
};

void app_main(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &g_i2c));
    ui_start();       // screen first: it shows the BLE code and setup hints
    home_start(&s_device);
    sensor_start();
    ESP_LOGI(TAG, "started");
}
