file(GLOB_RECURSE NESTED_SRC
       **.cpp
)

idf_component_register(
    SRCS ${NESTED_SRC}
    INCLUDE_DIRS "." "display" "webp_player" "sockets" "sprites" "daughterboard" "config" "scheduler"
    REQUIRES esp_wifi heap hub75 libwebp protobufs kd_common matrx_resources network_provisioning esp_driver_i2c
)