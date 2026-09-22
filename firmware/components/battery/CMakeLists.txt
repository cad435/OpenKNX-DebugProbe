idf_component_register(
    SRCS         "BatteryMonitor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     esp_adc settings esp_timer
)
