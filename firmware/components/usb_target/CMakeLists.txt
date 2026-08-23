idf_component_register(
    SRCS         "UsbTarget.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     usb esp_timer
                 espressif__usb_host_cdc_acm
                 espressif__usb_host_vcp
                 espressif__usb_host_ch34x_vcp
                 espressif__usb_host_cp210x_vcp
                 espressif__usb_host_ftdi_vcp
                 espressif__usb_host_msc
)
