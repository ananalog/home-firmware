#include "ssd1306.h"

#include <stdlib.h>
#include <string.h>

#define PAGES_MAX 8
#define WIDTH_MAX 128

struct ssd1306 {
    i2c_master_dev_handle_t dev;
    ssd1306_config_t cfg;
    uint8_t buf[PAGES_MAX][WIDTH_MAX];
};

static esp_err_t cmds(ssd1306_handle_t d, const uint8_t *c, size_t n)
{
    uint8_t tmp[32];
    if (n + 1 > sizeof tmp) return ESP_ERR_INVALID_SIZE;
    tmp[0] = 0x00;  // control byte: commands
    memcpy(tmp + 1, c, n);
    return i2c_master_transmit(d->dev, tmp, n + 1, 100);
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg, ssd1306_handle_t *out)
{
    if (i2c_master_probe(bus, cfg->address, 50) != ESP_OK) return ESP_ERR_NOT_FOUND;
    ssd1306_handle_t d = calloc(1, sizeof *d);
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *cfg;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = 100000,  // the bus is shared with the CO2 sensor (max 100 kHz)
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &d->dev);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    // Same sequence as u8g2's SSD1306 72x40 ER driver.
    const uint8_t init[] = {
        0xAE,              // display off
        0xD5, 0x80,        // clock divide
        0xA8, (uint8_t)(cfg->height - 1),  // multiplex
        0xD3, 0x00,        // display offset
        0x40,              // start line 0
        0x8D, 0x14,        // charge pump on
        0x20, 0x00,        // horizontal addressing
        0xA1,              // segment remap
        0xC8,              // COM scan reversed
        0xDA, 0x12,        // COM pins
        0x81, 0x7F,        // contrast
        0xD9, 0xF1,        // pre-charge
        0xDB, 0x20,        // VCOMH
        0xAD, 0x30,        // internal IREF (72x40 panels)
        0xA4, 0xA6,        // resume RAM, normal
        0xAF,              // display on
    };
    err = cmds(d, init, sizeof init);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    ssd1306_clear(d);
    ssd1306_flush(d);
    *out = d;
    return ESP_OK;
}

uint8_t ssd1306_width(ssd1306_handle_t d) { return d->cfg.width; }
uint8_t ssd1306_height(ssd1306_handle_t d) { return d->cfg.height; }

void ssd1306_clear(ssd1306_handle_t d) { memset(d->buf, 0, sizeof d->buf); }

void ssd1306_pixel(ssd1306_handle_t d, int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= d->cfg.width || y >= d->cfg.height) return;
    if (on) d->buf[y / 8][x] |= (uint8_t)(1 << (y % 8));
    else d->buf[y / 8][x] &= (uint8_t)~(1 << (y % 8));
}

void ssd1306_fill_rect(ssd1306_handle_t d, int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) ssd1306_pixel(d, i, j, on);
}

void ssd1306_hline(ssd1306_handle_t d, int x, int y, int w) { ssd1306_fill_rect(d, x, y, w, 1, true); }

void ssd1306_invert_rect(ssd1306_handle_t d, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if (i < 0 || j < 0 || i >= d->cfg.width || j >= d->cfg.height) continue;
            d->buf[j / 8][i] ^= (uint8_t)(1 << (j % 8));
        }
}

static void blit_column(ssd1306_handle_t d, int x, int y, uint8_t bits, int rows)
{
    for (int r = 0; r < rows && r < 8; r++)
        if (bits & (1 << r)) ssd1306_pixel(d, x, y + r, true);
}

int ssd1306_text(ssd1306_handle_t d, int x, int y, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        for (int col = 0; col < 5; col++) blit_column(d, x + col, y, ssd1306_font5x8[c - 32][col], 8);
        x += 6;
    }
    return x;
}

int ssd1306_text_width(const char *s) { return (int)strlen(s) * 6; }

static int big_index(char c)
{
    const char *p = strchr(ssd1306_big_glyphs, c);
    return p ? (int)(p - ssd1306_big_glyphs) : -1;
}

int ssd1306_big(ssd1306_handle_t d, int x, int y, const char *s)
{
    for (; *s; s++) {
        int i = big_index(*s);
        if (i < 0) continue;
        int w = ssd1306_big_width[i];
        const uint8_t *g = ssd1306_big_data[i];
        for (int col = 0; col < w; col++)
            for (int p = 0; p < 3; p++) blit_column(d, x + col, y + p * 8, g[col * 3 + p], 8);
        x += w;
    }
    return x;
}

int ssd1306_big_width_of(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int i = big_index(*s);
        if (i >= 0) w += ssd1306_big_width[i];
    }
    return w;
}

esp_err_t ssd1306_flush(ssd1306_handle_t d)
{
    uint8_t pages = (uint8_t)((d->cfg.height + 7) / 8);
    const uint8_t win[] = {
        0x21, d->cfg.x_offset, (uint8_t)(d->cfg.x_offset + d->cfg.width - 1),  // column range
        0x22, 0, (uint8_t)(pages - 1),                                          // page range
    };
    esp_err_t err = cmds(d, win, sizeof win);
    if (err != ESP_OK) return err;
    uint8_t tmp[1 + WIDTH_MAX];
    tmp[0] = 0x40;  // control byte: data
    for (int p = 0; p < pages; p++) {
        memcpy(tmp + 1, d->buf[p], d->cfg.width);
        err = i2c_master_transmit(d->dev, tmp, d->cfg.width + 1, 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t ssd1306_contrast(ssd1306_handle_t d, uint8_t level)
{
    const uint8_t c[] = {0x81, level};
    return cmds(d, c, sizeof c);
}

esp_err_t ssd1306_power(ssd1306_handle_t d, bool on)
{
    const uint8_t c[] = {(uint8_t)(on ? 0xAF : 0xAE)};
    return cmds(d, c, sizeof c);
}
