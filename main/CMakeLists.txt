idf_component_register(SRCS "main.c" "sbb.c" "cJSON.c"
                            "http_server.c" "nvs_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_wifi esp_http_client
                             esp_event nvs_flash mbedtls
                             esp_http_server spiffs)

spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
