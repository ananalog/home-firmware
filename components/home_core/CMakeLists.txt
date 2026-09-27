idf_component_register(
    SRCS "src/core.c" "src/config.c" "src/net.c" "src/link.c" "src/ble.c" "src/dispatch.c"
         "src/points.c" "src/ota.c" "src/button.c" "src/logfwd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES home_proto esp_wifi esp_netif esp_event nvs_flash
    PRIV_REQUIRES app_update esp_app_format esp_timer bt mbedtls esp_driver_gpio lwip esp_partition)
# home_image_desc (HOME_IMAGE_DESC in the device's main.c) is not referenced by code:
# keep it from being garbage-collected so it lands right after esp_app_desc_t.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u home_image_desc")
