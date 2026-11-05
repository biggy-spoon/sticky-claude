idf_component_register(
    SRCS "device_nav.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json settings_service ui
)
