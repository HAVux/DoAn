idf_component_register(SRCS "wifi.c"
                       PRIV_REQUIRES driver spiffs esp_wifi esp_http_server nvs_flash esp_http_client mbedtls
                       INCLUDE_DIRS "." "../attendance_device")