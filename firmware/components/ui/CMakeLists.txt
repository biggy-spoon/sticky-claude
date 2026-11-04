idf_component_register(
    SRCS "ui.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl board epaper_display esp_timer heap
)
