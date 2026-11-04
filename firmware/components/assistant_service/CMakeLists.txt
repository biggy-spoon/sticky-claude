idf_component_register(
    SRCS "assistant_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json settings_service
)
