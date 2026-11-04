idf_component_register(
    SRCS "captive_portal.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server lwip settings_service wifi_service
)
