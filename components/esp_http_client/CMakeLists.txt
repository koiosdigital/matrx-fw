if(NOT ${IDF_TARGET} STREQUAL "linux")
    set(req lwip esp_event)
else()
    set(req linux esp_event)
endif()

idf_component_register(SRCS "esp_http_client.c"
                            "lib/http_auth.c"
                            "lib/http_header.c"
                            "lib/http_utils.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "lib/include"
                    # lwip is a public requirement because esp_http_client.h includes sys/socket.h
                    REQUIRES ${req}
                    PRIV_REQUIRES tcp_transport http_parser)

# KOIOS: this vendored copy backports the v6.0.2 esp_http_client_connect()
# keep-alive fix. The define lets dependents (kd_http.c) compile out their
# v6.0.0 close-per-request workaround while ESP_IDF_VERSION still says 6.0.0.
target_compile_definitions(${COMPONENT_LIB} INTERFACE KD_HTTP_CLIENT_CONNECT_FIX_BACKPORTED=1)
