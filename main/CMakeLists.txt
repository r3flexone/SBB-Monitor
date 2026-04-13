idf_component_register(SRCS "main.c" "sbb.c" "cJSON.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd esp_wifi esp_http_client
                             esp_event nvs_flash)