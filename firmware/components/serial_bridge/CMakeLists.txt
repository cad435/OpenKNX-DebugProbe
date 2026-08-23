idf_component_register(
    SRCS          "SerialBridge.cpp" "RingBuffer.cpp" "Rfc2217Server.cpp"
    INCLUDE_DIRS  "include"
    REQUIRES      usb_target
    PRIV_REQUIRES lwip esp_hw_support
)
