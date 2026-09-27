// home_core — everything a Home device needs besides its own sensors:
// Wi-Fi (DHCP/static), server discovery and link, BLE setup service, points, OTA with rollback,
// factory reset, log forwarding. A device describes its points and reports values.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "home_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- firmware image descriptor
// Placed right after esp_app_desc_t (section .rodata_custom_desc); the server and the Android app
// read it from the .bin to refuse images built for another model.
typedef struct {
    uint32_t magic;        // HOME_IMAGE_MAGIC
    uint16_t model;        // HP_MODEL_*
    uint16_t hw_rev_mask;  // bit N = hardware revision N supported
    uint8_t proto_major;
    uint8_t proto_minor;
    uint8_t reserved[22];
} home_image_desc_t;

#define HOME_IMAGE_MAGIC 0x454D4F48u  // "HOME"

// Use once in the device's main.c.
#define HOME_IMAGE_DESC(model_, hw_mask_)                                                     \
    const __attribute__((used, section(".rodata_custom_desc"))) home_image_desc_t home_image_desc = { \
        .magic = HOME_IMAGE_MAGIC, .model = (model_), .hw_rev_mask = (hw_mask_),               \
        .proto_major = HP_PROTO_MAJOR, .proto_minor = HP_PROTO_MINOR}

// ---------------------------------------------------------------- points

typedef struct {
    uint8_t id;
    const char *key;
    const char *title;
    uint8_t kind;            // HP_POINT_KIND_*
    uint8_t type;            // HP_POINT_TYPE_*
    const char *unit;        // NULL if none
    float min, max, step;    // NAN if not used
    const char *const *options;  // enum names
    uint8_t n_options;
    uint8_t ui;              // HP_UI_HINT_*
    uint8_t flags;           // HP_POINT_FLAGS_* (HISTORY, READONLY, ADVANCED, PERSIST)
    float thr_warn, thr_alarm;   // NAN if not used
    bool has_arg;            // actions: takes one numeric argument
    float arg_default;
    hp_value_t def;          // default value (settings)
} home_point_t;

// Value helpers
static inline hp_value_t home_f32(float f) { hp_value_t v = {0}; v.has_f = true; v.f = f; return v; }
static inline hp_value_t home_i32(int32_t i) { hp_value_t v = {0}; v.has_i = true; v.i = i; return v; }
static inline hp_value_t home_bool(bool b) { hp_value_t v = {0}; v.has_b = true; v.b = b; return v; }
hp_value_t home_str(const char *s);
float home_num(const hp_value_t *v);  // f, i or b as a number (NAN if none)

// ---------------------------------------------------------------- device callbacks

// A point was changed by the server/phone (or restored from NVS at start). Apply it and return ESP_OK,
// or an error (ESP_ERR_INVALID_ARG → INVALID_VALUE). `applied` may be adjusted (e.g. clamped).
typedef esp_err_t (*home_set_fn)(uint8_t point, hp_value_t *applied);

// An action (kind ACTION) was invoked. `text` (cap bytes) is an optional answer.
typedef esp_err_t (*home_invoke_fn)(uint8_t point, float arg, char *text, size_t cap);

// UI events for the device's screen/LED.
typedef enum {
    HOME_UI_STATE,        // link state changed: arg = home_state()
    HOME_UI_PAIR_CODE,    // show the BLE pairing code: arg = code (0 = hide)
    HOME_UI_OTA,          // OTA progress: arg = percent (0..100), -1 = finished/aborted
    HOME_UI_IDENTIFY,     // blink/show "it's me": arg = seconds
    HOME_UI_RESET_HOLD,   // button held: arg = seconds held (0 = released)
    HOME_UI_REBOOTING,    // about to restart
} home_ui_event_t;

typedef void (*home_ui_fn)(home_ui_event_t ev, int arg);

typedef struct {
    uint16_t model;               // HP_MODEL_*
    uint8_t hw_rev;
    const char *short_name;       // for BLE name / hostname: "co2" → Home-CO2-A1B2, home-co2-a1b2
    const home_point_t *points;
    size_t n_points;
    home_set_fn on_set;
    home_invoke_fn on_invoke;
    home_ui_fn on_ui;             // may be NULL
    bool has_display;             // BLE pairing code on the screen (else: button window)
    int button_gpio;              // -1 if none; active low. Short press: open BLE window; 5 s: reset settings; 15 s: full reset
} home_device_t;

// ---------------------------------------------------------------- API

// Starts everything (NVS, Wi-Fi, BLE, link). Calls on_set for every PERSIST point with its stored or default value.
esp_err_t home_start(const home_device_t *dev);

// Stores a new value and sends it (REPORT). Safe from any task. Offline: history points are buffered.
void home_report(uint8_t point, hp_value_t v);
// Sends several already stored values in one REPORT.
void home_flush_reports(void);
// Current value of a point (NULL if unknown).
const hp_value_t *home_value(uint8_t point);
// Sends an EVENT (threshold crossed, sensor error, ...).
void home_event(uint8_t kind, uint8_t point, const hp_value_t *value, const char *text);

// State of the connection to the server (HP_LINK_STATE_*).
uint8_t home_state(void);
bool home_adopted(void);
// Unix time in ms from the server (0 if unknown yet).
uint64_t home_time_ms(void);
// "a1b2c3d4e5f6"
const char *home_device_id(void);
const char *home_device_name(void);
bool home_ble_connected(void);

void home_restart(const char *why);
void home_factory_reset(uint8_t mode);  // HP_RESET_MODE_*, restarts

#ifdef __cplusplus
}
#endif
