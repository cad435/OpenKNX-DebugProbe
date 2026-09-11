idf_component_register(
    SRCS         "WebServer.cpp" "AssetStore.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     esp_http_server joltwallet__littlefs esp_partition
)
# web/fallback.html wird von tools/embed_web.py nach
# include/generated/fallback_html.hpp uebersetzt (EMBED_TXTFILES geht mit
# PlatformIO nicht, siehe platformio.ini).
#
# Die eigentliche Oberflaeche liegt NICHT mehr hier, sondern in firmware/data/
# und wird als LittleFS-Image in die Partition `storage` geschrieben — siehe
# tools/fs_image.py und AssetStore.
