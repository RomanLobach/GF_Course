idf_component_register(
    SRCS "buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)