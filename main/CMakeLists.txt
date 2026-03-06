idf_component_register(SRCS "main.c" "wifi_station.c" "discord_bot.c"
                       PRIV_REQUIRES spi_flash esp_wifi esp_netif esp_event nvs_flash lwip esp_websocket_client json esp-tls mbedtls esp_http_client driver
                       INCLUDE_DIRS "")
