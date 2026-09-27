// Host tests for pure logic of the firmware drivers (no ESP-IDF needed): gcc + run.
#include <stdint.h>
#include <stdio.h>

uint8_t acd1200_crc8(const uint8_t *data, int n);

int main(void)
{
    int fails = 0;
    // CRC-8/0x31/init 0xFF (Sensirion-style): known value for {0xBE, 0xEF} is 0x92.
    const uint8_t v[] = {0xBE, 0xEF};
    if (acd1200_crc8(v, 2) != 0x92) {
        printf("FAIL crc8(BE EF) = %02X\n", acd1200_crc8(v, 2));
        fails++;
    }
    // Calibration command body for 420 ppm: 0x01 0xA4 → CRC as the sensor expects.
    const uint8_t c[] = {0x01, 0xA4};
    printf("crc8(01 A4) = %02X\n", acd1200_crc8(c, 2));
    printf(fails ? "FAILED\n" : "ok\n");
    return fails;
}
