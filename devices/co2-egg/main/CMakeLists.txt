idf_component_register(SRCS "main.c" "sensor.c" "ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES home_core ssd1306 acd1200 esp_driver_gpio esp_driver_i2c esp_timer)
