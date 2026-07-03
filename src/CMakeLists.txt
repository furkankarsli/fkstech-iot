# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# Explicitly compile main.c and local cJSON.c for the ESP32 (ESP-IDF) environment
idf_component_register(SRCS "main.c" "cJSON.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client esp_http_server esp_wifi nvs_flash esp_netif)
