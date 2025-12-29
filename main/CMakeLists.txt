file(GLOB_RECURSE NESTED_SRC
       **.cpp
)

idf_component_register(
    SRCS ${NESTED_SRC}
    INCLUDE_DIRS "." "hw_defs" "display" "webp_player" "sockets" "sprites" "daughterboard" "config" "scheduler"
    REQUIRES esp_wifi heap ESP32-HUB75-MatrixPanel-DMA libwebp json bootloader_support protobufs kd_common esp_http_client wifi_provisioning matrx_resources
)

idf_build_set_property(COMPILE_OPTIONS "-DDEVICE_NAME_PREFIX=\"MATRX\"" APPEND)