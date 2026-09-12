idf_component_register(
    SRCS         "B003Link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     usb_target
)
# Das Protokoll von cnlohrs rv003usb-Bootloader, portiert aus pgm-b003fun.c
# des ch32fun-Projekts. Siehe B003Link.hpp fuer die Begruendung.
