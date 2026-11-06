idf_component_register(
    SRCS "device_nav.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer json settings_service ui
)
