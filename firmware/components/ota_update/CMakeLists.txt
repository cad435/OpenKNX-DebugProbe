idf_component_register(
    SRCS         "OtaUpdater.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     web_api app_update
    PRIV_REQUIRES esp_http_server bootloader_support
)
