idf_component_register(
    SRCS         "WiFiManager.cpp" "CaptiveDns.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     esp_wifi esp_netif esp_event esp_timer settings web_api
    PRIV_REQUIRES lwip esp_hw_support
)
# web/portal.html wird von tools/embed_web.py nach
# include/generated/portal_html.hpp uebersetzt.
