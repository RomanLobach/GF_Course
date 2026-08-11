idf_component_register(
    SRCS "sx1276.c"
    INCLUDE_DIRS "include"
    REQUIRES bitbang_spi driver esp_timer
)