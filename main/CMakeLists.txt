file(GLOB_RECURSE NESTED_SRC
       **.cpp
)

idf_component_register(
    SRCS ${NESTED_SRC}
    INCLUDE_DIRS "." "crypto" "provisioning" "wifi" "display" "sockets" "sprites" "ota" "scheduler"
    REQUIRES bt esp_wifi nvs_flash wifi_provisioning heap ESP32-HUB75-MatrixPanel-DMA libwebp efuse esp_http_client json qrcode esp_https_ota bootloader_support MATRX-protobufs
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=parentheses" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-write-strings" APPEND)