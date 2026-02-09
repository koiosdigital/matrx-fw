file(GLOB_RECURSE NESTED_SRC
       **.cpp
)

idf_component_register(
    SRCS ${NESTED_SRC}
    INCLUDE_DIRS "." "hw_defs" "display" "webp_player" "sockets" "sprites" "daughterboard" "config" "scheduler" "auto_brightness"
    REQUIRES esp_wifi heap esp-hub75 libwebp protobufs kd_common matrx_resources network_provisioning
)