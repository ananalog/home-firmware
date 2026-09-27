#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "home.h"

// Point ids (see main.c for the table).
enum {
    P_CO2 = 1,
    P_SENSOR_TEMP = 2,
    P_STATUS = 3,
    P_SENSOR_MODEL = 4,
    P_REPORT_INTERVAL = 10,
    P_REPORT_DELTA = 11,
    P_AUTO_CAL = 12,
    P_DISPLAY_MODE = 13,
    P_BRIGHTNESS = 14,
    P_THR_WARN = 15,
    P_THR_ALARM = 16,
    P_LED_ALARM = 17,
    P_CALIBRATE = 20,
    P_SENSOR_RESET = 21,
};

enum { STATUS_PREHEAT = 0, STATUS_OK = 1, STATUS_ERROR = 2 };
enum { DISPLAY_CO2 = 0, DISPLAY_GRAPH = 1, DISPLAY_NIGHT = 2, DISPLAY_OFF = 3 };

typedef struct {
    int report_interval_s;
    int report_delta;
    bool auto_cal;
    int display_mode;
    int brightness;
    int thr_warn;
    int thr_alarm;
    bool led_alarm;
} settings_t;

extern settings_t g_set;
extern i2c_master_bus_handle_t g_i2c;

// sensor.c
void sensor_start(void);
int sensor_ppm(void);          // last filtered value, -1 if none
int sensor_status(void);
int sensor_trend(void);        // -1 falling, 0 flat, 1 rising (10 min)
const uint16_t *sensor_graph(int *n);  // one value per minute, oldest first
int sensor_preheat_left_s(void);
esp_err_t sensor_calibrate(uint16_t ppm);
esp_err_t sensor_factory_reset(void);
esp_err_t sensor_set_auto_calibration(bool on);
void sensor_settings_changed(void);

// ui.c
void ui_start(void);
void ui_event(home_ui_event_t ev, int arg);
void ui_settings_changed(void);
