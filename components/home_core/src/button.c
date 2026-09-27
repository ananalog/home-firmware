#include "core_internal.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

// Button (active low): short press — open the BLE setup window and identify;
// hold 5 s — reset settings; hold 15 s — full reset (settings + factory firmware).

static const char *TAG = "button";

static void button_task(void *arg)
{
    int gpio = g_dev->button_gpio;
    int held_ms = 0;
    int reported = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool pressed = gpio_get_level(gpio) == 0;
        if (pressed) {
            held_ms += 50;
            int s = held_ms / 1000;
            if (s >= 2 && s != reported) {
                reported = s;
                core_ui(HOME_UI_RESET_HOLD, s);
            }
            continue;
        }
        if (held_ms == 0) continue;
        int s = held_ms / 1000;
        held_ms = 0;
        reported = 0;
        if (s >= 15) home_factory_reset(HP_RESET_MODE_ALL);
        else if (s >= 5) home_factory_reset(HP_RESET_MODE_SETTINGS);
        else {
            if (s >= 2) core_ui(HOME_UI_RESET_HOLD, 0);  // released early: cancelled
            ESP_LOGI(TAG, "short press: BLE setup window open");
            ble_open_window();
            core_ui(HOME_UI_IDENTIFY, 2);
        }
    }
}

void button_start(void)
{
    gpio_config_t c = {.pin_bit_mask = 1ULL << g_dev->button_gpio, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&c);
    xTaskCreate(button_task, "home_button", 3072, NULL, 3, NULL);
}
