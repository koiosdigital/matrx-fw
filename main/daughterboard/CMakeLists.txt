idf_component_register(
    SRCS "daughterboard.cpp"
    INCLUDE_DIRS "."
    REQUIRES "driver" "esp_event" "freertos"
)
