set(srcs "main.c" "wifi_station.c" "discord_bot.c" "misha_bot.c")
set(requires spi_flash esp_wifi esp_netif esp_event nvs_flash lwip esp_websocket_client json esp-tls mbedtls esp_http_client driver)

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "llm8.c")
    list(APPEND requires spiffs)
endif()

idf_component_register(SRCS "${srcs}"
                       PRIV_REQUIRES "${requires}"
                       INCLUDE_DIRS "")

if(IDF_TARGET STREQUAL "esp32s3")
    spiffs_create_partition_image(storage ../spiffs_data FLASH_IN_PROJECT)
endif()
