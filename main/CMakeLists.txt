file(GLOB_RECURSE NESTED_SRC
       **.cpp
)

idf_component_register(
    SRCS ${NESTED_SRC}
    INCLUDE_DIRS "." "hw_defs" "display" "sockets" "sprites" "scheduler"
    REQUIRES esp_wifi heap ESP32-HUB75-MatrixPanel-DMA libwebp json qrcode bootloader_support MATRX-protobufs kd_common esp_http_client wifi_provisioning
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=parentheses" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-write-strings" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DDEVICE_NAME_PREFIX=\"MATRX\"" APPEND)