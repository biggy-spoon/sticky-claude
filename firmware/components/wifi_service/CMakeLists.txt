idf_component_register(
    SRCS "wifi_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event
)
