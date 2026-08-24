idf_component_register(
    SRCS         "StatusLed.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     driver esp_driver_gpio
)
