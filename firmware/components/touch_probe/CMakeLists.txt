idf_component_register(
    SRCS "touch_probe.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_i2c esp_timer
)
