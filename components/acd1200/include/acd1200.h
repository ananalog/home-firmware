// ASAIR (Aosong) ACD1200 NDIR CO2 sensor over I2C (address 0x2A, max 100 kHz).
// Command set of the ACD family (ACD10/ACD3100/ACD1200): CRC-8 poly 0x31 init 0xFF over every 2 bytes.
// NOTE: the sensor pulls SDA/SCL up to 5 V — use a level shifter with ESP32-C3.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define ACD1200_ADDR 0x2A
#define ACD1200_PREHEAT_MS 120000
#define ACD1200_MIN_INTERVAL_MS 2000

typedef struct acd1200 *acd1200_handle_t;

typedef struct {
    uint32_t ppm;
    int16_t temp_raw;   // internal sensor temperature as reported (sensor-specific scale)
} acd1200_reading_t;

esp_err_t acd1200_init(i2c_master_bus_handle_t bus, acd1200_handle_t *out);
esp_err_t acd1200_read(acd1200_handle_t h, acd1200_reading_t *out);   // blocks ~80 ms
esp_err_t acd1200_set_auto_calibration(acd1200_handle_t h, bool on);
esp_err_t acd1200_get_auto_calibration(acd1200_handle_t h, bool *on);
esp_err_t acd1200_calibrate(acd1200_handle_t h, uint16_t target_ppm); // 400..5000
esp_err_t acd1200_factory_reset(acd1200_handle_t h);
esp_err_t acd1200_version(acd1200_handle_t h, char out[11]);
esp_err_t acd1200_serial(acd1200_handle_t h, char out[11]);
uint8_t acd1200_crc8(const uint8_t *data, int n);
