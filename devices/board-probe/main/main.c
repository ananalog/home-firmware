// Board probe: checks a new board before the real firmware. Flash over USB and watch the monitor:
//   scripts/fw-build.sh board-probe && scripts/fw-flash-usb.sh board-probe --app-only && scripts/fw-monitor.sh
// Prints chip/flash info, scans I2C on candidate pin pairs, shows the result on the OLED,
// blinks the LED on GPIO8 and reports BOOT button presses (GPIO9).
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"

static const char *TAG = "probe";

static int scan(int sda, int scl, uint8_t *found, int max, i2c_master_bus_handle_t *keep)
{
    i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = sda, .scl_io_num = scl, .clk_source = I2C_CLK_SRC_DEFAULT,
                                  .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return 0;
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78 && n < max; a++)
        if (i2c_master_probe(bus, a, 20) == ESP_OK) found[n++] = a;
    if (keep && n) *keep = bus;
    else i2c_del_master_bus(bus);
    return n;
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    ESP_LOGI(TAG, "chip ESP32-C3 rev %d.%d, %d core(s), flash %lu MB, MAC " MACSTR,
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned long)(flash >> 20), MAC2STR(mac));

    const int pairs[][2] = {{5, 6}, {8, 9}, {4, 5}, {6, 7}};
    i2c_master_bus_handle_t oled_bus = NULL;
    char line[40] = "no i2c";
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        uint8_t found[16];
        i2c_master_bus_handle_t keep = NULL;
        int n = scan(pairs[i][0], pairs[i][1], found, 16, &keep);
        char list[64] = "";
        for (int k = 0; k < n; k++) snprintf(list + strlen(list), sizeof list - strlen(list), " 0x%02X", found[k]);
        ESP_LOGI(TAG, "I2C SDA=%d SCL=%d:%s", pairs[i][0], pairs[i][1], n ? list : " nothing");
        bool has_oled = false;
        for (int k = 0; k < n; k++) has_oled |= found[k] == 0x3C || found[k] == 0x3D;
        if (has_oled && !oled_bus) {
            oled_bus = keep;
            snprintf(line, sizeof line, "SDA%d SCL%d", pairs[i][0], pairs[i][1]);
            ESP_LOGI(TAG, "OLED found on SDA=%d SCL=%d%s", pairs[i][0], pairs[i][1],
                     n > 1 ? " (with other devices: CO2 sensor at 0x2A?)" : "");
        } else if (keep) i2c_del_master_bus(keep);
    }

    ssd1306_handle_t oled = NULL;
    if (oled_bus) {
        ssd1306_config_t cfg = SSD1306_72X40;
        if (ssd1306_init(oled_bus, &cfg, &oled) == ESP_OK) {
            char fl[16];
            snprintf(fl, sizeof fl, "flash %luMB", (unsigned long)(flash >> 20));
            ssd1306_text(oled, 0, 0, "PROBE OK");
            ssd1306_text(oled, 0, 10, line);
            ssd1306_text(oled, 0, 20, fl);
            ssd1306_flush(oled);
        }
    }

    gpio_config_t led = {.pin_bit_mask = 1ULL << 8, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_config_t btn = {.pin_bit_mask = 1ULL << 9, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&btn);
    int presses = 0;
    bool was = false;
    for (int t = 0;; t++) {
        gpio_set_level(8, (t / 5) % 2);
        bool pressed = gpio_get_level(9) == 0;
        if (pressed && !was) {
            presses++;
            ESP_LOGI(TAG, "BOOT button pressed (%d)", presses);
            if (oled) {
                char b[24];
                snprintf(b, sizeof b, "button %d", presses);
                ssd1306_fill_rect(oled, 0, 30, 72, 10, false);
                ssd1306_text(oled, 0, 30, b);
                ssd1306_flush(oled);
            }
        }
        was = pressed;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
