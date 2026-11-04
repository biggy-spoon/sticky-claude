idf_component_register(
    SRCS "main.cpp"
    REQUIRES nvs_flash board button_service pdm_mic recording_service
             wifi_service assistant_service epaper_display settings_service
             captive_portal
)
