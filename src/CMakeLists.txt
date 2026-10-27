idf_component_register(
    SRCS "lumen_iot_core.c" "lumen_iot_wifi.c" "lumen_iot_net.c" "lumen_iot_mqtt.c" "lumen_iot_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash driver esp_http_client mbedtls mqtt log LumenProtocol json esp_timer
)