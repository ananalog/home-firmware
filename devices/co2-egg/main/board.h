// ESP32-C3 "Egg" (SuperMini with 0.42" OLED): pins verified from the board photo, OLED pins per
// the common 72x40 variant — check with devices/board-probe if the screen stays dark.
#pragma once

#define BOARD_I2C_SDA 5      // OLED and (through a level shifter) ACD1200
#define BOARD_I2C_SCL 6
#define BOARD_I2C_HZ 100000  // ACD1200 max 100 kHz
#define BOARD_LED_GPIO 8     // blue LED "IO8", active low
#define BOARD_LED_ACTIVE_LOW 1
#define BOARD_BUTTON_GPIO 9  // BOOT button, active low
#define BOARD_OLED_ADDR 0x3C
