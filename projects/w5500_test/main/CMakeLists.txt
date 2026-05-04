idf_component_register(
    SRCS "w5500_test.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_eth
        esp_netif
        esp_event
        nvs_flash
        esp_driver_gpio
        esp_driver_spi
        espressif__w5500
        lwip
)