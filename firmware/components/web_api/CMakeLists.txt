idf_component_register(
    SRCS         "WebServer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     esp_http_server
)
# web/index.html wird von tools/embed_web.py nach
# include/generated/index_html.hpp uebersetzt (EMBED_TXTFILES geht mit
# PlatformIO nicht, siehe platformio.ini).
