idf_component_register(
    SRCS         "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES     settings web_api net_wifi ota_update usb_target serial_bridge uf2_flasher ch32_flasher status_led battery esp_timer
)
