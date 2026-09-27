idf_component_register(SRCS "src/ssd1306.c" "src/fonts.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c)
