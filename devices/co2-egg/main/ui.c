#include <stdio.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"

// 72x40 OLED + the IO8 LED. Everything ASCII: the tiny screen has no Cyrillic font.

static const char *TAG = "ui";
static ssd1306_handle_t s_oled;
static volatile int s_pair_code;
static volatile int s_ota = -1;
static volatile int s_reset_hold;
static volatile int64_t s_identify_until;
static volatile bool s_rebooting;
static volatile bool s_dirty = true;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void led(bool on) { gpio_set_level(BOARD_LED_GPIO, BOARD_LED_ACTIVE_LOW ? !on : on); }

void ui_event(home_ui_event_t ev, int arg)
{
    switch (ev) {
    case HOME_UI_PAIR_CODE: s_pair_code = arg; break;
    case HOME_UI_OTA: s_ota = arg; break;
    case HOME_UI_IDENTIFY: s_identify_until = now_ms() + arg * 1000LL; break;
    case HOME_UI_RESET_HOLD: s_reset_hold = arg; break;
    case HOME_UI_REBOOTING: s_rebooting = true; break;
    default: break;
    }
    s_dirty = true;
}

void ui_settings_changed(void) { s_dirty = true; }

static void center_text(int y, const char *s) { ssd1306_text(s_oled, (72 - ssd1306_text_width(s)) / 2, y, s); }

static void center_big(int y, const char *s) { ssd1306_big(s_oled, (72 - ssd1306_big_width_of(s)) / 2, y, s); }

static const char *link_text(void)
{
    switch (home_state()) {
    case HP_LINK_STATE_NO_CONFIG: return "SETUP";
    case HP_LINK_STATE_WIFI_CONNECTING: return "WIFI..";
    case HP_LINK_STATE_IP_OK:
    case HP_LINK_STATE_SERVER_CONNECTING: return "SRV..";
    default: return home_adopted() ? "" : "NEW";
    }
}

static void draw_status_line(int ppm)
{
    // bottom line (y=32): link state or "ppm" + trend, BLE mark on the right
    char left[16];
    int pre = sensor_preheat_left_s();
    const char *lt = link_text();
    if (lt[0]) snprintf(left, sizeof left, "%s", lt);
    else if (pre > 0 && ppm > 0) snprintf(left, sizeof left, "warm %d", pre);
    else snprintf(left, sizeof left, "ppm %s", sensor_trend() > 0 ? "^" : sensor_trend() < 0 ? "v" : "");
    ssd1306_text(s_oled, 0, 32, left);
    if (home_ble_connected()) ssd1306_text(s_oled, 66, 32, "B");
}

static void draw_graph(void)
{
    int n;
    const uint16_t *g = sensor_graph(&n);
    if (n < 2) return;
    int lo = 5000, hi = 0;
    for (int i = 0; i < n; i++) {
        if (g[i] < lo) lo = g[i];
        if (g[i] > hi) hi = g[i];
    }
    if (hi - lo < 100) hi = lo + 100;
    for (int i = 0; i < n; i++) {
        int x = 72 - n + i;
        int h = 1 + (g[i] - lo) * 22 / (hi - lo);
        ssd1306_fill_rect(s_oled, x, 23 - h, 1, h, true);
    }
}

static void draw(void)
{
    ssd1306_clear(s_oled);
    char buf[16];
    int ppm = sensor_ppm();
    if (s_rebooting) {
        center_text(16, "restart");
    } else if (s_reset_hold > 0) {
        center_text(0, s_reset_hold >= 15 ? "FULL RESET" : s_reset_hold >= 5 ? "RESET" : "hold...");
        snprintf(buf, sizeof buf, "%d", s_reset_hold);
        center_big(10, buf);
        ssd1306_text(s_oled, 0, 32, "5s cfg 15s all");
    } else if (s_pair_code) {
        center_text(0, "BLE code");
        snprintf(buf, sizeof buf, "%04d", s_pair_code);
        center_big(10, buf);
    } else if (s_ota >= 0) {
        center_text(0, "UPDATE");
        snprintf(buf, sizeof buf, "%d", s_ota);
        center_big(9, buf);
        ssd1306_fill_rect(s_oled, 0, 36, s_ota * 72 / 100, 4, true);
    } else if (home_state() == HP_LINK_STATE_NO_CONFIG && ppm < 0) {
        center_text(4, "Home CO2");
        center_text(16, "setup via");
        center_text(28, "Android app");
    } else if (g_set.display_mode == DISPLAY_GRAPH) {
        draw_graph();
        if (ppm >= 0) snprintf(buf, sizeof buf, "%d ppm", ppm);
        else snprintf(buf, sizeof buf, "--");
        ssd1306_text(s_oled, 0, 32, buf);
        const char *lt = link_text();
        if (lt[0]) ssd1306_text(s_oled, 72 - ssd1306_text_width(lt), 32, lt);
    } else {
        if (sensor_status() == STATUS_ERROR) center_text(8, "sensor?");
        else {
            snprintf(buf, sizeof buf, ppm >= 0 ? "%d" : "----", ppm);
            center_big(0, buf);
        }
        draw_status_line(ppm);
    }
    // Identify: blink the whole screen.
    if (now_ms() < s_identify_until && (now_ms() / 250) % 2) ssd1306_invert_rect(s_oled, 0, 0, 72, 40);
    ssd1306_flush(s_oled);
}

static void task(void *arg)
{
    int tick = 0;
    int last_brightness = -1;
    bool on = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(125));
        tick++;
        int64_t t = now_ms();
        int ppm = sensor_ppm();
        bool identify = t < s_identify_until;

        // LED: identify fast blink; alarm fast, warning slow (unless disabled or night/off).
        bool l = false;
        if (identify) l = (tick / 2) % 2;
        else if (g_set.led_alarm && g_set.display_mode < DISPLAY_NIGHT && ppm >= 0 && sensor_status() == STATUS_OK) {
            if (ppm >= g_set.thr_alarm) l = (tick / 2) % 2;
            else if (ppm >= g_set.thr_warn) l = (tick % 16) < 2;
        }
        led(l);

        if (!s_oled) continue;
        bool busy = s_pair_code || s_ota >= 0 || s_reset_hold || identify || s_rebooting || home_state() == HP_LINK_STATE_NO_CONFIG;
        bool want_on = busy || g_set.display_mode != DISPLAY_OFF;
        if (want_on != on) {
            ssd1306_power(s_oled, want_on);
            on = want_on;
        }
        int brightness = busy ? 100 : g_set.display_mode == DISPLAY_NIGHT ? 1 : g_set.brightness;
        if (brightness != last_brightness) {
            ssd1306_contrast(s_oled, (uint8_t)(brightness * 255 / 100));
            last_brightness = brightness;
        }
        if (on && (s_dirty || identify || tick % 8 == 0)) {
            s_dirty = false;
            draw();
        }
    }
}

void ui_start(void)
{
    gpio_config_t c = {.pin_bit_mask = 1ULL << BOARD_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&c);
    led(false);
    ssd1306_config_t cfg = SSD1306_72X40;
    cfg.address = BOARD_OLED_ADDR;
    esp_err_t err = ssd1306_init(g_i2c, &cfg, &s_oled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no OLED (%s)", esp_err_to_name(err));
        s_oled = NULL;
    }
    xTaskCreate(task, "co2_ui", 3072, NULL, 3, NULL);
}
