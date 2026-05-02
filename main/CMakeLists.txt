idf_component_register(SRCS "main.c" "sbb.c" "cJSON.c"
                            "http_server.c" "nvs_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd esp_wifi esp_http_client
                             esp_event nvs_flash
                             esp_http_server spiffs mdns)

spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
