idf_component_register(
    SRCS "wifi_tester.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip
)