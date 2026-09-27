#include "acd1200.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct acd1200 {
    i2c_master_dev_handle_t dev;
};

uint8_t acd1200_crc8(const uint8_t *data, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

esp_err_t acd1200_init(i2c_master_bus_handle_t bus, acd1200_handle_t *out)
{
    if (i2c_master_probe(bus, ACD1200_ADDR, 50) != ESP_OK) return ESP_ERR_NOT_FOUND;
    acd1200_handle_t h = calloc(1, sizeof *h);
    if (!h) return ESP_ERR_NO_MEM;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACD1200_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    *out = h;
    return ESP_OK;
}

static esp_err_t cmd(acd1200_handle_t h, const uint8_t *c, size_t n) { return i2c_master_transmit(h->dev, c, n, 100); }

static esp_err_t req(acd1200_handle_t h, const uint8_t *c, size_t n, uint8_t *resp, size_t rn, int wait_ms)
{
    esp_err_t err = cmd(h, c, n);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    return i2c_master_receive(h->dev, resp, rn, 100);
}

esp_err_t acd1200_read(acd1200_handle_t h, acd1200_reading_t *out)
{
    const uint8_t c[] = {0x03, 0x00};
    uint8_t r[9];
    esp_err_t err = req(h, c, sizeof c, r, sizeof r, 80);
    if (err != ESP_OK) return err;
    if (acd1200_crc8(r, 2) != r[2] || acd1200_crc8(r + 3, 2) != r[5] || acd1200_crc8(r + 6, 2) != r[8])
        return ESP_ERR_INVALID_CRC;
    out->ppm = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[3] << 8) | r[4];
    out->temp_raw = (int16_t)((r[6] << 8) | r[7]);
    return ESP_OK;
}

esp_err_t acd1200_set_auto_calibration(acd1200_handle_t h, bool on)
{
    uint8_t c[5] = {0x53, 0x06, 0x00, (uint8_t)(on ? 1 : 0), 0};
    c[4] = acd1200_crc8(c + 2, 2);
    return cmd(h, c, sizeof c);
}

esp_err_t acd1200_get_auto_calibration(acd1200_handle_t h, bool *on)
{
    const uint8_t c[] = {0x53, 0x06};
    uint8_t r[3];
    esp_err_t err = req(h, c, sizeof c, r, sizeof r, 10);
    if (err != ESP_OK) return err;
    if (acd1200_crc8(r, 2) != r[2]) return ESP_ERR_INVALID_CRC;
    *on = r[1] != 0;
    return ESP_OK;
}

esp_err_t acd1200_calibrate(acd1200_handle_t h, uint16_t ppm)
{
    if (ppm < 400 || ppm > 5000) return ESP_ERR_INVALID_ARG;
    uint8_t c[5] = {0x52, 0x04, (uint8_t)(ppm >> 8), (uint8_t)ppm, 0};
    c[4] = acd1200_crc8(c + 2, 2);
    return cmd(h, c, sizeof c);
}

esp_err_t acd1200_factory_reset(acd1200_handle_t h)
{
    const uint8_t c[] = {0x52, 0x02, 0x00};
    return cmd(h, c, sizeof c);
}

static esp_err_t read_text(acd1200_handle_t h, uint8_t a, uint8_t b, char out[11])
{
    const uint8_t c[] = {a, b};
    uint8_t r[10];
    esp_err_t err = req(h, c, sizeof c, r, sizeof r, 10);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 10; i++) out[i] = (r[i] >= 32 && r[i] < 127) ? (char)r[i] : 0;
    out[10] = 0;
    return ESP_OK;
}

esp_err_t acd1200_version(acd1200_handle_t h, char out[11]) { return read_text(h, 0xD1, 0x00, out); }
esp_err_t acd1200_serial(acd1200_handle_t h, char out[11]) { return read_text(h, 0xD2, 0x01, out); }
